#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "bsp_audio.h"
#include "bsp_button.h"
#include "cJSON.h"
#include "device_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "esp_transport_tcp.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "ui.h"
#include "wifi.h"

#include "bsp_battery.h"
#include "esp_ota_ops.h"
#include "settings.h"
#include "portal.h"
#include "firmware_update.h"
#include "device.h"
#include "menu.h"
#include "controls.h"
#include "adpcm.h"
#include "ws_send.h"
#include "audio_transport.h"
#include "playback_gain.h"

#define SAMPLES 480
#define FRAME_BYTES (SAMPLES * sizeof(int16_t))
#define ECHO_GUARD_MS 180
typedef struct {
    uint32_t epoch, received_at;
    uint8_t packet[ADPCM_BYTES];
} audio_frame_t;
_Static_assert(sizeof(audio_frame_t) * PLAYBACK_FRAMES <= 33 * 1024, "playback RAM budget");
typedef enum {
    CMD_CONNECT,
    CMD_OK,
    CMD_STOP,
    CMD_UP,
    CMD_DOWN,
    CMD_WAKE,
    CMD_APPLY,
    CMD_VOICE_CHANGED,
    CMD_VOICE_FAILED,
    CMD_DRAIN,
    CMD_FAULT,
    CMD_MENU_BACK,
    CMD_PTT_BEGIN,
    CMD_PTT_END,
    CMD_NEW_START,
    CMD_NETCHECK,
    CMD_NETCHECK_DONE,
    CMD_CANCEL,
    CMD_VOLUME_SAVE,
    CMD_LIVE,
    CMD_CHAT,
    CMD_VOICE,
    CMD_INPUT_OVERRUN
} command_type_t;
typedef struct {
    command_type_t type;
    uint32_t epoch;
} command_t;
static const char *TAG = "gpt_live";
static esp_websocket_client_handle_t ws;
static esp_transport_handle_t tcp_transport;
typedef struct {
    uint32_t generation;
    uint8_t packet[ADPCM_BYTES];
} capture_frame_t;
#define CAPTURE_FRAMES 48 // 960 ms, 12 KiB. Capture never waits on network I/O.
static QueueHandle_t playback, commands, capture_queue;
static SemaphoreHandle_t capture_lock;
static TaskHandle_t sender_handle;
static atomic_uint capture_generation, session_generation, capture_dropped, capture_overruns;
static atomic_bool recovering;

static SemaphoreHandle_t send_lock;
static atomic_bool connected, ready, recording, starting, wanted, awaiting_clear, end_pending,
    sticky_error;
