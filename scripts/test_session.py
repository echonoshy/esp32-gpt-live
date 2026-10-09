#!/usr/bin/env python3
"""Offline process-level regression. Fake Codex/media peers never contact OpenAI.
Run: cargo build --manifest-path server/Cargo.toml && .tools/python/bin/python scripts/test_session.py
"""
import asyncio
import contextlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

import websockets

ROOT = Path(__file__).resolve().parents[1]


def log(kind, **values):
    with open(os.environ["PASSPORT_TEST_LOG"], "a") as f:
        f.write(json.dumps({"kind": kind, **values}) + "\n")


def codex():
    for line in sys.stdin:
        m = json.loads(line)
        method, params = m["method"], m.get("params", {})
        if "id" not in m:
            continue
        result = {}
        if method == "account/read":
            result = {"account": {"type": "chatgpt", "planType": "test"}}
        elif method == "thread/start":
            result = {"thread": {"id": str(uuid.uuid4())}}
        elif method == "thread/realtime/start":
            log("session", thread=params["threadId"], voice=params["voice"], prompt=params["prompt"])
            time.sleep(0.4)  # Buffer microphone input through startup.
        print(json.dumps({"id": m["id"], "result": result}), flush=True)
        if method == "thread/realtime/start":
            print(json.dumps({"method": "thread/realtime/sdp", "params": {"sdp": "offline"}}), flush=True)


async def worker():
    url = next(x for x in sys.argv if x.startswith("http://"))
    origin, ident = url.split("/media#")
    async with websockets.connect(origin.replace("http:", "ws:") + "/worker?id=" + ident) as ws:
        await ws.send(json.dumps({"type": "offer", "sdp": "offline"}))
        enabled = False
        ready = False
        played = 0
        frames = 0
        input_at = 0.0
        async def tick():
            nonlocal played
            while True:
                await asyncio.sleep(0.02)
                if ready:
                    voiced = frames >= 8 and played < 12 and time.monotonic() - input_at > 0.35
                    if voiced:
                        played += 1
                    await ws.send(struct.pack("<h", 1500 if voiced else 0) * 480)
        task = asyncio.create_task(tick())
        try:
            async for m in ws:
                if isinstance(m, bytes):
                    log("audio", enabled=enabled, size=len(m), worker=ident)
                    if enabled:
                        frames += 1
                        if frames == 1:
                            input_at = time.monotonic()
                            await ws.send(json.dumps({"type": "user_started"}))
                        elif frames == 16:
                            await ws.send(json.dumps({"type": "user_started"}))
                else:
                    e = json.loads(m)
                    if e["type"] == "answer":
                        ready = True
                        await ws.send(json.dumps({"type": "ready"}))
                    if e["type"] == "input":
                        enabled = e["enabled"]
                        log("input", enabled=enabled, worker=ident)
                        if e.get("clear"):
                            frames = 0
                            played = 0
        finally:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError, websockets.ConnectionClosed):
                await task


async def check():
    with tempfile.TemporaryDirectory(prefix="passport-offline-") as td:
        temp = Path(td)
        logpath = temp / "events.jsonl"
        env = dict(os.environ, PASSPORT_TEST_LOG=str(logpath), DEVICE_TOKEN="", RUST_LOG="error")
        for name in ("codex", "worker"):
            script = temp / name
            import shlex
            script.write_text("#!/bin/sh\nexec " + shlex.quote(sys.executable) + " " + shlex.quote(str(Path(__file__).resolve())) + " --" + name + ' "$@"\n')
            script.chmod(0o700)
            env["CODEX_BIN" if name == "codex" else "CHROMIUM_BIN"] = str(script)
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        env["LISTEN"] = f"127.0.0.1:{port}"
        process = subprocess.Popen([str(ROOT / "target/debug/passport-voice")], env=env, stdout=subprocess.DEVNULL)
        def events(kind):
            data = logpath.read_text() if logpath.exists() else ""
            return [e for e in map(json.loads, data.splitlines()) if e["kind"] == kind]
        try:
            for _ in range(100):
                try:
                    ws = await websockets.connect(f"ws://127.0.0.1:{port}/device")
                    break
                except OSError:
                    await asyncio.sleep(0.03)
            else:
                raise AssertionError("offline server did not start")
            async with ws:
                messages = []
                async def receive():
                    async for m in ws:
                        messages.append(json.loads(m) if isinstance(m, str) else m)
                reader = asyncio.create_task(receive())
                async def send(kind, **kw):
                    await ws.send(json.dumps({"type": kind, **kw}))
                async def until(predicate):
                    async with asyncio.timeout(5):
                        while not predicate():
                            await asyncio.sleep(0.02)
                await send("start", protocol=3, mode="ptt", voice="juniper", language=1)
                await send("ptt_start")
                for _ in range(15):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: any(isinstance(m, dict) and m.get("state") == "ready" for m in messages))
                await asyncio.sleep(0.3)
                assert not events("audio"), "audio reached model while button held"
                await send("ptt_end")
                await until(lambda: len(events("audio")) == 15)
                assert all(e["enabled"] and e["size"] == 960 for e in events("audio"))
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                assert all(len(m) == 964 for m in messages if isinstance(m, bytes))
                assert events("session")[0]["voice"] == "juniper"
                assert "自然英语" in events("session")[0]["prompt"]
                # New chat requires a stop acknowledgement and a fresh Codex thread.
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                messages.clear()
                await send("start", protocol=3, mode="continuous", duplex=1, voice="cove")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "listen" for m in messages))
                sessions = events("session")
                assert len(sessions) == 2 and sessions[0]["thread"] != sessions[1]["thread"]
                for _ in range(10):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "playback_begin" for m in messages))
                count = len(events("audio"))
                for _ in range(6):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: len(events("audio")) == count + 6)
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "clear" for m in messages))
                await send("pause")
                await asyncio.sleep(0.1)
                count = len(events("audio"))
                await ws.send(b"\x00" * 960)
                await asyncio.sleep(0.1)
                assert len(events("audio")) == count, "menu did not pause microphone"
                await send("resume")
                await ws.send(b"\x00" * 960)
                await until(lambda: len(events("audio")) > count)
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                # Already-ended session must still acknowledge a new-chat stop.
                messages.clear()
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                # A short utterance released before media-ready must also survive startup.
                messages.clear()
                count = len(events("audio"))
                await send("start", protocol=3, mode="ptt", voice="maple")
                await send("ptt_start")
                for _ in range(10):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await send("ptt_end")
                await until(lambda: len(events("audio")) == count + 10)
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                await ws.close()
                await reader
                print("PASS: startup capture and early release, release-only model input, tagged playback, voice/prompt, fresh conversation, full-duplex barge-in, menu pause/resume, idle stop acknowledgement")
        finally:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    if "--codex" in sys.argv:
        codex()
    elif "--worker" in sys.argv:
        asyncio.run(worker())
    else:
        asyncio.run(check())
