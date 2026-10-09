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
#include "device.h"

#define SAMPLES 480
#define FRAME_BYTES (SAMPLES * sizeof(int16_t))
#define PLAYBACK_FRAMES 32
#define PLAYBACK_PREFILL 12
#define ECHO_GUARD_MS 180
typedef struct {
    uint32_t epoch;
    int16_t samples[SAMPLES];
} audio_frame_t;
typedef enum {
    CMD_CONNECT,
    CMD_OK,
    CMD_STOP,
    CMD_VOLUME_UP,
    CMD_VOLUME_DOWN,
    CMD_WIFI,
    CMD_WAKE,
    CMD_SETTINGS,
    CMD_APPLY,
    CMD_DRAIN,
    CMD_FAULT
} command_type_t;
typedef struct {
    command_type_t type;
    uint32_t epoch;
} command_t;
static const char *TAG = "gpt_live";
static esp_websocket_client_handle_t ws;
static esp_transport_handle_t tcp_transport;
static QueueHandle_t playback, commands;
static SemaphoreHandle_t send_lock;
static atomic_bool connected, ready, recording, starting, wanted, awaiting_clear, end_pending,
    sticky_error;
static atomic_bool boot_complete;
static bool settings_open;
static atomic_uint epoch, mic_frames, speaker_frames, dropped_frames, playback_refills;
static atomic_int battery = -1, attempts;
static atomic_uint listen_after, retry_at, started_at, ready_at, last_output_at;
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
    if (portal_updating())
        return "UPDATING";
    if (!atomic_load(&connected))
        return "CONNECTING";
    if (atomic_load(&starting))
        return "CONNECTING VOICE";
    if (atomic_load(&recording))
        return "LISTENING";
    if (atomic_load(&ready))
        return "SPEAKING";
    return "IDLE";
}
void device_apply_settings(void)
{
    enqueue(CMD_APPLY, 0);
}
static bool send_json(const char *message)
{
    if (!atomic_load(&connected))
        return false;
    size_t n = strlen(message);
    xSemaphoreTake(send_lock, portMAX_DELAY);
    int sent = esp_websocket_client_send_text(ws, message, n, pdMS_TO_TICKS(1000));
    xSemaphoreGive(send_lock);
    return sent == n;
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
    char ip[20], detail[96];
    voice_wifi_ip(ip, sizeof(ip));
    snprintf(detail, sizeof(detail), "Speak after LISTENING\nhttp://%s", ip);
    ui_set("READY", detail);
}
static void retry(void)
{
    mute();
    atomic_store(&ready, false);
    atomic_store(&starting, false);
    if (!atomic_load(&wanted))
        return;
    int count = atomic_fetch_add(&attempts, 1) + 1;
    if (count > 3) {
        atomic_store(&wanted, false);
        atomic_store(&sticky_error, true);
        ui_set("CONNECTION LOST", "Press OK to retry\nHold DOWN for Wi-Fi setup");
    } else {
        atomic_store(&retry_at, now_ms() + (2000 << (count - 1)));
        ui_set("RECONNECTING", "Please wait...");
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
        if (atomic_load(&wanted)) {
            atomic_store(&retry_at, now_ms() + 1500);
            ui_set("RECONNECTING", "Opening voice again...");
        } else if (!atomic_load(&sticky_error))
            idle_ui();
        ESP_LOGI(TAG, "device bridge connected");
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
        atomic_store(&connected, false);
        if (atomic_load(&wanted))
            retry();
        else {
            mute();
            atomic_store(&ready, false);
            atomic_store(&starting, false);
            if (!atomic_load(&sticky_error))
                ui_set("CONNECTING", "Connecting voice server...");
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
        if (opcode == 2 && received == FRAME_BYTES + 4) {
            uint32_t e;
            memcpy(&e, payload, 4);
            if (e != atomic_load(&epoch) || !atomic_load(&ready) || atomic_load(&recording) ||
                atomic_load(&awaiting_clear))
                return;
            if (xQueueSend(playback, payload, 0) != pdTRUE) {
                audio_frame_t discarded;
                xQueueReceive(playback, &discarded, 0);
                atomic_fetch_add(&dropped_frames, 1);
                xQueueSend(playback, payload, 0);
            }
        } else if (opcode == 1) {
            payload[received] = 0;
            cJSON *m = cJSON_Parse(payload), *t = cJSON_GetObjectItem(m, "type"),
                  *state = cJSON_GetObjectItem(m, "state"), *ev = cJSON_GetObjectItem(m, "epoch");
            const char *type = cJSON_IsString(t) ? t->valuestring : "";
            uint32_t e = cJSON_IsNumber(ev) ? (uint32_t)ev->valuedouble : 0;
            if (!strcmp(type, "status") && cJSON_IsString(state)) {
                ESP_LOGI(TAG, "session %s", state->valuestring);
                if (!strcmp(state->valuestring, "ready") && atomic_load(&wanted)) {
                    atomic_store(&ready, true);
                    atomic_store(&starting, false);
                    atomic_store(&ready_at, now_ms());
                    atomic_store(&sticky_error, false);
                } else if (!strcmp(state->valuestring, "connecting") && atomic_load(&wanted))
                    ui_set("CONNECTING VOICE", "Opening subscription voice...");
                else if (!strcmp(state->valuestring, "idle")) {
                    bool active = atomic_load(&ready) || atomic_load(&starting);
                    mute();
                    atomic_store(&ready, false);
                    atomic_store(&starting, false);
                    if (active && atomic_load(&wanted))
                        retry();
                    else if (!atomic_load(&wanted) && !atomic_load(&sticky_error))
                        idle_ui();
                }
            } else if (!strcmp(type, "listen") && atomic_load(&ready) &&
                       !atomic_load(&awaiting_clear)) {
                atomic_store(&epoch, e);
                atomic_store(&end_pending, false);
                atomic_store(&listen_after, microphone_deadline());
                atomic_store(&recording, true);
                ui_set("LISTENING", "Speak, then pause for reply");
            } else if (!strcmp(type, "playback_begin") && atomic_load(&ready) &&
                       !atomic_load(&awaiting_clear)) {
                atomic_store(&recording, false);
                atomic_store(&end_pending, false);
                atomic_store(&epoch, e);
                ui_set("SPEAKING", "OK: interrupt and speak");
            } else if (!strcmp(type, "playback_end") && atomic_load(&ready) &&
                       e == atomic_load(&epoch) && !atomic_load(&awaiting_clear)) {
                atomic_store(&end_pending, true);
            } else if (!strcmp(type, "clear") && atomic_load(&ready)) {
                mute();
                atomic_store(&epoch, e);
                atomic_store(&listen_after, microphone_deadline());
                atomic_store(&recording, true);
                ui_set("LISTENING", "Continue speaking");
            } else if (!strcmp(type, "error")) {
                cJSON *again = cJSON_GetObjectItem(m, "retryable"),
                      *message = cJSON_GetObjectItem(m, "code");
                if (cJSON_IsTrue(again) && atomic_load(&wanted))
                    retry();
                else {
                    atomic_store(&wanted, false);
                    mute();
                    atomic_store(&ready, false);
                    atomic_store(&starting, false);
                    atomic_store(&sticky_error, true);
                    ui_set("VOICE UNAVAILABLE",
                           cJSON_IsString(message) && !strcmp(message->valuestring, "account")
                               ? "Server login required"
                           : cJSON_IsString(message) && !strcmp(message->valuestring, "quota")
                               ? "Voice usage unavailable"
                               : "Press OK to retry");
                }
            } else if (!strcmp(type, "ended")) {
                cJSON *reason = cJSON_GetObjectItem(m, "reason");
                atomic_store(&wanted, false);
                mute();
                atomic_store(&ready, false);
                atomic_store(&starting, false);
                atomic_store(&sticky_error, true);
                ui_set("CHAT ENDED", cJSON_IsString(reason) && !strcmp(reason->valuestring, "limit")
                                         ? "30 min limit. OK: new chat"
                                         : "Idle timeout. OK: new chat");
            }
            cJSON_Delete(m);
        }
    }
}
static void button(bsp_btn_t key, bsp_btn_ev_t event, void *user)
{
    static bool held[3];
    if (event == BSP_BTN_PRESS) {
        held[key] = false;
        enqueue(CMD_WAKE, 0);
    } else if (event == BSP_BTN_LONG) {
        held[key] = true;
        if (key == BSP_BTN_OK)
            enqueue(CMD_STOP, 0);
        else if (key == BSP_BTN_UP)
            enqueue(device_busy() ? CMD_VOLUME_UP : CMD_SETTINGS, 0);
        else if (key == BSP_BTN_DOWN)
            enqueue(device_busy() && voice_wifi_connected() ? CMD_VOLUME_DOWN : CMD_WIFI, 0);
    } else if (event == BSP_BTN_RELEASE && !held[key]) {
        if (key == BSP_BTN_OK)
            enqueue(CMD_OK, 0);
        else
            enqueue(key == BSP_BTN_UP ? CMD_VOLUME_UP : CMD_VOLUME_DOWN, 0);
    }
}
static void command_task(void *unused)
{
    command_t c;
    for (;;) {
        xQueueReceive(commands, &c, portMAX_DELAY);
        if (c.type != CMD_DRAIN && c.type != CMD_CONNECT)
            ui_wake();
        if (portal_updating())
            continue;
        if (c.type == CMD_WAKE)
            continue;
        if (voice_wifi_provisioning()) {
            if (c.type == CMD_OK || c.type == CMD_VOLUME_UP || c.type == CMD_VOLUME_DOWN)
                voice_wifi_setup_next();
            else if (c.type == CMD_STOP)
                esp_restart();
            continue;
        }
        if (settings_open && (c.type == CMD_OK || c.type == CMD_STOP)) {
            settings_open = false;
            ui_show_guide();
            idle_ui();
            continue;
        }
        if (c.type == CMD_OK && atomic_load(&ready)) {
            mute();
            atomic_store(&awaiting_clear, true);
            ui_set("INTERRUPTING", "Wait for LISTENING");
            if (!send_json("{\"type\":\"interrupt\"}"))
                retry();
        } else if (c.type == CMD_CONNECT || c.type == CMD_OK) {
            if (c.type == CMD_OK) {
                atomic_store(&wanted, true);
                atomic_store(&attempts, 0);
                atomic_store(&sticky_error, false);
            }
            if (atomic_load(&wanted) && !atomic_load(&starting) && !atomic_load(&ready) &&
                atomic_load(&connected)) {
                atomic_store(&starting, true);
                atomic_store(&started_at, now_ms());
                char start[128];
                snprintf(start, sizeof(start),
                         "{\"type\":\"start\",\"protocol\":2,\"mode\":\"continuous\",\"idle_"
                         "seconds\":%d}",
                         settings_get().idle_seconds);
                if (!send_json(start))
                    retry();
                else
                    ui_set("CONNECTING VOICE", "Wait for LISTENING");
            } else if (!atomic_load(&connected))
                ui_set("WAITING FOR WI-FI", "Hold DOWN for phone setup");
        } else if (c.type == CMD_STOP) {
            atomic_store(&wanted, false);
            atomic_store(&ready, false);
            atomic_store(&starting, false);
            atomic_store(&sticky_error, false);
            mute();
            send_json("{\"type\":\"stop\"}");
            idle_ui();
        } else if (c.type == CMD_DRAIN) {
            if (atomic_load(&ready) && c.epoch == atomic_load(&epoch) && !atomic_load(&recording) &&
                !atomic_load(&awaiting_clear)) {
                char message[80];
                snprintf(message, sizeof(message), "{\"type\":\"playback_drained\",\"epoch\":%lu}",
                         (unsigned long)c.epoch);
                if (!send_json(message))
                    retry();
            }
        } else if (c.type == CMD_VOLUME_UP || c.type == CMD_VOLUME_DOWN) {
            settings_t p = settings_get();
            p.volume += c.type == CMD_VOLUME_UP ? 5 : -5;
            if (p.volume > 100)
                p.volume = 100;
            if (p.volume < 0)
                p.volume = 0;
            if (settings_save(p) == ESP_OK) {
                bsp_audio_set_volume(p.volume);
                ui_battery(device_battery(), p.volume);
            }
        } else if (c.type == CMD_APPLY) {
            bsp_audio_set_volume(settings_get().volume);
            ui_battery(device_battery(), settings_get().volume);
            ui_wake();
        } else if (c.type == CMD_SETTINGS) {
            if (voice_wifi_connected()) {
                settings_open = true;
                portal_arm();
            } else
                ui_set("WI-FI NOT CONNECTED", "Hold DOWN for phone setup");
        } else if (c.type == CMD_WIFI) {
            if (!device_busy() || !voice_wifi_connected()) {
                atomic_store(&wanted, false);
                atomic_store(&starting, false);
                atomic_store(&ready, false);
                mute();
                send_json("{\"type\":\"stop\"}");
                if (voice_wifi_request_setup() != ESP_OK)
                    ui_set("SETUP UNAVAILABLE", "Please restart and retry");
            }
        } else if (c.type == CMD_FAULT) {
            send_json("{\"type\":\"stop\"}");
            retry();
        }
    }
}
static void microphone_task(void *unused)
{
    int16_t frame[SAMPLES];
    for (;;) {
        if (bsp_audio_read(frame, FRAME_BYTES) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!atomic_load(&recording) || !atomic_load(&connected) ||
            (int32_t)(now_ms() - atomic_load(&listen_after)) < 0)
            continue;
        xSemaphoreTake(send_lock, portMAX_DELAY);
        if (atomic_load(&recording)) {
            int sent = esp_websocket_client_send_bin(ws, (const char *)frame, FRAME_BYTES,
                                                     pdMS_TO_TICKS(1000));
            if (sent == FRAME_BYTES)
                atomic_fetch_add(&mic_frames, 1);
            else {
                atomic_store(&recording, false);
                enqueue(CMD_FAULT, 0);
            }
        }
        xSemaphoreGive(send_lock);
    }
}
static void speaker_task(void *unused)
{
    audio_frame_t frame;
    uint32_t last_write = 0, prefill_since = 0;
    bool playing = false;
    for (;;) {
        uint32_t now = now_ms();
        UBaseType_t queued = uxQueueMessagesWaiting(playback);
        bool have = false;
        memset(&frame, 0, sizeof(frame));
        if (!atomic_load(&ready) || atomic_load(&recording) || atomic_load(&awaiting_clear)) {
            playing = false;
            prefill_since = 0;
        } else {
            if (!playing && queued) {
                if (!prefill_since)
                    prefill_since = now;
                if (queued >= PLAYBACK_PREFILL || now - prefill_since >= 350 ||
                    atomic_load(&end_pending)) {
                    playing = true;
                    prefill_since = 0;
                }
            }
            if (playing) {
                if (xQueueReceive(playback, &frame, 0) == pdTRUE)
                    have = frame.epoch == atomic_load(&epoch);
                else {
                    playing = false;
                    prefill_since = 0;
                    if (now - last_write < 150)
                        atomic_fetch_add(&playback_refills, 1);
                }
            }
        }
        if (have && !atomic_load(&recording) && !atomic_load(&awaiting_clear)) {
            last_write = now;
            atomic_store(&last_output_at, now);
            atomic_fetch_add(&speaker_frames, 1);
        } else
            memset(frame.samples, 0, FRAME_BYTES);
        if (bsp_audio_write(frame.samples, FRAME_BYTES) != ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(20));
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
            enqueue(CMD_OK, 0);
        else if (!strcmp(line, "interrupt"))
            enqueue(CMD_OK, 0);
        else if (!strcmp(line, "stop"))
            enqueue(CMD_STOP, 0);
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
        ui_tick(device_busy() || portal_updating());
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
        if (atomic_load(&starting) && now_ms() - atomic_load(&started_at) > 55000) {
            atomic_store(&started_at, now_ms());
            enqueue(CMD_FAULT, 0);
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
                     "connected=%d ready=%d listening=%d battery=%d",
                     (unsigned long)esp_get_free_heap_size(), atomic_load(&mic_frames),
                     atomic_load(&speaker_frames), atomic_load(&dropped_frames),
                     (unsigned)uxQueueMessagesWaiting(playback), atomic_load(&playback_refills),
                     atomic_load(&connected), atomic_load(&ready), atomic_load(&recording),
                     device_battery());
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    settings_init();
    ui_init();
    playback = xQueueCreate(PLAYBACK_FRAMES, sizeof(audio_frame_t));
    commands = xQueueCreate(12, sizeof(command_t));
    send_lock = xSemaphoreCreateMutex();
    assert(playback && commands && send_lock);
    ESP_ERROR_CHECK(bsp_audio_init());
    ESP_ERROR_CHECK(bsp_audio_set_format(24000, 16, 1));
    bsp_audio_set_volume(settings_get().volume);
    ESP_ERROR_CHECK(bsp_button_init(button, NULL));
    ESP_ERROR_CHECK(xTaskCreate(command_task, "controls", 4096, NULL, 5, NULL) == pdPASS
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
    ESP_ERROR_CHECK(portal_start());
    static const char headers[] = "Authorization: Bearer " DEVICE_TOKEN "\r\n";
    // Keep a transport handle so each reconnect can configure its new socket.
    assert(strncmp(VOICE_URL, "ws://", 5) == 0);
    tcp_transport = esp_transport_tcp_init();
    assert(tcp_transport);
    esp_transport_handle_t transport = esp_transport_ws_init(tcp_transport);
    assert(transport);
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