static atomic_bool boot_complete, warmup_pending;
static atomic_bool settings_open;
static bool menu_notice;
static atomic_bool interaction_active;
static atomic_uint released_at;
static atomic_bool output_timing_pending;
static uplink_batch_t uplink;    // Protected by send_lock, including control boundaries.
static atomic_uint captured_frames;
static atomic_bool menu_active, live_mode, full_duplex, ptt_active, restart_pending, can_listen;
static atomic_uint capture_started;
static atomic_uint epoch, mic_frames, speaker_frames, dropped_frames, playback_refills;
static atomic_int battery = -1, attempts, active_voice = -1;
static atomic_uint listen_after, retry_at, started_at, ready_at, last_output_at;
static atomic_bool netcheck, netcheck_pending;
static atomic_uint alloc_failures, alloc_failed_size, send_max_ms, send_slow, send_errors;
static atomic_uint mic_read_max_ms, mic_read_errors, receive_gap_max_ms, receive_last_ms;
static atomic_uint netcheck_runs, playback_age_max_ms;
static atomic_uint wifi_disconnects, wifi_last_reason, ws_disconnects, ws_errors;
static atomic_uint playback_target, send_packets;
static atomic_bool serial_trace;
static atomic_uint mic_peak, output_peak;
static atomic_uint speech_interruptions, speech_cleared_frames, speech_clear_at;
static void wifi_diagnostic(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const wifi_event_sta_disconnected_t *event = data;
    atomic_fetch_add(&wifi_disconnects, 1);
    atomic_store(&wifi_last_reason, event->reason);
}
static void allocation_failed(size_t size, uint32_t caps, const char *function)
{
    (void)caps;
    (void)function;
    atomic_fetch_add(&alloc_failures, 1);
    atomic_store(&alloc_failed_size, size);
}
static void maximum(atomic_uint *value, uint32_t candidate)
{
    uint32_t previous = atomic_load(value);
    while (candidate > previous && !atomic_compare_exchange_weak(value, &previous, candidate)) {
    }
}
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static uint32_t microphone_deadline(void)
{
    uint32_t now = now_ms();
    uint32_t elapsed = now - atomic_load(&last_output_at);
    return elapsed >= ECHO_GUARD_MS ? now : now + ECHO_GUARD_MS - elapsed;
}
static void enqueue(command_type_t type, uint32_t e)
{
    command_t c = {type, e};
    xQueueSend(commands, &c, 0);
}
bool device_busy(void)
{
    return atomic_load(&wanted) || atomic_load(&ready) || atomic_load(&starting);
}
int device_battery(void)
{
    return atomic_load(&battery);
}
const char *device_state(void)
{
    if (atomic_load(&netcheck))
        return "NETWORK CHECK";
    if (portal_updating())
        return "UPDATING";
    if (!atomic_load(&connected))
        return "CONNECTING";
    if (atomic_load(&menu_active))
        return "NAVIGATION";
    if (atomic_load(&recording))
        return atomic_load(&live_mode) ? "LISTENING" : "RECORDING";
    if (atomic_load(&starting))
        return "CONNECTING VOICE";
    if (atomic_load(&ready)) {
        if (uxQueueMessagesWaiting(playback) || atomic_load(&end_pending))
            return "SPEAKING";
        return "CHAT ACTIVE";
    }
    return "IDLE";
}
void device_apply_settings(bool voice_changed)
{
    command_t c = {voice_changed ? CMD_VOICE_CHANGED : CMD_APPLY, 0};
    // HTTP settings saves must not silently lose the apply/restart request to a full queue.
    xQueueSend(commands, &c, portMAX_DELAY);
}
void device_netcheck(void)
{
    enqueue(CMD_NETCHECK, 0);
}
void device_add_diagnostics(cJSON *root)
{
    cJSON *j = cJSON_AddObjectToObject(root, "diagnostics");
    if (!j)
        return;
    cJSON_AddStringToObject(j, "codec", "ima-adpcm");
    cJSON_AddNumberToObject(j, "wire_frame_bytes", ADPCM_BYTES);
    cJSON_AddNumberToObject(j, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(j, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(j, "alloc_failures", atomic_load(&alloc_failures));
    cJSON_AddNumberToObject(j, "alloc_failed_size", atomic_load(&alloc_failed_size));
    cJSON_AddNumberToObject(j, "send_max_ms", atomic_load(&send_max_ms));
    cJSON_AddNumberToObject(j, "send_slow", atomic_load(&send_slow));
    cJSON_AddNumberToObject(j, "send_errors", atomic_load(&send_errors));
    cJSON_AddNumberToObject(j, "mic_read_max_ms", atomic_load(&mic_read_max_ms));
    cJSON_AddNumberToObject(j, "mic_read_errors", atomic_load(&mic_read_errors));
    cJSON_AddNumberToObject(j, "receive_gap_max_ms", atomic_load(&receive_gap_max_ms));
    cJSON_AddNumberToObject(j, "tx_frames", atomic_load(&mic_frames));
    cJSON_AddNumberToObject(j, "speaker_frames", atomic_load(&speaker_frames));
    cJSON_AddNumberToObject(j, "dropped_frames", atomic_load(&dropped_frames));
    cJSON_AddNumberToObject(j, "playback_refills", atomic_load(&playback_refills));
    cJSON_AddNumberToObject(j, "playback_age_max_ms", atomic_load(&playback_age_max_ms));
    cJSON_AddNumberToObject(j, "playback_queued", uxQueueMessagesWaiting(playback));
    cJSON_AddNumberToObject(j, "playback_capacity_ms", PLAYBACK_FRAMES * 20);
    cJSON_AddNumberToObject(j, "playback_target_ms", atomic_load(&playback_target) * 20);
    cJSON_AddNumberToObject(j, "send_packets", atomic_load(&send_packets));
    cJSON_AddNumberToObject(j, "capture_queued", uxQueueMessagesWaiting(capture_queue));
    cJSON_AddNumberToObject(j, "capture_dropped", atomic_load(&capture_dropped));
    cJSON_AddNumberToObject(j, "capture_overruns", atomic_load(&capture_overruns));
    cJSON_AddNumberToObject(j, "mic_peak", atomic_load(&mic_peak));
    cJSON_AddNumberToObject(j, "output_peak", atomic_load(&output_peak));
    cJSON_AddNumberToObject(j, "speech_interruptions", atomic_load(&speech_interruptions));
    cJSON_AddNumberToObject(j, "speech_cleared_frames", atomic_load(&speech_cleared_frames));
    cJSON_AddNumberToObject(j, "speech_clear_at", atomic_load(&speech_clear_at));
    cJSON_AddNumberToObject(j, "ws_disconnects", atomic_load(&ws_disconnects));
    cJSON_AddNumberToObject(j, "ws_errors", atomic_load(&ws_errors));
    cJSON_AddNumberToObject(j, "button_mv", bsp_button_read_mv());
    cJSON_AddNumberToObject(j, "button_keys", bsp_button_read_keys());
    cJSON_AddBoolToObject(j, "live", atomic_load(&live_mode));
    cJSON_AddBoolToObject(j, "microphone_active", atomic_load(&recording));
    cJSON_AddBoolToObject(j, "interaction_active", atomic_load(&interaction_active));
    int confirmed_voice = atomic_load(&active_voice);
    cJSON_AddStringToObject(j, "requested_voice", settings_voice(settings_get().voice));
    if (confirmed_voice >= 0)
        cJSON_AddStringToObject(j, "active_voice", settings_voice(confirmed_voice));
    else
        cJSON_AddNullToObject(j, "active_voice");
    cJSON_AddBoolToObject(j, "netcheck", atomic_load(&netcheck));
    cJSON_AddNumberToObject(j, "netcheck_runs", atomic_load(&netcheck_runs));
    cJSON_AddNumberToObject(j, "wifi_disconnects", atomic_load(&wifi_disconnects));
    cJSON_AddNumberToObject(j, "wifi_last_reason", atomic_load(&wifi_last_reason));
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        cJSON_AddNumberToObject(j, "wifi_rssi", ap.rssi);
}
// Call only while holding send_lock. Diagnostics cover each outgoing audio batch.
static bool send_wire(const uint8_t *packets, unsigned frames)
{
    unsigned bytes = frames * ADPCM_BYTES;
    uint32_t before = now_ms();
    int sent = esp_websocket_client_send_bin(ws, (const char *)packets, bytes, pdMS_TO_TICKS(3000));
    uint32_t elapsed = now_ms() - before;
    maximum(&send_max_ms, elapsed);
    if (elapsed > 80)
        atomic_fetch_add(&send_slow, 1);
    if (sent != (int)bytes) {
        atomic_fetch_add(&send_errors, 1);
        ESP_LOGW(TAG, "audio send failed frames=%u elapsed_ms=%lu sent=%d", frames,
                 (unsigned long)elapsed, sent);
        return false;
    }
    atomic_fetch_add(&send_packets, 1);
    atomic_fetch_add(&mic_frames, frames);
    return true;
}
// Holding send_lock serializes dequeue, batching and control boundaries.
static bool flush_capture(void)
{
    capture_frame_t frame;
    while (xQueueReceive(capture_queue, &frame, 0) == pdTRUE) {
        if (frame.generation == atomic_load(&capture_generation) &&
            !uplink_append(&uplink, frame.packet, send_wire))
            return false;
    }
    return true;
}
static void audio_sender_task(void *unused)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        xSemaphoreTake(send_lock, portMAX_DELAY);
        uint32_t generation = atomic_load(&session_generation);
        capture_frame_t frame;
        bool ok = true;
        if (!atomic_load(&connected)) {
            xQueueReset(capture_queue);
            uplink.count = 0;
        }
        if (atomic_load(&connected) && xQueueReceive(capture_queue, &frame, 0) == pdTRUE &&
            frame.generation == atomic_load(&capture_generation))
            ok = uplink_append(&uplink, frame.packet, send_wire);
        bool more = uxQueueMessagesWaiting(capture_queue) != 0;
        xSemaphoreGive(send_lock);
        if (more) xTaskNotifyGive(sender_handle);
        if (!ok) {
            atomic_store(&recording, false);
            // A transport abort already schedules recovery in the WS callback.
            if (atomic_load(&connected))
                enqueue(CMD_FAULT, generation);
        }
    }
}
static bool send_json(const char *message)
{
    if (!atomic_load(&connected))
        return false;
    size_t n = strlen(message);
    xSemaphoreTake(send_lock, portMAX_DELAY);
    // Release must follow the final partial audio batch. Cancellation instead discards it.
    if (!strcmp(message, "{\"type\":\"ptt_end\"}")) {
        if (!flush_capture() || !uplink_flush(&uplink, send_wire)) {
            xSemaphoreGive(send_lock);
            return false;
        }
    } else if (strstr(message, "\"type\":\"stop\"") || strstr(message, "\"type\":\"cancel\"") ||
               strstr(message, "\"type\":\"mode\"") || strstr(message, "\"type\":\"start\"") ||
               strstr(message, "\"type\":\"probe\"") || strstr(message, "\"type\":\"ptt_start\"")) {
        atomic_fetch_add(&capture_generation, 1);
        xQueueReset(capture_queue);
        uplink.count = 0;
    }
    int sent = esp_websocket_client_send_text(ws, message, n, pdMS_TO_TICKS(3000));
    xSemaphoreGive(send_lock);
    return sent >= 0 && (size_t)sent == n;
}
static void mute(void)
{
    atomic_store(&recording, false);
    atomic_store(&end_pending, false);
    atomic_store(&awaiting_clear, false);
    atomic_fetch_add(&epoch, 1);
    xQueueReset(playback);
}
static void idle_ui(void)
{
    if (!portal_updating() && !settings_open) ui_set("READY", "Hold OK: home");
}
static void retry(void)
{
    // Send failure, disconnect and idle can describe the same failure.
    if (atomic_exchange(&recovering, true))
        return;
    atomic_fetch_add(&capture_generation, 1);
    xQueueReset(capture_queue);
    atomic_store(&active_voice, -1);
    atomic_store(&output_timing_pending, false);
    atomic_store(&interaction_active, false);
    mute();
    atomic_store(&ptt_active, false);
    atomic_store(&ready, false);
    atomic_store(&starting, false);
    if (!atomic_load(&wanted))
        return;
    int count = atomic_fetch_add(&attempts, 1) + 1;
    if (count > 3) {
        atomic_store(&wanted, false);
        atomic_store(&sticky_error, true);
        ui_set("CONNECTION LOST", "OK: retry\nHold OK: home");
    } else {
        atomic_store(&retry_at, now_ms() + (2000 << (count - 1)));
        ui_set("RECONNECTING", "Connecting...");
    }
}
static void ws_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        int no_delay = 1;
        int socket = esp_transport_get_socket(tcp_transport);
        if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0)
            ESP_LOGW(TAG, "TCP_NODELAY unavailable");
        atomic_store(&connected, true);
        atomic_store(&warmup_pending, true);
        if (atomic_load(&wanted)) {
            atomic_store(&retry_at, now_ms() + 1500);
            ui_set("RECONNECTING", "Restoring audio");
        } else if (!atomic_load(&sticky_error))
            idle_ui();
        ESP_LOGI(TAG, "device bridge connected");
    } else if (event == WEBSOCKET_EVENT_ERROR) {
        atomic_fetch_add(&ws_errors, 1);
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
        atomic_store(&active_voice, -1);
        atomic_fetch_add(&ws_disconnects, 1);
        ESP_LOGW(TAG, "bridge disconnected send_errors=%u send_max_ms=%u heap=%lu",
                 atomic_load(&send_errors), atomic_load(&send_max_ms),
                 (unsigned long)esp_get_free_heap_size());
        atomic_store(&interaction_active, false);
        atomic_store(&connected, false);
        if (atomic_exchange(&restart_pending, false))
            atomic_store(&wanted, true);
        if (atomic_load(&wanted))
            retry();
        else {
            mute();
            atomic_store(&ready, false);
            atomic_store(&starting, false);
            if (!atomic_load(&sticky_error))
                ui_set("CONNECTING", "Connecting to voice");
        }
    } else if (event == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *frame = data;
        static char payload[1200];
        static size_t received;
        static int opcode;
        if (frame->op_code != 0 && frame->op_code != 1 && frame->op_code != 2)
            return;
        if (frame->payload_offset == 0) {
            received = 0;
            opcode = frame->op_code;
        }
        if (frame->payload_len <= 0 || frame->payload_len >= sizeof(payload) ||
            frame->payload_offset != received || frame->data_len < 0 ||
            received + frame->data_len > sizeof(payload) - 1)
            return;
        memcpy(payload + received, frame->data_ptr, frame->data_len);
        received += frame->data_len;
        if (received != frame->payload_len)
            return;
        if (opcode == 2 && received && received % (ADPCM_BYTES + 4) == 0 &&
            received <= 4 * (ADPCM_BYTES + 4)) {
            uint32_t now = now_ms(), previous = atomic_exchange(&receive_last_ms, now);
            if (previous)
                maximum(&receive_gap_max_ms, now - previous);
            for (size_t offset = 0; offset < received; offset += ADPCM_BYTES + 4) {
                uint32_t e;
                memcpy(&e, payload + offset, 4);
                if ((atomic_load(&menu_active) && !atomic_load(&netcheck)) || atomic_load(&settings_open) ||
                    e != atomic_load(&epoch) || !atomic_load(&ready) ||
                    (atomic_load(&recording) && !atomic_load(&full_duplex)) ||
                    atomic_load(&awaiting_clear))
                    return;
                audio_frame_t compressed = {.epoch = e, .received_at = now};
                const uint8_t *packet = (const uint8_t *)payload + offset + 4;
                if (packet[2] > 88 || packet[3] != 0)
                    return;
                memcpy(compressed.packet, packet, ADPCM_BYTES);
                if (xQueueSend(playback, &compressed, 0) != pdTRUE) {
                    audio_frame_t discarded;
                    xQueueReceive(playback, &discarded, 0);
                    atomic_fetch_add(&dropped_frames, 1);
                    xQueueSend(playback, &compressed, 0);
                }
            }
        } else if (opcode == 1) {
            payload[received] = 0;
            cJSON *m = cJSON_Parse(payload), *t = cJSON_GetObjectItem(m, "type"),
                  *state = cJSON_GetObjectItem(m, "state"), *ev = cJSON_GetObjectItem(m, "epoch");
            const char *type = cJSON_IsString(t) ? t->valuestring : "";
            uint32_t e = cJSON_IsNumber(ev) ? (uint32_t)ev->valuedouble : 0;
            if (atomic_load(&serial_trace))
                ESP_LOGI(TAG, "trace rx %s", payload);
            // Late voice status cannot overwrite update progress or restart controls.
            if (portal_updating()) { cJSON_Delete(m); return; }
            if (!strcmp(type, "status") && cJSON_IsString(state)) {
                ESP_LOGI(TAG, "session %s", state->valuestring);
                if (!strcmp(state->valuestring, "ready") && atomic_load(&wanted)) {
                    if (!atomic_load(&netcheck)) {
                        cJSON *voice = cJSON_GetObjectItem(m, "voice");
                        int expected = settings_get().voice;
                        if (!cJSON_IsString(voice) || strcmp(voice->valuestring, settings_voice(expected))) {
                            atomic_store(&wanted, false);
                            atomic_store(&starting, false);
                            atomic_store(&ready, false);
                            atomic_store(&active_voice, -1);
                            atomic_store(&sticky_error, true);
                            mute();
                            enqueue(CMD_VOICE_FAILED, 0);
                            cJSON_Delete(m);
                            return;
                        }
                        atomic_store(&active_voice, expected);
                        ESP_LOGI(TAG, "voice applied: %s", voice->valuestring);
                    }
                    atomic_store(&ready, true);
                    atomic_store(&starting, false);
                    atomic_store(&ready_at, now_ms());
                    atomic_store(&sticky_error, false);
                } else if (!strcmp(state->valuestring, "connecting") && atomic_load(&wanted) &&
                           !atomic_load(&recording))
                    ui_set("CONNECTING VOICE", "Opening voice session");
                else if (!strcmp(state->valuestring, "idle")) {
                    bool active = atomic_load(&ready) || atomic_load(&starting);
                    mute();
                    atomic_store(&ready, false);
                    atomic_store(&starting, false);
                    atomic_store(&active_voice, -1);
                    if (active && atomic_load(&wanted))
                        retry();
                    else if (!atomic_load(&wanted) && !atomic_load(&sticky_error))
                        idle_ui();
                }
            } else if (!strcmp(type, "stopped")) {
                if (atomic_exchange(&netcheck_pending, false))
                    enqueue(CMD_NETCHECK, 0);
                if (atomic_exchange(&restart_pending, false))
                    enqueue(CMD_NEW_START, 0);
            } else if (!strcmp(type, "listen") && atomic_load(&ready) &&
                       !atomic_load(&awaiting_clear)) {
                atomic_store(&epoch, e);
                atomic_store(&end_pending, false);
                atomic_store(&listen_after,
                             atomic_load(&live_mode) ? now_ms() : microphone_deadline());
                atomic_store(&can_listen, true);
                atomic_store(&recording, atomic_load(&live_mode) && (!atomic_load(&menu_active) || atomic_load(&netcheck)));
                ui_set("LISTENING", "Hold OK: home");
            } else if (!strcmp(type, "playback_begin") && atomic_load(&ready) &&
                       !atomic_load(&awaiting_clear)) {
                atomic_store(&can_listen, atomic_load(&full_duplex));
                atomic_store(&recording, atomic_load(&full_duplex) && (!atomic_load(&menu_active) || atomic_load(&netcheck)));
                atomic_store(&end_pending, false);
                atomic_store(&epoch, e);
                atomic_store(&receive_last_ms, 0);
                if (!atomic_load(&live_mode)) {
                    atomic_store(&interaction_active, true);
                    ui_set("SPEAKING", "Hold OK: home");
                }
            } else if (!strcmp(type, "playback_end") && atomic_load(&ready) &&
                       e == atomic_load(&epoch) && !atomic_load(&awaiting_clear)) {
                atomic_store(&end_pending, true);
            } else if (!strcmp(type, "clear") && atomic_load(&wanted)) {
                cJSON *reason = cJSON_GetObjectItem(m, "reason");
                if (cJSON_IsString(reason) && !strcmp(reason->valuestring, "speech")) {
                    unsigned queued = uxQueueMessagesWaiting(playback);
                    atomic_fetch_add(&speech_interruptions, 1);
                    atomic_fetch_add(&speech_cleared_frames, queued);
                    atomic_store(&speech_clear_at, now_ms());
                    ESP_LOGI(TAG, "speech clear epoch=%lu queued_ms=%u", (unsigned long)e, queued * 20);
                }
                mute();
                atomic_store(&epoch, e);
                atomic_store(&listen_after,
                             atomic_load(&live_mode) ? now_ms() : microphone_deadline());
                bool capture = atomic_load(&live_mode) || atomic_load(&ptt_active);
                atomic_store(&can_listen, capture);
                atomic_store(&recording, capture && (!atomic_load(&menu_active) || atomic_load(&netcheck)));
                if (capture)
                    ui_set(atomic_load(&live_mode) ? "LISTENING" : "RECORDING",
                           atomic_load(&live_mode) ? "Speak freely"
                                                   : "OK: send\nHold OK: home");
            } else if (!strcmp(type, "activity") && atomic_load(&live_mode)) {
                cJSON *active = cJSON_GetObjectItem(m, "active");
                atomic_store(&interaction_active, cJSON_IsTrue(active));
                ui_set(cJSON_IsTrue(active) ? "LIVE CHATTING" : "LIVE LISTENING",
                       "Hold OK: home");
            } else if (!strcmp(type, "waiting") && !atomic_load(&live_mode)) {
                if (!atomic_load(&recording) && !atomic_load(&ptt_active)) {
                    atomic_store(&epoch, e);
                    atomic_store(&interaction_active, false);
                    atomic_store(&awaiting_clear, false);
                    cJSON *too_long = cJSON_GetObjectItem(m, "too_long");
                    ui_set(cJSON_IsTrue(too_long) ? "RECORDING TOO LONG" : "HOLD TO TALK",
                           cJSON_IsTrue(too_long) ? "Input discarded\n60 s maximum"
                                                  : "Hold OK: home");
                }
            } else if (!strcmp(type, "submitted") && !atomic_load(&menu_active)) {
                atomic_store(&interaction_active, true);
                if (!atomic_load(&ptt_active))
                    ui_set("THINKING", "Waiting for response");
            } else if (!strcmp(type, "probe_done")) {
                atomic_store(&wanted, false);
                atomic_store(&recording, false);
                enqueue(CMD_NETCHECK_DONE, 0);
            } else if (!strcmp(type, "error")) {
                cJSON *again = cJSON_GetObjectItem(m, "retryable"),
                      *message = cJSON_GetObjectItem(m, "code");
                if (cJSON_IsTrue(again) && atomic_load(&wanted))
                    retry();
                else {
                    atomic_store(&wanted, false);
                    atomic_store(&ptt_active, false);
                    mute();
                    atomic_store(&ready, false);
                    atomic_store(&starting, false);
                    atomic_store(&sticky_error, true);
                    ui_set("VOICE UNAVAILABLE",
                           cJSON_IsString(message) && !strcmp(message->valuestring, "account")
                               ? "Sign in on server"
                           : cJSON_IsString(message) && !strcmp(message->valuestring, "quota")
                               ? "Voice limit reached"
                               : "OK: retry");
                }
            } else if (!strcmp(type, "ended")) {
                cJSON *reason = cJSON_GetObjectItem(m, "reason");
                atomic_store(&wanted, false);
                atomic_store(&ptt_active, false);
                mute();
                atomic_store(&ready, false);
                atomic_store(&starting, false);
                atomic_store(&sticky_error, true);
                ui_set("CHAT ENDED", cJSON_IsString(reason) && !strcmp(reason->valuestring, "limit")
                                         ? "30 min limit\nOK: new session"
                                         : "Idle timeout\nOK: new session");
            }
            cJSON_Delete(m);
        }
    }
}
static void button(uint8_t keys, void *user)
{
    (void)user;
    static controls_t controls;
    bool navigation =
        atomic_load(&menu_active) || atomic_load(&settings_open) || voice_wifi_provisioning();
    uint32_t actions = controls_update(&controls, keys, now_ms(), navigation);
    static const struct {
        uint32_t bit;
        command_type_t command;
    } mapping[] = {{CONTROL_WAKE, CMD_WAKE}, {CONTROL_UP, CMD_UP},
                   {CONTROL_DOWN, CMD_DOWN}, {CONTROL_SAVE_VOLUME, CMD_VOLUME_SAVE},
                   {CONTROL_SELECT, CMD_OK}, {CONTROL_BACK, CMD_MENU_BACK}};
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++)
        if (actions & mapping[i].bit)
            enqueue(mapping[i].command, 0);
}
static void apply_device_settings(void)
{
    settings_t p = settings_get();
    bsp_audio_set_volume(p.volume);
    ui_battery(device_battery(), p.volume);
    ui_wake();
}
static void show_chat(void)
{
    bool live = atomic_load(&live_mode);
    if (atomic_load(&menu_active)) {
        menu_refresh();
        return;
    }
    ui_mode(live);
    ui_show_guide();
    if (atomic_load(&recording))
        ui_set(live ? "LISTENING" : "RECORDING", "Hold OK: home");
    else if (atomic_load(&ready))
        ui_set(live ? "CHAT ACTIVE" : "HOLD TO TALK",
               live ? "Speak freely" : "OK: start recording");
    else if (atomic_load(&starting))
        ui_set("CONNECTING VOICE", "Connecting...");
    else if (!atomic_load(&connected))
        ui_set("SERVER NOT CONNECTED", "OK: retry\nHold OK: home");
    else if (!atomic_load(&sticky_error))
        idle_ui();
}
static void start_session(void)
{
    if (atomic_load(&menu_active) || atomic_load(&settings_open) || !atomic_load(&wanted) || atomic_load(&starting) ||
        atomic_load(&ready) || atomic_load(&restart_pending))
        return;
    if (!atomic_load(&connected)) {
        ui_set("SERVER NOT CONNECTED", "Hold OK: home");
        return;
    }
    settings_t p = settings_get();
    bool live = atomic_load(&live_mode);
    atomic_store(&full_duplex, live);
    atomic_store(&can_listen, false);
    atomic_store(&recovering, false);
    atomic_fetch_add(&session_generation, 1);
    atomic_store(&starting, true);
    atomic_store(&started_at, now_ms());
    char start[320];
    snprintf(start, sizeof(start),
             "{\"type\":\"start\",\"protocol\":8,\"mode\":\"%s\",\"voice\":\"%s\","
             "\"duplex\":1,\"idle_seconds\":%d}",
             live ? "continuous" : "ptt", settings_voice(p.voice), p.idle_seconds);
    ui_mode(live);
    if (!atomic_load(&menu_active) && !settings_open)
        ui_show_guide();
    if (!send_json(start))
        retry();
    else
        ui_set("CONNECTING VOICE", live ? "Speak when connected" : "OK: start recording");
}
static void stop_session(void)
{
    atomic_store(&active_voice, -1);
    atomic_store(&wanted, false);
    atomic_store(&ptt_active, false);
    atomic_store(&can_listen, false);
    atomic_store(&ready, false);
    atomic_store(&starting, false);
    atomic_store(&sticky_error, false);
    mute();
    atomic_store(&interaction_active, false);
    send_json("{\"type\":\"stop\"}");
}
static void new_conversation(bool voice_changed)
{
    // A preference change invalidates the old session, not the current screen.
    // start_session keeps the replacement deferred until the user opens chat.
    bool keep_settings = voice_changed &&
                         (atomic_load(&menu_active) || atomic_load(&settings_open));
    if (!keep_settings) {
        atomic_store(&menu_active, false);
        atomic_store(&settings_open, false);
        menu_notice = false;
        ui_show_guide();
    }
    // Reuse the stop acknowledgement if another change is already waiting for it.
    if (!atomic_exchange(&restart_pending, true))
        stop_session();
    ui_set(voice_changed ? "APPLYING VOICE" : "NEW CONVERSATION",
           voice_changed ? settings_voice(settings_get().voice) : "Clearing context");
    if (!atomic_load(&connected)) {
        atomic_store(&restart_pending, false);
        enqueue(CMD_NEW_START, 0);
    }
}
static void enter_home(void)
{
    // Close audio admission first, before any network callback can reopen it.
    atomic_store(&menu_active, true);
    atomic_store(&ptt_active, false);
    atomic_store(&interaction_active, false);
    atomic_store(&output_timing_pending, false);
    mute();
    menu_open(atomic_load(&live_mode) ? 1 : 0);
    if (atomic_load(&wanted)) {
        // Pause before cancelling so a continuous session stays silent on home.
        bool ok = send_json("{\"type\":\"pause\"}");
        if (!send_json("{\"type\":\"cancel\"}"))
            ok = false;
        if (!ok)
            retry();
    }
}
static void open_chat(bool live)
{
    atomic_store(&menu_active, false);
    atomic_store(&live_mode, live);
    atomic_store(&full_duplex, live);
    atomic_store(&ptt_active, false);
    atomic_store(&interaction_active, false);
    atomic_store(&output_timing_pending, false);
    mute();
    ui_mode(live);
    ui_show_guide();
    if (atomic_load(&ready) || atomic_load(&starting)) {
        // Mode also resumes the paused session; it preserves the conversation thread.
        atomic_store(&awaiting_clear, true);
        if (!send_json(live ? "{\"type\":\"mode\",\"mode\":\"continuous\"}"
                            : "{\"type\":\"mode\",\"mode\":\"ptt\"}"))
            retry();
    } else {
        atomic_store(&wanted, true);
        atomic_store(&attempts, 0);
        atomic_store(&sticky_error, false);
        start_session();
    }
    show_chat();
}
static void command_task(void *unused)
{
    command_t c;
    for (;;) {
        xQueueReceive(commands, &c, portMAX_DELAY);
        if (c.type != CMD_DRAIN && c.type != CMD_CONNECT)
            ui_wake();
        if (portal_updating() || c.type == CMD_WAKE)
            continue;
        if (c.type == CMD_NETCHECK) {
            if (device_busy() || !atomic_load(&connected))
                continue;
            if (atomic_load(&wanted) || atomic_load(&ready) || atomic_load(&starting)) {
                atomic_store(&netcheck_pending, true);
                stop_session();
                continue;
            }
            xSemaphoreTake(send_lock, portMAX_DELAY);
            atomic_fetch_add(&capture_generation, 1);
            atomic_store(&netcheck, true);
            xSemaphoreGive(send_lock);
            atomic_store(&wanted, true);
            atomic_store(&starting, true);
            atomic_store(&started_at, now_ms());
            atomic_store(&live_mode, true);
            atomic_store(&full_duplex, true);
            atomic_store(&receive_last_ms, 0);
            atomic_fetch_add(&netcheck_runs, 1);
            send_json("{\"type\":\"probe\",\"protocol\":8}");
            continue;
        }
        if (c.type == CMD_NETCHECK_DONE ||
            (atomic_load(&netcheck) && c.type != CMD_DRAIN && c.type != CMD_APPLY)) {
            stop_session();
            xSemaphoreTake(send_lock, portMAX_DELAY);
            atomic_store(&netcheck, false);
            atomic_store(&live_mode, false);
            atomic_store(&full_duplex, false);
            xSemaphoreGive(send_lock);
            show_chat();
            if (c.type != CMD_VOICE_CHANGED)
                continue;
        }
        if (voice_wifi_provisioning()) {
            if (c.type == CMD_OK || c.type == CMD_UP || c.type == CMD_DOWN)
                voice_wifi_setup_next();
            else if (c.type == CMD_MENU_BACK)
                esp_restart();
            continue;
        }
        if (settings_open && (c.type == CMD_OK || c.type == CMD_MENU_BACK || c.type == CMD_UP ||
                              c.type == CMD_DOWN)) {
            if (c.type == CMD_OK || c.type == CMD_MENU_BACK) {
                settings_open = false;
                atomic_store(&menu_active, true);
                menu_refresh();
            }
            continue;
        }
        if (atomic_load(&menu_active) && (c.type == CMD_OK || c.type == CMD_UP ||
                                          c.type == CMD_DOWN || c.type == CMD_MENU_BACK)) {
            if (menu_notice) {
                menu_notice = false;
                menu_refresh();
                continue;
            }
            menu_action_t action = c.type == CMD_MENU_BACK ? menu_back()
                                                           : menu_key(c.type == CMD_OK   ? 0
                                                                      : c.type == CMD_UP ? -1
                                                                                         : 1);
            apply_device_settings();
            if (action == MENU_CHAT || action == MENU_LIVE) {
                open_chat(action == MENU_LIVE);
            } else if (action == MENU_NEW_CHAT || action == MENU_VOICE_CHANGED) {
                new_conversation(action == MENU_VOICE_CHANGED);
            } else if (action == MENU_WIFI) {
                stop_session();
                atomic_store(&menu_active, false);
                ui_show_guide();
                if (voice_wifi_request_setup() != ESP_OK)
                    ui_set("SETUP UNAVAILABLE", "Restart and retry");
            } else if (action == MENU_UPDATE) {
                if (voice_wifi_connected()) {
                    atomic_store(&restart_pending, false);
                    if (device_busy()) stop_session();
                    atomic_store(&menu_active, false);
                    settings_open = true;
                    ui_show_update();
                    if (firmware_update_start() != ESP_OK)
                        ui_set("UPDATE UNAVAILABLE", "OK: back / retry");
                } else {
                    menu_notice = true;
                    const char *items[] = {"Connect Wi-Fi first", "Network > Connect"};
                    ui_show_menu("UPDATE UNAVAILABLE", items, 2, 0, "OK / hold OK: back");
                }
            }
            // Selecting Voice is the first recording gesture, not a navigation-only press.
            if (action == MENU_CHAT)
                c.type = CMD_PTT_BEGIN;
            else
                continue;
        }
        if (c.type == CMD_OK) {
            if (atomic_load(&live_mode))
                c.type =
                    (atomic_load(&ready) || atomic_load(&starting)) ? CMD_CANCEL : CMD_NEW_START;
            else if (atomic_load(&ptt_active))
                c.type = CMD_PTT_END;
            else if (atomic_load(&interaction_active))
                c.type = CMD_CANCEL;
            else
                c.type = CMD_PTT_BEGIN;
        }
        if (c.type == CMD_MENU_BACK) {
            enter_home();
        } else if (c.type == CMD_UP || c.type == CMD_DOWN) {
            settings_adjust_volume(c.type == CMD_UP ? 5 : -5);
            apply_device_settings();
            ui_volume(settings_get().volume);
        } else if (c.type == CMD_VOLUME_SAVE) {
            if (settings_save(settings_get()) != ESP_OK)
                ESP_LOGW(TAG, "volume persistence failed");
        } else if (c.type == CMD_CANCEL) {
            if ((atomic_load(&ready) || atomic_load(&starting)) &&
                atomic_exchange(&interaction_active, false)) {
                mute();
                atomic_store(&awaiting_clear, true);
                atomic_store(&output_timing_pending, false);
                if (!send_json("{\"type\":\"cancel\"}"))
                    retry();
                else
                    ui_set("STOPPED", "Context saved\nOK: resume");
            }
        } else if (c.type == CMD_NEW_START || c.type == CMD_CONNECT) {
            if (c.type == CMD_NEW_START) {
                atomic_store(&wanted, true);
                atomic_store(&attempts, 0);
                atomic_store(&sticky_error, false);
            }
            start_session();
        } else if (c.type == CMD_PTT_BEGIN) {
            if (atomic_load(&menu_active) || atomic_load(&restart_pending) ||
                atomic_load(&ptt_active))
                continue;
            if (!atomic_load(&connected)) {
                ui_set("SERVER NOT CONNECTED", "Hold OK: home");
                continue;
            }
            xSemaphoreTake(send_lock, portMAX_DELAY);
            atomic_store(&captured_frames, 0);
            uplink.count = 0;
            atomic_fetch_add(&capture_generation, 1);
            atomic_store(&capture_started, now_ms());
            xSemaphoreGive(send_lock);
            atomic_store(&live_mode, false);
            atomic_store(&full_duplex, false);
            atomic_store(&wanted, true);
            atomic_store(&attempts, 0);
            atomic_store(&sticky_error, false);
            start_session();
            mute();
            atomic_store(&ptt_active, true);
            atomic_store(&output_timing_pending, false);
            if (send_json("{\"type\":\"ptt_start\"}")) {
                atomic_store(&listen_after, now_ms());
                atomic_store(&recording, true);
                ui_set("RECORDING", "OK: send\nHold OK: home");
            } else {
                retry();
            }
        } else if (c.type == CMD_PTT_END) {
            if (atomic_exchange(&ptt_active, false)) {
                atomic_store(&recording, false);
                // Let any already admitted 20 ms frame finish queueing before draining.
                xSemaphoreTake(capture_lock, portMAX_DELAY);
                xSemaphoreGive(capture_lock);
                atomic_store(&released_at, now_ms());
                atomic_store(&output_timing_pending, true);
                atomic_store(&interaction_active, true);
                if (!send_json("{\"type\":\"ptt_end\"}"))
                    retry();
                else if (!atomic_load(&menu_active))
                    ui_set("THINKING", "Waiting for response");
            }
        } else if (c.type == CMD_LIVE) {
            open_chat(!atomic_load(&live_mode));
        } else if (c.type == CMD_VOICE) {
            open_chat(false);
        } else if (c.type == CMD_CHAT) {
            open_chat(atomic_load(&live_mode));
        } else if (c.type == CMD_STOP) {
            stop_session();
            show_chat();
        } else if (c.type == CMD_DRAIN) {
            if (atomic_load(&ready) && c.epoch == atomic_load(&epoch) &&
                (!atomic_load(&recording) || atomic_load(&full_duplex)) &&
                !atomic_load(&awaiting_clear)) {
                char message[80];
                snprintf(message, sizeof(message), "{\"type\":\"playback_drained\",\"epoch\":%lu}",
                         (unsigned long)c.epoch);
                if (!send_json(message))
                    retry();
            }
        } else if (c.type == CMD_VOICE_FAILED) {
            stop_session();
            atomic_store(&sticky_error, true);
            ui_set("VOICE NOT APPLIED", "OK: retry\nHold OK: home");
        } else if (c.type == CMD_VOICE_CHANGED) {
            apply_device_settings();
            new_conversation(true);
        } else if (c.type == CMD_APPLY) {
            apply_device_settings();
            if (atomic_load(&menu_active))
                menu_refresh();
            else if (!atomic_load(&settings_open) && !device_busy())
                show_chat();
        } else if (c.type == CMD_INPUT_OVERRUN) {
            if (c.epoch != atomic_load(&capture_generation))
                continue;
            atomic_store(&ptt_active, false);
            atomic_store(&interaction_active, false);
            mute();
            send_json("{\"type\":\"cancel\"}");
            ui_set("NETWORK SLOW", "Please try a shorter message");
        } else if (c.type == CMD_FAULT) {
            if (c.epoch != atomic_load(&session_generation) || !atomic_load(&connected) ||
                atomic_load(&recovering) || !atomic_load(&wanted))
                continue;
            send_json("{\"type\":\"stop\"}");
            retry();
        }
    }
}
static void microphone_task(void *unused)
{
    int16_t frame[SAMPLES];
    adpcm_state_t codec = {0};
    uint32_t encoded_generation = UINT32_MAX;
    for (;;) {
        uint32_t generation = atomic_load(&capture_generation);
        uint32_t read_at = now_ms();
        esp_err_t result = bsp_audio_read(frame, FRAME_BYTES);
        maximum(&mic_read_max_ms, now_ms() - read_at);
        if (result != ESP_OK) {
            atomic_fetch_add(&mic_read_errors, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        xSemaphoreTake(capture_lock, portMAX_DELAY);
        if (generation == atomic_load(&capture_generation) && atomic_load(&recording) &&
            atomic_load(&connected) && (int32_t)(now_ms() - atomic_load(&listen_after)) >= 0 &&
            (!atomic_load(&menu_active) || atomic_load(&netcheck))) {
            if (atomic_load(&netcheck))
                memset(frame, 0, sizeof(frame));
            unsigned peak = 0;
            for (unsigned i = 0; i < SAMPLES; i++)
                if ((unsigned)abs(frame[i]) > peak) peak = abs(frame[i]);
            atomic_store(&mic_peak, peak);
            ui_audio_samples(false, frame, SAMPLES);
            if (encoded_generation != generation) {
                codec = (adpcm_state_t){0};
                encoded_generation = generation;
            }
            capture_frame_t packet = {.generation = generation};
            adpcm_encode(&codec, frame, packet.packet);
            if (xQueueSend(capture_queue, &packet, 0) != pdTRUE) {
                if (atomic_load(&live_mode)) {
                    capture_frame_t discarded;
                    if (xQueueReceive(capture_queue, &discarded, 0) == pdTRUE)
                        atomic_fetch_add(&capture_dropped, 1);
                    if (xQueueSend(capture_queue, &packet, 0) != pdTRUE)
                        atomic_fetch_add(&capture_dropped, 1);
                } else {
                    // Never silently submit a truncated PTT utterance or reconnect for overload.
                    atomic_store(&recording, false);
                    atomic_fetch_add(&capture_overruns, 1);
                    enqueue(CMD_INPUT_OVERRUN, generation);
                }
            }
            xTaskNotifyGive(sender_handle);
            if (atomic_load(&ptt_active) && atomic_fetch_add(&captured_frames, 1) + 1 >= 3000) {
                atomic_store(&recording, false);
                enqueue(CMD_PTT_END, 0);
            }
        }
        xSemaphoreGive(capture_lock);
    }
}
static void speaker_task(void *unused)
{
    audio_frame_t frame;
    int16_t samples[SAMPLES];
    uint32_t last_write = 0;
    playback_gate_t gate = {0};
    playback_gain_t output_gain = {0};
    for (;;) {
        uint32_t now = now_ms(), current_epoch = atomic_load(&epoch);
        bool live = atomic_load(&live_mode), ended = atomic_load(&end_pending);
        UBaseType_t queued = uxQueueMessagesWaiting(playback);
        bool have = false;
        // Preserve speech recovered after a network stall. Only the bounded queue's
        // overflow policy may evict audio; half a second of backlog is not obsolete.
        if (gate.epoch != current_epoch || !gate.target)
            playback_gate_reset(&gate, current_epoch, live);
        memset(samples, 0, sizeof(samples));
        if (!atomic_load(&ready) || (atomic_load(&recording) && !atomic_load(&full_duplex)) ||
            atomic_load(&awaiting_clear)) {
            playback_gate_reset(&gate, current_epoch, live);
        } else {
            if (gate.playing && !queued && !ended && now - last_write < 150)
                atomic_fetch_add(&playback_refills, 1);
            if (playback_gate_ready(&gate, queued, live, ended, now) &&
                xQueueReceive(playback, &frame, 0) == pdTRUE)
                have = frame.epoch == atomic_load(&epoch);
        }
        atomic_store(&playback_target, gate.target);
        if (have && frame.epoch == atomic_load(&epoch) &&
            (!atomic_load(&recording) || atomic_load(&full_duplex)) &&
            !atomic_load(&awaiting_clear) && adpcm_decode(frame.packet, ADPCM_BYTES, samples)) {
            maximum(&playback_age_max_ms, now - frame.received_at);
            last_write = now;
            atomic_store(&last_output_at, now);
            atomic_fetch_add(&speaker_frames, 1);
            if (atomic_exchange(&output_timing_pending, false))
                ESP_LOGI(TAG, "PTT release_to_speaker_ms=%u", now - atomic_load(&released_at));
        }
        unsigned peak = 0;
        for (unsigned i = 0; i < SAMPLES; i++)
            if ((unsigned)abs(samples[i]) > peak) peak = abs(samples[i]);
        atomic_store(&output_peak, peak);
        playback_gain_apply(&output_gain, samples, SAMPLES);
        if (bsp_audio_write(samples, FRAME_BYTES) != ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(20));
        else
            ui_audio_samples(true, samples, SAMPLES);
        // Drain the queue AND the 60 ms I2S DMA buffer before reopening the microphone.
        if (atomic_load(&end_pending) && uxQueueMessagesWaiting(playback) == 0 &&
            now_ms() - last_write >= ECHO_GUARD_MS) {
            uint32_t e = atomic_load(&epoch);
            bool yes = true;
            if (atomic_compare_exchange_strong(&end_pending, &yes, false))
                enqueue(CMD_DRAIN, e);
        }
    }
}
static void console_task(void *unused)
{
    char line[256];
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r')
            continue;
        if (ch != '\n') {
            if (used + 1 < sizeof(line))
                line[used++] = (char)ch;
            else
                overflow = true;
            continue;
        }
        line[used] = 0;
        used = 0;
        if (overflow) {
            overflow = false;
            memset(line, 0, sizeof(line));
            continue;
        }
        if (!strncmp(line, "wifi ", 5)) {
            cJSON *c = cJSON_Parse(line + 5), *s = cJSON_GetObjectItem(c, "ssid"),
                  *p = cJSON_GetObjectItem(c, "password");
            bool saved = cJSON_IsString(s) && cJSON_IsString(p) &&
                         voice_wifi_save(s->valuestring, p->valuestring) == ESP_OK;
            cJSON_Delete(c);
            memset(line, 0, sizeof(line));
            puts(saved ? "WIFI_CONFIG_SAVED" : "WIFI_CONFIG_INVALID");
            fflush(stdout);
            if (saved) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            continue;
        }
        if (!strcmp(line, "connect"))
            enqueue(CMD_CHAT, 0);
        else if (!strcmp(line, "live"))
            enqueue(CMD_LIVE, 0);
        else if (!strcmp(line, "stop"))
            enqueue(CMD_STOP, 0);
        else if (!strcmp(line, "voice"))
            enqueue(CMD_VOICE, 0);
        else if (!strcmp(line, "ok"))
            enqueue(CMD_OK, 0);
        else if (!strcmp(line, "record"))
            enqueue(CMD_PTT_BEGIN, 0);
        else if (!strcmp(line, "send"))
            enqueue(CMD_PTT_END, 0);
        else if (!strcmp(line, "cancel"))
            enqueue(CMD_CANCEL, 0);
        else if (!strcmp(line, "home"))
            enqueue(CMD_MENU_BACK, 0);
        else if (!strcmp(line, "trace on"))
            atomic_store(&serial_trace, true);
        else if (!strcmp(line, "trace off"))
            atomic_store(&serial_trace, false);
    }
}
static void health_task(void *unused)
{
    bool gauge = bsp_battery_init() == ESP_OK;
    if (gauge)
        atomic_store(&battery, bsp_battery_soc());
    ui_battery(device_battery(), settings_get().volume);
    unsigned ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ticks++;
        // Never send from the websocket callback: it may hold the transport lock.
        if (atomic_load(&connected) && atomic_exchange(&warmup_pending, false)) {
            if (!send_json("{\"type\":\"hello\",\"warmup\":true}"))
                atomic_store(&warmup_pending, true);
        }
        ui_tick((device_busy() && !atomic_load(&menu_active)) || portal_updating());
        if (ticks >= 50 && atomic_exchange(&boot_complete, false)) {
            // Confirm only after hardware, UI, networking and local web service initialized.
            // Offline Wi-Fi provisioning is a valid boot too.
            esp_ota_img_states_t state;
            if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                state == ESP_OTA_IMG_PENDING_VERIFY)
                ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        }
        if (ticks % 50 == 0) {
            if (!gauge && ticks % 300 == 0)
                gauge = bsp_battery_init() == ESP_OK;
            if (gauge)
                atomic_store(&battery, bsp_battery_soc());
            ui_battery(device_battery(), settings_get().volume);
        }
        if (atomic_load(&ptt_active) && now_ms() - atomic_load(&capture_started) > 60000) {
            enqueue(CMD_PTT_END, 0);
        }
        if (atomic_load(&netcheck) && now_ms() - atomic_load(&started_at) > 20000)
            enqueue(CMD_NETCHECK_DONE, 0);
        if (atomic_load(&starting) && now_ms() - atomic_load(&started_at) > 55000) {
            atomic_store(&started_at, now_ms());
            enqueue(CMD_FAULT, atomic_load(&session_generation));
        }
        if (atomic_load(&wanted) && atomic_load(&connected) && !atomic_load(&starting) &&
            !atomic_load(&ready) && (int32_t)(now_ms() - atomic_load(&retry_at)) >= 0) {
            atomic_store(&retry_at, now_ms() + 3000);
            enqueue(CMD_CONNECT, 0);
        }
        if (atomic_load(&ready) && now_ms() - atomic_load(&ready_at) > 30000)
            atomic_store(&attempts, 0);
        if (ticks % 100 == 0)
            ESP_LOGI(TAG,
                     "health heap=%lu tx=%u speaker=%u dropped=%u buffered=%u refills=%u "
                     "connected=%d ready=%d listening=%d battery=%d min_heap=%lu "
                     "alloc_fail=%u send_max_ms=%u wifi_lost=%u wifi_reason=%u",
                     (unsigned long)esp_get_free_heap_size(), atomic_load(&mic_frames),
                     atomic_load(&speaker_frames), atomic_load(&dropped_frames),
                     (unsigned)uxQueueMessagesWaiting(playback), atomic_load(&playback_refills),
                     atomic_load(&connected), atomic_load(&ready), atomic_load(&recording),
                     device_battery(), (unsigned long)esp_get_minimum_free_heap_size(),
                     atomic_load(&alloc_failures), atomic_load(&send_max_ms),
                     atomic_load(&wifi_disconnects), atomic_load(&wifi_last_reason));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
    ESP_ERROR_CHECK(nvs_flash_init());
    settings_init();
    atomic_store(&live_mode, false);
    ui_init();
    ui_mode(false);
    atomic_store(&menu_active, true);
    menu_open(0);
    playback = xQueueCreate(PLAYBACK_FRAMES, sizeof(audio_frame_t));
    commands = xQueueCreate(12, sizeof(command_t));
    send_lock = xSemaphoreCreateMutex();
    capture_lock = xSemaphoreCreateMutex();
    capture_queue = xQueueCreate(CAPTURE_FRAMES, sizeof(capture_frame_t));
    assert(playback && commands && send_lock && capture_lock && capture_queue);
    ESP_ERROR_CHECK(bsp_audio_init());
    ESP_ERROR_CHECK(bsp_audio_set_format(24000, 16, 1));
    bsp_audio_set_volume(settings_get().volume);
    ESP_ERROR_CHECK(bsp_button_init(button, NULL));
    ESP_ERROR_CHECK(xTaskCreate(command_task, "controls", 6144, NULL, 5, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(audio_sender_task, "audio_send", 4096, NULL, 5, &sender_handle) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(microphone_task, "mic", 4096, NULL, 6, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(speaker_task, "speaker", 4096, NULL, 6, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(console_task, "console", 3072, NULL, 3, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(health_task, "health", 3072, NULL, 2, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    esp_err_t wifi = voice_wifi_start();
    if (wifi != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi setup required: %s", esp_err_to_name(wifi));
        atomic_store(&boot_complete, true);
        return;
    }
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_diagnostic, NULL));
    ESP_ERROR_CHECK(portal_start());
    static const char headers[] = "Authorization: Bearer " DEVICE_TOKEN "\r\n";
    // Keep a transport handle so each reconnect can configure its new socket.
    assert(strncmp(VOICE_URL, "ws://", 5) == 0);
    tcp_transport = esp_transport_tcp_init();
    assert(tcp_transport);
    esp_transport_handle_t transport = esp_transport_ws_init(tcp_transport);
    assert(transport);
    ws_send_bind(transport, tcp_transport);
    const char *path = strchr(VOICE_URL + 5, '/');
    esp_transport_ws_config_t transport_config = {
        .ws_path = path ? path : "/",
        .headers = headers,
        // The client owns heartbeat tracking and must receive the PONG events.
        .propagate_control_frames = true,
    };
    ESP_ERROR_CHECK(esp_transport_ws_set_config(transport, &transport_config));
    esp_websocket_client_config_t config = {
        .uri = VOICE_URL,
        .headers = headers,
        .buffer_size = 1200,
        .ext_transport = transport,
        .task_stack = 6144,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 5000,
        .ping_interval_sec = 15,
    };
    ws = esp_websocket_client_init(&config);
    assert(ws);
    ESP_ERROR_CHECK(esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(ws));
    atomic_store(&boot_complete, true);
}
