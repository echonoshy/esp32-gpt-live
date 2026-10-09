#include "ws_send.h"
#include "esp_random.h"
#include "esp_transport_ws.h"
#include <stdint.h>

static esp_transport_handle_t voice_ws, voice_tcp;
// Fixed workspace, protected by esp_websocket_client's TX mutex. No per-frame heap.
static uint8_t wire[1208];

void ws_send_bind(esp_transport_handle_t websocket, esp_transport_handle_t parent)
{
    voice_ws = websocket;
    voice_tcp = parent;
}

int __real_esp_transport_ws_send_raw(esp_transport_handle_t t, ws_transport_opcodes_t opcode,
                                     const char *data, int len, int timeout_ms);

// IDF 5.5 writes the WS header and payload separately. With TCP_NODELAY that consumes
// two queued TCP segments per 20 ms frame; lwIP's writable low-water mark stalls the
// microphone on a high-RTT link. Keep both in one masked RFC 6455 frame/write.
int __wrap_esp_transport_ws_send_raw(esp_transport_handle_t t, ws_transport_opcodes_t opcode,
                                     const char *data, int len, int timeout_ms)
{
    if (t != voice_ws || !voice_tcp || len < 0 || len > 1200 || (len && !data))
        return __real_esp_transport_ws_send_raw(t, opcode, data, len, timeout_ms);
    int writable = esp_transport_poll_write(voice_tcp, timeout_ms);
    if (writable <= 0)
        return writable;
    unsigned header = 2;
    wire[0] = (uint8_t)opcode;
    if (len <= 125)
        wire[1] = 0x80 | len;
    else {
        wire[1] = 0xfe;
        wire[2] = (unsigned)len >> 8;
        wire[3] = len & 255;
        header = 4;
    }
    esp_fill_random(wire + header, 4);
    for (int i = 0; i < len; i++)
        wire[header + 4 + i] = (uint8_t)data[i] ^ wire[header + (i % 4)];
    unsigned total = header + 4 + len;
    int sent = esp_transport_write(voice_tcp, (const char *)wire, total, timeout_ms);
    // A partial frame must close the connection, never be retried as a new WS frame.
    return sent == (int)total ? len : -1;
}
