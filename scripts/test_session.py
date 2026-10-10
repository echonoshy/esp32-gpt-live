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
    log("codex_options", args=sys.argv)
    for line in sys.stdin:
        m = json.loads(line)
        method, params = m["method"], m.get("params", {})
        if "id" not in m:
            continue
        result = {}
        if method == "account/read":
            result = {"account": {"type": "chatgpt", "planType": "test"}}
        elif method == "thread/start":
            assert params.get("environments") != [], "empty environments disables native tools"
            log("thread_config", base=params.get("baseInstructions"), developer=params.get("developerInstructions"),
                cwd=params["cwd"], permissions=params.get("permissions"), approval=params["approvalPolicy"])
            result = {"thread": {"id": str(uuid.uuid4())}}
        elif method == "thread/realtime/appendText":
            log("interrupt", thread=params["threadId"])
        elif method == "thread/realtime/start":
            log("session", thread=params["threadId"], voice=params["voice"], prompt=params["prompt"],
                context=params["initialItems"], handoffs=params["clientManagedHandoffs"])
            time.sleep(0.4)  # Buffer microphone input through startup.
        print(json.dumps({"id": m["id"], "result": result}), flush=True)
        if method == "thread/realtime/start":
            print(json.dumps({"method": "thread/realtime/sdp", "params": {"sdp": "offline"}}), flush=True)


