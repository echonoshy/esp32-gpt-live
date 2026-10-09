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
        elif method == "thread/realtime/appendText":
            log("interrupt", thread=params["threadId"])
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
                        if played == 0:
                            await ws.send(json.dumps({"type":"transcript_delta","role":"assistant","text":"reply"}))
                        played += 1
                    if voiced:
                        log("output", worker=ident, sample=1500 + played)
                    await ws.send(struct.pack("<h", 1500 + played if voiced else 0) * 480)
                    if voiced and played == 12:
                        await ws.send(json.dumps({"type":"transcript","role":"assistant","text":"reply"}))
        task = asyncio.create_task(tick())
        try:
            async for m in ws:
                if isinstance(m, bytes):
                    log("audio", enabled=enabled, size=len(m), worker=ident, at=time.monotonic())
                    if enabled:
                        frames += 1
                        if frames == 1:
                            input_at = time.monotonic()
                            await ws.send(json.dumps({"type":"transcript_delta","role":"user","text":"hello"}))
                        elif frames == 16:
                            await ws.send(json.dumps({"type":"transcript","role":"user","text":"hello"}))
                else:
                    e = json.loads(m)
                    if e["type"] == "answer":
                        ready = True
                        await ws.send(json.dumps({"type": "ready"}))
                    if e["type"] == "input":
                        enabled = e["enabled"]
                        log("input", enabled=enabled, worker=ident)
                        if e.get("clear"):
                            if frames:
                                for role in ("user","assistant"):
                                    await ws.send(json.dumps({"type":"transcript","role":role,"text":"old tail"}))
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
                await send("start", protocol=3, mode="continuous", duplex=0, voice="cove")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "listen" for m in messages))
                sessions = events("session")
                assert len(sessions) == 2 and sessions[0]["thread"] != sessions[1]["thread"]
                count=len(events("audio"))
                for _ in range(10):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda:len(events("audio"))==count+10)
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "playback_begin" for m in messages))
                count = len(events("audio"))
                for _ in range(6):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: len(events("audio")) == count + 6)
                await asyncio.sleep(0.25)
                assert not any(isinstance(m,dict) and m.get("type")=="clear" for m in messages), "natural speech forced local interruption"
                assert not events("interrupt"), "natural speech injected a stop instruction"
                sessions_before_cancel=len(events("session"))
                await send("cancel")
                await until(lambda:any(isinstance(m,dict) and m.get("type")=="clear" for m in messages))
                await asyncio.sleep(0.15)
                messages.clear()
                await asyncio.sleep(0.15)
                assert not any(isinstance(m,bytes) for m in messages), "canceled audio escaped"
                # A fresh user/assistant exchange resumes without an arbitrary quiet gate.
                for _ in range(16):
                    await ws.send(struct.pack("<h",1200)*480)
                    await asyncio.sleep(0.02)
                await until(lambda:any(isinstance(m,bytes) for m in messages))
                assert len(events("session"))==sessions_before_cancel, "cancel discarded context"
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
                # Protocol 4 streams input during the hold, but preserves early output for release.
                messages.clear()
                await send("start", protocol=4, mode="ptt", duplex=1)
                await until(lambda: any(isinstance(m, dict) and m.get("state") == "ready" for m in messages))
                session_count = len(events("session"))
                count = len(events("audio"))
                output_count = len(events("output"))
                await send("ptt_start")
                for _ in range(40):
                    await ws.send(struct.pack("<h", 1200) * 480)
                    await asyncio.sleep(0.02)
                await until(lambda: len(events("audio")) == count + 40)
                await until(lambda: len(events("output")) == output_count + 12)
                assert not any(isinstance(m, bytes) for m in messages), "early reply escaped while OK held"
                released = time.monotonic()
                await send("ptt_end")
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                first_audio_ms = round((time.monotonic() - released) * 1000)
                assert first_audio_ms < 400, f"buffered reply waited {first_audio_ms}ms"
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "playback_end" for m in messages))
                samples = [struct.unpack_from("<h", m, 4)[0] for m in messages if isinstance(m, bytes)]
                assert [v for v in samples if v] == list(range(1501, 1513)), "reply prefix/tail lost or reordered"
                end = next(m for m in messages if isinstance(m, dict) and m.get("type") == "playback_end")
                await send("playback_drained", epoch=end["epoch"])
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "waiting" and m.get("epoch") == end["epoch"] for m in messages))
                # Explicit PTT cancel clears queued output, keeping the Codex thread.
                await send("ptt_start")
                for _ in range(16):
                    await ws.send(struct.pack("<h",1200)*480)
                    await asyncio.sleep(0.02)
                await send("ptt_end")
                await send("cancel")
                await asyncio.sleep(0.15)
                messages.clear()
                await asyncio.sleep(0.5)
                assert not any(isinstance(m,bytes) for m in messages), "PTT cancellation leaked output"
                # Quick-menu switching and live -> hold use the same upstream conversation.
                messages.clear()
                await send("mode", mode="continuous")
                await until(lambda: any(isinstance(m, dict) and m.get("mode") == "continuous" for m in messages))
                count = len(events("audio"))
                for _ in range(10):
                    await ws.send(struct.pack("<h", 1200) * 480)
                    await asyncio.sleep(0.02)
                await until(lambda: len(events("audio")) == count + 10)
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "playback_begin" for m in messages))
                count = len(events("audio"))
                for _ in range(6):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: len(events("audio")) == count + 6)
                messages.clear()
                count = len(events("audio"))
                await send("ptt_start")
                for _ in range(20):
                    await ws.send(struct.pack("<h", 1200) * 480)
                    await asyncio.sleep(0.02)
                await until(lambda: len(events("audio")) == count + 20)
                assert not any(isinstance(m, bytes) for m in messages)
                await send("ptt_end")
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                await send("mode", mode="ptt")
                await until(lambda: any(isinstance(m, dict) and m.get("mode") == "ptt" for m in messages))
                assert len(events("session")) == session_count, "mode change reset context"
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                # Switching during startup must retain more than the legacy 32-frame live queue.
                messages.clear()
                count = len(events("audio"))
                await send("start", protocol=4, mode="continuous")
                await send("ptt_start")
                for _ in range(60):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await send("ptt_end")
                await until(lambda: len(events("audio")) == count + 60)
                assert all(e["enabled"] for e in events("audio")[count:])
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                # Compressed wire format still reaches the media worker as 960-byte PCM.
                vectors = json.loads((ROOT / 'scripts/fixtures/adpcm.json').read_text())
                packet = bytes.fromhex(vectors[1]['encoded'])
                messages.clear()
                count = len(events('audio'))
                await send('start', protocol=5, mode='ptt')
                await until(lambda: any(isinstance(m, dict) and m.get('state') == 'ready' for m in messages))
                await send('ptt_start')
                for _ in range(24):
                    await ws.send(packet)
                    await asyncio.sleep(0.02)
                await until(lambda: len(events('audio')) == count + 24)
                assert not any(isinstance(m, bytes) for m in messages)
                assert all(e['enabled'] and e['size'] == 960 for e in events('audio')[count:])
                await send('ptt_end')
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                assert all(len(m) == 248 and m[6] <= 88 and m[7] == 0 for m in messages if isinstance(m, bytes))
                await send('stop')
                await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                # A silent probe must never create a Codex or media session.
                sessions_before_probe = len(events('session'))
                messages.clear()
                await send('probe', protocol=5)
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                for _ in range(10):
                    await ws.send(bytes(244))
                    await asyncio.sleep(0.02)
                assert len(events('session')) == sessions_before_probe
                assert all(len(m) == 248 for m in messages if isinstance(m, bytes))
                await send('stop')
                await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                # Late microphone frames after shutdown must not close the bridge.
                for _ in range(8):
                    await ws.send(bytes(244))
                messages.clear()
                await send('probe', protocol=5)
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                await send('stop')
                await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                await ws.close()
                await reader
                print("PASS: startup capture and early release, release-only model input, tagged playback, voice/prompt, fresh conversation, continuous duplex without automatic stop prompts, explicit cancellation, menu pause/resume, idle stop acknowledgement")
                print(f"PASS: streaming PTT input, held-output gate, all 12 reply frames preserved, buffered first audio {first_audio_ms} ms (offline only), mode switch preserves context, live-to-PTT startup queue")
                print('PASS: compressed 244/248-byte wire with PCM media compatibility, silent transport probe creates no upstream session')
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
