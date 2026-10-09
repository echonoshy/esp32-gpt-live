#!/usr/bin/env python3
"""Check the actual firmware WebSocket coalescer against RFC 6455 framing."""
import ctypes as c
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ws-send-check-') as td:
    temp = Path(td)
    (temp/'esp_transport.h').write_text('''#pragma once
typedef void *esp_transport_handle_t;
int esp_transport_poll_write(esp_transport_handle_t, int);
int esp_transport_write(esp_transport_handle_t, const char *, int, int);
''')
    (temp/'esp_transport_ws.h').write_text('typedef int ws_transport_opcodes_t;\n')
    (temp/'esp_random.h').write_text('#include <stddef.h>\nvoid esp_fill_random(void *, size_t);\n')
    (temp/'mock.c').write_text('''#include <stdint.h>
#include <string.h>
unsigned char captured[1208];
int count, captured_size, partial, writable = 1;
void esp_fill_random(void *p, size_t n) { memset(p, 0x5a, n); }
int esp_transport_poll_write(void *t, int ms) { (void)t; (void)ms; return writable; }
int esp_transport_write(void *t, const char *p, int n, int ms) {
    (void)t; (void)ms; count++; captured_size=n; memcpy(captured,p,n); return n-partial;
}
int __real_esp_transport_ws_send_raw(void *t, int op, const char *p, int n, int ms) {
    (void)t; (void)op; (void)p; (void)n; (void)ms; return -99;
}
''')
    shared = str(temp/'ws.so')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-shared', '-fPIC', '-O2', '-I'+str(temp), str(root/'firmware/main/ws_send.c'),
                    str(temp/'mock.c'), '-o', shared], check=True)
    lib = c.CDLL(shared)
    lib.ws_send_bind.argtypes = [c.c_void_p, c.c_void_p]
    lib.ws_send_bind(1, 2)
    send = lib.__wrap_esp_transport_ws_send_raw
    send.argtypes = [c.c_void_p, c.c_int, c.c_char_p, c.c_int, c.c_int]
    count = c.c_int.in_dll(lib, 'count')
    size = c.c_int.in_dll(lib, 'captured_size')
    capture = (c.c_ubyte*1208).in_dll(lib, 'captured')
    for op, n in [(0x89, 0), (0x8a, 6), (0x88, 2), (0x81, 125), (0x82, 126), (0x82, 244), (0x82, 976), (0x82, 1200), (0x02, 244), (0x80, 244)]:
        payload = bytes(i % 256 for i in range(n))
        before = count.value
        assert send(1, op, payload, n, 1000) == n
        assert count.value == before+1
        raw = bytes(capture[:size.value])
        assert raw[0] == op and raw[1] & 0x80
        header = 2 if n <= 125 else 4
        assert (raw[1] & 127 if header == 2 else int.from_bytes(raw[2:4], 'big')) == n
        mask = raw[header:header+4]
        assert bytes(v ^ mask[i % 4] for i, v in enumerate(raw[header+4:])) == payload
    c.c_int.in_dll(lib, 'partial').value = 1
    assert send(1, 0x82, b'abc', 3, 1000) == -1
    c.c_int.in_dll(lib, 'writable').value = 0
    before = count.value
    assert send(1, 0x82, b'abc', 3, 1000) == 0
    assert count.value == before
    assert send(3, 0x82, b'abc', 3, 1000) == -99
print('PASS: one masked write per frame, short/extended lengths, control/fragment frames, partial failure, fallback')
