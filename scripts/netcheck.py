#!/usr/bin/env python3
"""Silent authenticated transport probe; never starts Codex or transmits microphone audio."""
import argparse
import json
import time
import urllib.request
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--url', default='http://192.168.7.17')
p.add_argument('--out', default='artifacts/diagnostics/netcheck.json')
a = p.parse_args()

def info():
    with urllib.request.urlopen(a.url + '/api/info', timeout=3) as r:
        return json.load(r)

before = info()
nonce = before.pop('nonce')
print('BEFORE', json.dumps(before), flush=True)
request = urllib.request.Request(a.url + '/api/netcheck', data=b'', method='POST',
                                 headers={'X-Device-Nonce': nonce})
with urllib.request.urlopen(request, timeout=3) as r:
    print(r.read().decode(), flush=True)
samples = []
started = time.monotonic()
while time.monotonic() - started < 24:
    time.sleep(1)
    try:
        d = info()
        d.pop('nonce', None)
        d['elapsed_s'] = round(time.monotonic() - started, 1)
        samples.append(d)
        q = d.get('diagnostics', {})
        print(d['elapsed_s'], d['state'], 'free', q.get('heap_free'), 'largest', q.get('heap_largest'),
              'tx', q.get('tx_frames'), 'play', q.get('speaker_frames'), 'fail', q.get('alloc_failures'),
              'send_ms', q.get('send_max_ms'), 'refills', q.get('playback_refills'), flush=True)
        if (not q.get('netcheck') and time.monotonic() - started > 15
                and q.get('netcheck_runs', 0) > before.get('diagnostics', {}).get('netcheck_runs', 0)):
            break
    except Exception as e:
        samples.append({'elapsed_s': round(time.monotonic() - started, 1), 'error': str(e)})
        print('READ ERROR', type(e).__name__, flush=True)
out = Path(a.out)
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps({'before': before, 'samples': samples}, indent=2))
out.chmod(0o600)
print('Saved', out)

valid = [sample for sample in samples if 'diagnostics' in sample]
if not valid:
    raise SystemExit('FAIL: no device diagnostics returned')
first = before.get('diagnostics', {})
last = valid[-1]['diagnostics']
delta = {key: last.get(key, 0) - first.get(key, 0) for key in
         ('tx_frames', 'speaker_frames', 'dropped_frames', 'playback_refills',
          'send_errors', 'ws_disconnects', 'wifi_disconnects', 'alloc_failures')}
print('DELTA', json.dumps(delta), flush=True)
if delta['tx_frames'] < 400 or delta['speaker_frames'] < 400 or last.get('netcheck'):
    raise SystemExit('FAIL: bidirectional silent probe did not complete; counters are not a valid transport result')
print('COMPLETE: silent bidirectional transport only; actual conversation/listening still required', flush=True)