async def worker():
    url = next(x for x in sys.argv if x.startswith("http://"))
    from urllib.parse import urlsplit, parse_qs
    parsed = urlsplit(url)
    origin, ident = f"{parsed.scheme}://{parsed.netloc}", parsed.fragment
    voice = parse_qs(parsed.query)['voice'][0]
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
                        await ws.send(json.dumps({"type": "ready", "voice": voice, "voice_ack": True}))
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
        context_dir = temp / "context"
        context_dir.mkdir()
        env = dict(os.environ, PASSPORT_TEST_LOG=str(logpath), DEVICE_TOKEN="", RUST_LOG="error",
                   VOICE_CONTEXT_DIR=str(context_dir))
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
                # Old firmware may still send these fields; none may impose conversation style.
                await send("start", protocol=3, mode="ptt", voice="juniper", language=1, pace=2, reply_length=1)
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
                prompt = events("session")[0]["prompt"]
                assert all(s not in prompt for s in ("普通话", "自然英语", "语速", "一句话", "简短回答"))
                config = events("thread_config")[0]
                assert config["base"] == config["developer"] and "python3" in config["base"]
                assert config["permissions"] == "mic-on" and config["approval"] == "never"
                assert Path(config["cwd"]) == context_dir.resolve()
                options = events("codex_options")[0]["args"]
                assert 'web_search="live"' in options and 'features.shell_tool=true' in options
                assert 'permissions.mic-on.network.enabled=false' in options
                assert 'shell_environment_policy.inherit="none"' in options
                assert events("session")[0]["context"] == []
                assert events("session")[0]["handoffs"] is False
                # New chat requires a stop acknowledgement and a fresh Codex thread.
                await send("stop")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "stopped" for m in messages))
                saved_context = "测试上下文：项目叫 mic-on。"
                (context_dir / "CONTEXT.md").write_text(saved_context)
                messages.clear()
                await send("start", protocol=3, mode="continuous", duplex=0, voice="cove")
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "listen" for m in messages))
                sessions = events("session")
                assert len(sessions) == 2 and sessions[0]["thread"] != sessions[1]["thread"]
                assert sessions[0]["prompt"] == sessions[1]["prompt"], "retired style fields changed instructions"
                assert sessions[1]["voice"] == "cove"
                assert saved_context in sessions[1]["context"][0]["text"]
                assert events("thread_config")[1]["cwd"] == config["cwd"]
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
                # Recover 2.4 seconds of TCP backlog at once: never overflow the
                # small media bridge, and never dump a burst into the AudioWorklet.
                count = len(events("audio"))
                for _ in range(120):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: len(events("audio")) == count + 120)
                burst = events("audio")[count:]
                assert burst[-1]["at"] - burst[0]["at"] > 2.0, "LIVE backlog was not paced"
                assert not any(isinstance(m, dict) and m.get("type") == "error" for m in messages), "LIVE backlog disconnected session"
                await until(lambda: any(isinstance(m, dict) and m.get("type") == "playback_begin" for m in messages))
                count = len(events("audio"))
                for _ in range(6):
                    await ws.send(struct.pack("<h", 1200) * 480)
                await until(lambda: len(events("audio")) == count + 6)
                messages.clear()
                count = len(events("audio"))
                await send("ptt_start")
                await until(lambda: any(isinstance(m, dict) and m.get('type') == 'clear' for m in messages))
                # Old epoch packets may already be on the wire before clear. The
                # new epoch, which firmware admits, must stay silent while held.
                boundary = next(i for i, m in enumerate(messages) if isinstance(m, dict) and m.get('type') == 'clear')
                del messages[:boundary + 1]
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
                # Protocol 7 splits batched uplink into ordered 20 ms PCM frames.
                messages.clear()
                count = len(events('audio'))
                await send('start', protocol=7, mode='ptt')
                await until(lambda: any(isinstance(m, dict) and m.get('state') == 'ready' for m in messages))
                await send('ptt_start')
                for frames in (4, 4, 3):
                    await ws.send(packet * frames)
                await send('ptt_end')
                await until(lambda: len(events('audio')) == count + 11)
                assert all(e['enabled'] and e['size'] == 960 for e in events('audio')[count:])
                await until(lambda: any(isinstance(m, bytes) for m in messages))
                assert all(len(m) == 248 for m in messages if isinstance(m, bytes))
                await send('stop')
                await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                print('PASS: protocol 7 batches and final 3-frame tail arrive before release; older protocols remain supported')
                # Home cancels an unfinished recording (no ptt_end) and pauses live input.
                # Run once during startup too: queued navigation must not reopen the mic.
                for initial_mode in ('ptt', 'continuous'):
                    messages.clear()
                    await send('start', protocol=7, mode=initial_mode)
                    if initial_mode == 'ptt':
                        await send('ptt_start')
                        await ws.send(packet * 4)
                    await send('pause')
                    await send('cancel')
                    await until(lambda: any(isinstance(m, dict) and m.get('type') == 'clear' for m in messages))
                    await asyncio.sleep(0.25)
                    assert not events('input')[-1]['enabled'], 'home reopened microphone during startup'
                    count = len(events('audio'))
                    session_count = len(events('session'))
                    messages.clear()
                    for _ in range(4):
                        await ws.send(packet * 4)  # Late frames after the user's back gesture.
                    await asyncio.sleep(0.5)
                    assert len(events('audio')) == count, 'home forwarded cancelled recording'
                    assert not any(isinstance(m, bytes) for m in messages), 'home leaked old reply audio'
                    assert not any(isinstance(m, dict) and m.get('type') == 'submitted' for m in messages)
                    # Either home entry can resume the existing thread, repeatedly.
                    for target_mode in ('continuous', 'ptt', 'continuous'):
                        messages.clear()
                        await send('mode', mode=target_mode)
                        await until(lambda: any(isinstance(m, dict) and m.get('mode') == target_mode for m in messages))
                        if target_mode == 'ptt':
                            await send('ptt_start')
                        count = len(events('audio'))
                        for _ in range(3):
                            await ws.send(packet * 4)
                        await until(lambda: len(events('audio')) == count + 12)
                        assert all(e['enabled'] for e in events('audio')[count:])
                        assert len(events('session')) == session_count, 'home navigation reset conversation'
                        if target_mode == 'ptt':
                            await send('ptt_end')
                        await send('pause')
                        await send('cancel')
                        await asyncio.sleep(0.15)
                        assert not events('input')[-1]['enabled']
                    messages.clear()
                    await send('stop')
                    await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                print('PASS: home cancels unsent speech, stays silent during startup, resumes both modes without resetting context')
                # Voice changes use the same stop/stopped/start boundary in both input modes.
                for mode in ('ptt', 'continuous'):
                    previous_thread = None
                    for voice in ('cove', 'breeze'):
                        messages.clear()
                        await send('start', protocol=7, mode=mode, voice=voice)
                        await until(lambda: any(isinstance(m, dict) and m.get('state') == 'ready' for m in messages))
                        acknowledged = next(m for m in messages if isinstance(m, dict) and m.get('state') == 'ready')
                        assert acknowledged.get('voice') == voice, 'device did not receive confirmed voice'
                        current = events('session')[-1]
                        assert current['voice'] == voice and current['thread'] != previous_thread
                        previous_thread = current['thread']
                        if mode == 'continuous':
                            await until(lambda: any(isinstance(m, dict) and m.get('type') == 'listen' for m in messages))
                        else:
                            assert not any(isinstance(m, dict) and m.get('type') == 'listen' for m in messages)
                        await send('stop')
                        await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                print('PASS: new voice reaches a fresh conversation while preserving PTT or continuous input mode')
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
                # Protocol 8 coalesces downlink frames, bounded by four packets;
                # controls flush a partial tail before playback_end/stopped.
                messages.clear()
                await send('probe', protocol=8)
                await until(lambda: sum(len(m)//248 for m in messages if isinstance(m, bytes)) >= 12)
                await send('stop')
                await until(lambda: any(isinstance(m, dict) and m.get('type') == 'stopped' for m in messages))
                packets = [m for m in messages if isinstance(m, bytes)]
                assert any(len(m) > 248 for m in packets), 'downlink never batched'
                assert all(len(m) in (248, 496, 744, 992) for m in packets)
                assert all(m[i:i+4] == b'\x01\x00\x00\x00' for m in packets for i in range(0, len(m), 248))
                assert len(events('session')) == sessions_before_probe
                print('PASS: LIVE 2.4 second backlog is paced without disconnect; protocol 8 downlink batches retain independent epochs')
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
