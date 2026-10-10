#!/usr/bin/env python3
"""Exercise the actual firmware sender: framing, backpressure, partial writes, deadlines."""
import ctypes as c
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ws-send-check-') as td:
    temp = Path(td)
    for folder in ('lwip', 'freertos'):
        (temp/folder).mkdir()
    (temp/'esp_transport.h').write_text('''#pragma once
typedef void *esp_transport_handle_t;
int esp_transport_get_socket(esp_transport_handle_t);
''')
    (temp/'esp_transport_ws.h').write_text('typedef int ws_transport_opcodes_t;\n')
    (temp/'esp_random.h').write_text('#include <stddef.h>\nvoid esp_fill_random(void *, size_t);\n')
    (temp/'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
    (temp/'esp_log.h').write_text('void mock_log(const char *, const char *, ...);\n#define ESP_LOGW mock_log\n')
    (temp/'lwip/sockets.h').write_text('''#include <sys/socket.h>
#define send mock_send
int mock_send(int, const void *, unsigned, int);
''')
    (temp/'freertos/FreeRTOS.h').write_text('#define pdMS_TO_TICKS(x) (x)\n')
    (temp/'freertos/task.h').write_text('void vTaskDelay(unsigned);\n')
    (temp/'mock.c').write_text('''#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
unsigned char captured[1208];
int count, captured_size, chunk, fail_after, release_ms, interrupt_once;
int64_t now_us;
void reset(void) { count=captured_size=chunk=release_ms=interrupt_once=0; fail_after=-1; now_us=0; }
void esp_fill_random(void *p, size_t n) { memset(p, 0x5a, n); }
int esp_transport_get_socket(void *t) { (void)t; return 7; }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ms) { now_us += ms*1000; }
void mock_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int mock_send(int fd, const void *p, unsigned n, int flags) {
    count++;
    if(fd!=7 || !(flags & MSG_DONTWAIT)) { errno=EINVAL; return -1; }
    if(interrupt_once) { interrupt_once=0; errno=EINTR; return -1; }
    if(now_us < (int64_t)release_ms*1000) { errno=EAGAIN; return -1; }
    if(fail_after>=0 && captured_size>=fail_after) { errno=ECONNRESET; return -1; }
    if(chunk>0 && n>(unsigned)chunk) n=chunk;
    memcpy(captured+captured_size,p,n); captured_size+=n; return n;
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
    def var(name): return c.c_int.in_dll(lib,name)
    count, size = var('count'), var('captured_size')
    capture = (c.c_ubyte*1208).in_dll(lib, 'captured')
    clock = c.c_int64.in_dll(lib,'now_us')
    for op, n in [(0x89, 0), (0x8a, 6), (0x88, 2), (0x81, 125), (0x82, 126), (0x82, 244), (0x82, 976), (0x82, 1200), (0x02, 244), (0x80, 244)]:
        for chunk in (0, 7, 127):
            lib.reset();var('chunk').value=chunk
            payload = bytes(i % 256 for i in range(n))
            assert send(1, op, payload, n, 1000) == n
            raw = bytes(capture[:size.value])
            assert raw[0] == op and raw[1] & 0x80
            header = 2 if n <= 125 else 4
            assert (raw[1] & 127 if header == 2 else int.from_bytes(raw[2:4], 'big')) == n
            mask = raw[header:header+4]
            assert bytes(v ^ mask[i % 4] for i, v in enumerate(raw[header+4:])) == payload
            if not chunk: assert count.value == 1
    # Recover from a temporary full send buffer without duplicating a partial frame.
    lib.reset();var('release_ms').value=450;var('chunk').value=2
    assert send(1,0x82,b'abc',3,500)==3 and size.value==9 and clock.value==450000
    lib.reset();var('interrupt_once').value=1
    assert send(1,0x82,b'abc',3,500)==3 and size.value==9
    # One deadline, not a fresh timeout for each partial write/poll.
    lib.reset();var('release_ms').value=501
    assert send(1,0x82,b'abc',3,500)==-1 and size.value==0 and clock.value==500000
    lib.reset();var('release_ms').value=1
    assert send(1,0x82,b'abc',3,0)==-1 and clock.value==0
    lib.reset();var('chunk').value=2;var('fail_after').value=4
    assert send(1,0x82,b'abc',3,500)==-1 and size.value==4
    assert send(3,0x82,b'abc',3,1000)==-99
print('PASS: RFC6455 data/control/fragment framing; partial-write continuation; EAGAIN/EINTR recovery; bounded deadline; fatal failure; fallback')
