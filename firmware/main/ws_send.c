#include "ws_send.h"
#include "esp_random.h"
#include "esp_transport_ws.h"
#include <stdint.h>
#include <errno.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    // select() uses lwIP's send low-water threshold, not the size of this frame.
    // Try nonblocking send first; a healthy high-RTT socket may accept this small
    // frame even while select reports non-writable. Never restart a partial frame.
    int socket = esp_transport_get_socket(voice_tcp);
    int64_t began = esp_timer_get_time();
    int64_t deadline = timeout_ms < 0 ? INT64_MAX : began + (int64_t)timeout_ms * 1000;
    unsigned written = 0;
    int error = 0;
    while (written < total) {
        int n = send(socket, wire + written, total - written, MSG_DONTWAIT);
        if (n > 0) {
            written += n;
            continue;
        }
        error = n == 0 ? EPIPE : errno;
        if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR)
            break;
        if (esp_timer_get_time() >= deadline) {
            error = ETIMEDOUT;
            break;
        }
        // A short sleep retries available space below select's low-water mark,
        // yields to RX/Wi-Fi, and shares one deadline across all partial writes.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    int64_t elapsed = (esp_timer_get_time() - began) / 1000;
    if (elapsed > 80 || written != total)
        ESP_LOGW("ws_send", "write opcode=%d bytes=%d elapsed_ms=%lld sent=%u/%u errno=%d",
                 (int)opcode, len, elapsed, written, total, written == total ? 0 : error);
    if (written == total)
        return len;
    errno = error;
    return -1; // Only a real failure or exhausted deadline aborts the WS connection.
}
