"""Exercise the exact device protocol with a 24 kHz mono PCM WAV.

This consumes subscription voice usage. Credentials are read from .env, never
printed. Audio and transcripts stay under the ignored artifacts directory.
"""
import argparse
import array
import asyncio
import json
from pathlib import Path
import time
import wave
import websockets


async def probe(args):
    config = dict(line.split("=", 1) for line in Path(args.env).read_text().splitlines()
                  if line and not line.startswith("#") and "=" in line)
    with wave.open(args.wav, "rb") as source:
        assert (source.getframerate(), source.getnchannels(), source.getsampwidth()) == (24000, 1, 2)
        pcm = bytes(14400) + source.readframes(source.getnframes()) + bytes(19200)
    folder = Path(args.out)
    folder.mkdir(parents=True, exist_ok=False)
    events, audio = [], bytearray()
    started = time.monotonic()
    ready = asyncio.Event()
    ended = asyncio.Event()
    failed = asyncio.Event()
    async with websockets.connect(args.url, proxy=None, additional_headers={"Authorization": "Bearer " + config["DEVICE_TOKEN"]}, max_queue=64) as ws:
        async def receive():
            async for message in ws:
                if isinstance(message, bytes):
                    assert len(message) == 960
                    audio.extend(message)
                else:
                    event = json.loads(message)
                    events.append({"at": round(time.monotonic() - started, 3), **event})
                    print(json.dumps(events[-1], ensure_ascii=False), flush=True)
                    if event.get("state") == "ready": ready.set()
                    if event.get("state") == "idle" and ready.is_set(): ended.set()
                    if event.get("type") == "error": failed.set(); ready.set()
        reader = asyncio.create_task(receive())
        try:
            await ws.send(json.dumps({"type": "start"}))
            await asyncio.wait_for(ready.wait(), 90)
            if failed.is_set(): raise RuntimeError("voice setup failed")
            await ws.send(json.dumps({"type": "ptt_start"}))
            speech_started = time.monotonic()
            for offset in range(0, len(pcm), 960):
                await ws.send(pcm[offset:offset + 960].ljust(960, b"\0"))
                await asyncio.sleep(max(0, speech_started + (offset + 960) / 48000 - time.monotonic()))
            await ws.send(json.dumps({"type": "ptt_end"}))
            await asyncio.sleep(args.wait)
            await ws.send(json.dumps({"type": "stop"}))
            await asyncio.wait_for(ended.wait(), 10)
        finally:
            reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)
            with wave.open(str(folder / "reply.wav"), "wb") as out:
                out.setnchannels(1); out.setsampwidth(2); out.setframerate(24000)
                out.writeframes(audio)
            samples = array.array("h", audio)
            report = {"events": events, "peak": max(map(abs, samples), default=0),
                      "audio_seconds": len(audio) / 48000, "non_silent_samples": sum(abs(s) > 150 for s in samples)}
            (folder / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
            print(json.dumps({k:v for k,v in report.items() if k != "events"}), flush=True)
    if failed.is_set() or report["non_silent_samples"] < 2400:
        raise SystemExit("FAIL: no useful reply audio")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("wav")
    parser.add_argument("--url", default="ws://140.143.229.103:8787/device")
    parser.add_argument("--env", default=".env")
    parser.add_argument("--out", required=True)
    parser.add_argument("--wait", type=float, default=20)
    asyncio.run(probe(parser.parse_args()))
