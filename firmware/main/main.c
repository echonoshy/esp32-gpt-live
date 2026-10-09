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
#include "menu.h"
#include "controls.h"

#define SAMPLES 480
#define FRAME_BYTES (SAMPLES * sizeof(int16_t))
#define PLAYBACK_FRAMES 32
#define PLAYBACK_PREFILL 12
#define ECHO_GUARD_MS 180
#define PTT_HOLD_MS 200
#define PREROLL_FRAMES 16
typedef struct {
    uint32_t epoch;
    int16_t samples[SAMPLES];
} audio_frame_t;
typedef enum {
    CMD_CONNECT,
    CMD_OK,
    CMD_STOP,
    CMD_UP,
    CMD_DOWN,
    CMD_WAKE,
    CMD_SETTINGS,
    CMD_APPLY,
    CMD_DRAIN,
    CMD_FAULT,
    CMD_PTT_PREPARE,
    CMD_MENU_BACK,
    CMD_PTT_BEGIN,
    CMD_PTT_END,
    CMD_NEW_START
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
static atomic_bool settings_open;
static bool menu_notice;
static atomic_bool capture_pending, accept_requested;
static atomic_uint released_at;
static atomic_bool output_timing_pending;
static int16_t preroll[PREROLL_FRAMES][SAMPLES];
static unsigned preroll_count;   // Protected by send_lock.
static unsigned captured_frames; // Protected by send_lock; includes the pre-roll.
static atomic_bool menu_active, live_mode, full_duplex, session_full_duplex, ptt_held,
    restart_pending, can_listen;
static atomic_uint capture_started;
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
    if (atomic_load(&menu_active))
        return "SETTINGS";
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
    snprintf(detail, sizeof(detail), "Hold OK: %s\nhttp://%s",
             atomic_load(&live_mode) ? "talk (PTT)" : "talk", ip);
    ui_set("READY", detail);
}
static void retry(void)
{
    atomic_store(&capture_pending, false);
    atomic_store(&output_timing_pending, false);
    mute();
    atomic_store(&ptt_held, false);
    atomic_store(&ready, false);
    atomic_store(&starting, false);
    if (!atomic_load(&wanted))
        return;
    int count = atomic_fetch_add(&attempts, 1) + 1;
    if (count > 3) {
        atomic_store(&wanted, false);
        atomic_store(&sticky_error, true);
        ui_set("CONNECTION LOST", "Hold OK to retry\nUP: network settings");
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
        if (atomic_exchange(&restart_pending, false))
            atomic_store(&wanted, true);
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
            if (e != atomic_load(&epoch) || !atomic_load(&ready) ||
                (atomic_load(&recording) && !atomic_load(&full_duplex)) ||
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
                } else if (!strcmp(state->valuestring, "connecting") && atomic_load(&wanted) &&
                           !atomic_load(&recording))
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
            } else if (!strcmp(type, "stopped")) {
                if (atomic_exchange(&restart_pending, false))
                    enqueue(CMD_NEW_START, 0);
            } else if (!strcmp(type, "listen") && atomic_load(&ready) &&
                       !atomic_load(&awaiting_clear)) {
                atomic_store(&epoch, e);
                atomic_store(&end_pending, false);
                atomic_store(&listen_after, microphone_deadline());
                atomic_store(&can_listen, true);
                atomic_store(&recording, atomic_load(&live_mode) && !atomic_load(&menu_active));
                ui_set("LISTENING", "Hold OK: talk  UP: settings");
            } else if (!strcmp(type, "playback_begin") && atomic_load(&ready) &&
                       !atomic_load(&awaiting_clear)) {
                atomic_store(&can_listen, atomic_load(&full_duplex));
                atomic_store(&recording, atomic_load(&full_duplex) && !atomic_load(&menu_active));
                atomic_store(&end_pending, false);
                atomic_store(&epoch, e);
                ui_set("SPEAKING", "Hold OK: talk  UP: settings");
            } else if (!strcmp(type, "playback_end") && atomic_load(&ready) &&
                       e == atomic_load(&epoch) && !atomic_load(&awaiting_clear)) {
                atomic_store(&end_pending, true);
            } else if (!strcmp(type, "clear") && atomic_load(&wanted)) {
                mute();
                atomic_store(&epoch, e);
                atomic_store(&listen_after, microphone_deadline());
                bool capture = atomic_load(&live_mode) || atomic_load(&ptt_held);
                atomic_store(&can_listen, capture);
                atomic_store(&recording, capture && !atomic_load(&menu_active));
                if (capture)
                    ui_set(atomic_load(&live_mode) ? "LISTENING" : "RECORDING",
                           "Release OK to send in hold mode");
            } else if (!strcmp(type, "waiting") && !atomic_load(&live_mode)) {
                if (!atomic_load(&recording) && !atomic_load(&ptt_held)) {
                    atomic_store(&epoch, e);
                    atomic_store(&awaiting_clear, false);
                    cJSON *too_long = cJSON_GetObjectItem(m, "too_long");
                    ui_set(cJSON_IsTrue(too_long) ? "RECORDING TOO LONG" : "HOLD TO TALK",
                           cJSON_IsTrue(too_long) ? "Not sent. Max 60 seconds"
                                                  : "Hold OK, then speak\nUP: settings");
                }
            } else if (!strcmp(type, "submitted")) {
                if (!atomic_load(&ptt_held))
                    ui_set("THINKING", "Waiting for reply...");
            } else if (!strcmp(type, "error")) {
                cJSON *again = cJSON_GetObjectItem(m, "retryable"),
                      *message = cJSON_GetObjectItem(m, "code");
                if (cJSON_IsTrue(again) && atomic_load(&wanted))
                    retry();
                else {
                    atomic_store(&wanted, false);
                    atomic_store(&ptt_held, false);
                    mute();
                    atomic_store(&ready, false);
                    atomic_store(&starting, false);
                    atomic_store(&sticky_error, true);
                    ui_set("VOICE UNAVAILABLE",
                           cJSON_IsString(message) && !strcmp(message->valuestring, "account")
                               ? "Server login required"
                           : cJSON_IsString(message) && !strcmp(message->valuestring, "quota")
                               ? "Voice usage unavailable"
                               : "Hold OK to retry");
                }
            } else if (!strcmp(type, "ended")) {
                cJSON *reason = cJSON_GetObjectItem(m, "reason");
                atomic_store(&wanted, false);
                atomic_store(&ptt_held, false);
                mute();
                atomic_store(&ready, false);
                atomic_store(&starting, false);
                atomic_store(&sticky_error, true);
                ui_set("CHAT ENDED", cJSON_IsString(reason) && !strcmp(reason->valuestring, "limit")
                                         ? "30 min limit. Hold OK: start"
                                         : "Idle timeout. Hold OK: start");
            }
            cJSON_Delete(m);
        }
    }
}
static void button(bsp_btn_t key, bsp_btn_ev_t event, void *user)
{
    static controls_t controls;
    if (event != BSP_BTN_PRESS && event != BSP_BTN_RELEASE && event != BSP_BTN_LONG)
        return;
    control_event_t ev = event == BSP_BTN_PRESS  ? CONTROL_PRESS
                         : event == BSP_BTN_LONG ? CONTROL_LONG
                                                 : CONTROL_RELEASE;
    bool navigation =
        atomic_load(&menu_active) || atomic_load(&settings_open) || voice_wifi_provisioning();
    control_action_t action = controls_event(&controls, (control_key_t)key, ev, navigation);
    static const command_type_t mapping[] = {CMD_WAKE,        CMD_WAKE,    CMD_UP, CMD_DOWN,
                                             CMD_PTT_PREPARE, CMD_PTT_END, CMD_OK, CMD_MENU_BACK};
    if (action != CONTROL_NONE)
        enqueue(mapping[action], 0);
}
static void cancel_pending_capture(void)
{
    xSemaphoreTake(send_lock, portMAX_DELAY);
    atomic_store(&capture_pending, false);
    atomic_store(&accept_requested, false);
    preroll_count = 0;
    xSemaphoreGive(send_lock);
}
static void apply_device_settings(void)
{
    settings_t p = settings_get();
    bsp_audio_set_volume(p.volume);
    ui_battery(device_battery(), p.volume);
    ui_wake();
}
static void show_home(void)
{
    bool live = atomic_load(&live_mode);
    ui_mode(live);
    ui_show_guide();
    if (atomic_load(&recording))
        ui_set(live ? "LISTENING" : "RECORDING", "UP: settings");
    else if (atomic_load(&ready))
        ui_set(live ? "CHAT ACTIVE" : "HOLD TO TALK", "Hold OK: talk / interrupt");
    else
        idle_ui();
}
static void start_session(void)
{
    if (!atomic_load(&wanted) || atomic_load(&starting) || atomic_load(&ready))
        return;
    if (!atomic_load(&connected)) {
        ui_set("SERVER NOT CONNECTED", "UP: network settings");
        return;
    }
    settings_t p = settings_get();
    bool live = atomic_load(&live_mode);
    atomic_store(&session_full_duplex, p.duplex);
    atomic_store(&full_duplex, live && p.duplex);
    atomic_store(&can_listen, false);
    atomic_store(&starting, true);
    atomic_store(&started_at, now_ms());
    char start[320];
    snprintf(start, sizeof(start),
             "{\"type\":\"start\",\"protocol\":4,\"mode\":\"%s\",\"voice\":\"%s\",\"pace\":%d,"
             "\"reply_length\":%d,\"language\":%d,\"duplex\":%d,\"idle_seconds\":%d}",
             live ? "continuous" : "ptt", settings_voice(p.voice), p.pace, p.reply_length,
             p.language, p.duplex, p.idle_seconds);
    if (!send_json(start))
        retry();
    else
        ui_set("CONNECTING VOICE", live ? "Wait for LISTENING" : "Hold OK to record");
    ui_mode(live);
    if (!atomic_load(&menu_active) && !settings_open)
        ui_show_guide();
}
static void stop_session(void)
{
    cancel_pending_capture();
    atomic_store(&wanted, false);
    atomic_store(&ptt_held, false);
    atomic_store(&can_listen, false);
    atomic_store(&ready, false);
    atomic_store(&starting, false);
    atomic_store(&sticky_error, false);
    mute();
    send_json("{\"type\":\"stop\"}");
}
static void close_menu(void)
{
    atomic_store(&menu_active, false);
    if (atomic_load(&ready) && atomic_load(&live_mode)) {
        send_json("{\"type\":\"resume\"}");
        atomic_store(&recording, atomic_load(&can_listen));
    }
    show_home();
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
        if (atomic_load(&menu_active) &&
            (c.type == CMD_OK || c.type == CMD_UP || c.type == CMD_DOWN ||
             c.type == CMD_MENU_BACK || c.type == CMD_PTT_PREPARE || c.type == CMD_PTT_BEGIN)) {
            if (c.type == CMD_PTT_PREPARE || c.type == CMD_PTT_BEGIN)
                continue;
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
            if (action == MENU_CLOSE)
                close_menu();
            else if (action == MENU_NEW_CHAT) {
                atomic_store(&menu_active, false);
                ui_show_guide();
                atomic_store(&restart_pending, atomic_load(&connected));
                stop_session();
                ui_set("NEW CONVERSATION", "Clearing previous context...");
                if (!atomic_load(&connected))
                    enqueue(CMD_NEW_START, 0);
            } else if (action == MENU_STOP) {
                stop_session();
                close_menu();
            } else if (action == MENU_WIFI) {
                stop_session();
                atomic_store(&menu_active, false);
                ui_show_guide();
                if (voice_wifi_request_setup() != ESP_OK)
                    ui_set("SETUP UNAVAILABLE", "Please restart and retry");
            } else if (action == MENU_PHONE) {
                if (device_busy()) {
                    menu_notice = true;
                    const char *items[] = {"End chat first", "Conversation > End chat"};
                    ui_show_menu("PHONE SETTINGS", items, 2, 0, "OK: return to menu");
                } else if (voice_wifi_connected()) {
                    atomic_store(&menu_active, false);
                    settings_open = true;
                    portal_arm();
                }
            }
            continue;
        }
        if (c.type == CMD_UP || c.type == CMD_SETTINGS) {
            cancel_pending_capture();
            atomic_store(&menu_active, true);
            if (atomic_load(&live_mode)) {
                atomic_store(&recording, false);
                send_json("{\"type\":\"pause\"}");
            }
            menu_open();
        } else if (c.type == CMD_NEW_START || c.type == CMD_CONNECT) {
            if (c.type == CMD_NEW_START) {
                atomic_store(&wanted, true);
                atomic_store(&attempts, 0);
                atomic_store(&sticky_error, false);
            }
            start_session();
        } else if (c.type == CMD_PTT_PREPARE) {
            if (atomic_load(&restart_pending) || atomic_load(&capture_pending) ||
                atomic_load(&ptt_held))
                continue;
            xSemaphoreTake(send_lock, portMAX_DELAY);
            preroll_count = 0;
            captured_frames = 0;
            atomic_store(&capture_started, now_ms());
            atomic_store(&accept_requested, false);
            atomic_store(&capture_pending, true);
            xSemaphoreGive(send_lock);
        } else if (c.type == CMD_PTT_BEGIN) {
            if (!atomic_load(&capture_pending) || c.epoch != atomic_load(&capture_started))
                continue;
            if (!atomic_load(&connected)) {
                cancel_pending_capture();
                ui_set("SERVER NOT CONNECTED", "UP: network settings");
                continue;
            }
            atomic_store(&live_mode, false);
            atomic_store(&full_duplex, false);
            atomic_store(&wanted, true);
            atomic_store(&attempts, 0);
            atomic_store(&sticky_error, false);
            start_session();
            mute();
            atomic_store(&ptt_held, true);
            atomic_store(&output_timing_pending, false);
            if (send_json("{\"type\":\"ptt_start\"}")) {
                // Serialize pre-roll, subsequent microphone frames, and release on one lock.
                xSemaphoreTake(send_lock, portMAX_DELAY);
                bool ok = true;
                for (unsigned i = 0; i < preroll_count && ok; i++) {
                    ok = esp_websocket_client_send_bin(ws, (const char *)preroll[i], FRAME_BYTES,
                                                       pdMS_TO_TICKS(1000)) == FRAME_BYTES;
                    if (ok) {
                        atomic_fetch_add(&mic_frames, 1);
                        captured_frames++;
                    }
                }
                preroll_count = 0;
                atomic_store(&capture_pending, false);
                atomic_store(&listen_after, now_ms());
                atomic_store(&recording, ok);
                xSemaphoreGive(send_lock);
                if (!ok)
                    retry();
                else {
                    ui_mode(false);
                    ui_show_guide();
                    ui_set("RECORDING", "Speak now. Release OK: send");
                }
            } else {
                cancel_pending_capture();
                retry();
            }
        } else if (c.type == CMD_PTT_END) {
            cancel_pending_capture();
            if (atomic_exchange(&ptt_held, false)) {
                atomic_store(&recording, false);
                atomic_store(&released_at, now_ms());
                atomic_store(&output_timing_pending, true);
                if (!send_json("{\"type\":\"ptt_end\"}"))
                    retry();
                else if (!atomic_load(&menu_active))
                    ui_set("THINKING", "Waiting for reply...");
            }
        } else if (c.type == CMD_DOWN) {
            cancel_pending_capture();
            bool live = !atomic_load(&live_mode);
            atomic_store(&live_mode, live);
            atomic_store(&ptt_held, false);
            atomic_store(&output_timing_pending, false);
            mute();
            if (atomic_load(&ready) || atomic_load(&starting)) {
                atomic_store(&full_duplex, live && atomic_load(&session_full_duplex));
                atomic_store(&awaiting_clear, true);
                if (!send_json(live ? "{\"type\":\"mode\",\"mode\":\"continuous\"}"
                                    : "{\"type\":\"mode\",\"mode\":\"ptt\"}"))
                    retry();
            } else if (live) {
                atomic_store(&wanted, true);
                atomic_store(&attempts, 0);
                atomic_store(&sticky_error, false);
                start_session();
            }
            show_home();
        } else if (c.type == CMD_STOP) {
            stop_session();
            show_home();
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
        } else if (c.type == CMD_APPLY) {
            apply_device_settings();
            if (atomic_load(&menu_active))
                menu_refresh();
            else if (!device_busy())
                show_home();
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
        if (!atomic_load(&capture_pending) &&
            (!atomic_load(&recording) || !atomic_load(&connected) ||
             (int32_t)(now_ms() - atomic_load(&listen_after)) < 0))
            continue;
        xSemaphoreTake(send_lock, portMAX_DELAY);
        if (atomic_load(&capture_pending)) {
            if (preroll_count < PREROLL_FRAMES)
                memcpy(preroll[preroll_count++], frame, FRAME_BYTES);
            else {
                atomic_store(&capture_pending, false);
                enqueue(CMD_FAULT, 0);
            }
            if (atomic_load(&capture_pending) &&
                now_ms() - atomic_load(&capture_started) >= PTT_HOLD_MS &&
                !atomic_exchange(&accept_requested, true))
                enqueue(CMD_PTT_BEGIN, atomic_load(&capture_started));
        } else if (atomic_load(&recording) && atomic_load(&connected)) {
            int sent = esp_websocket_client_send_bin(ws, (const char *)frame, FRAME_BYTES,
                                                     pdMS_TO_TICKS(1000));
            if (sent == FRAME_BYTES) {
                atomic_fetch_add(&mic_frames, 1);
                if (atomic_load(&ptt_held) && ++captured_frames >= 3000) {
                    atomic_store(&recording, false);
                    enqueue(CMD_PTT_END, 0);
                }
            } else {
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
        if (!atomic_load(&ready) || (atomic_load(&recording) && !atomic_load(&full_duplex)) ||
            atomic_load(&awaiting_clear)) {
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
        if (have && (!atomic_load(&recording) || atomic_load(&full_duplex)) &&
            !atomic_load(&awaiting_clear)) {
            last_write = now;
            atomic_store(&last_output_at, now);
            atomic_fetch_add(&speaker_frames, 1);
            if (atomic_exchange(&output_timing_pending, false))
                ESP_LOGI(TAG, "PTT release_to_speaker_ms=%u", now - atomic_load(&released_at));
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
            enqueue(CMD_NEW_START, 0);
        else if (!strcmp(line, "live"))
            enqueue(CMD_DOWN, 0);
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
        if (atomic_load(&ptt_held) && now_ms() - atomic_load(&capture_started) > 60000) {
            enqueue(CMD_PTT_END, 0);
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
    atomic_store(&live_mode, false);
    ui_init();
    ui_mode(false);
    ui_show_guide();
    playback = xQueueCreate(PLAYBACK_FRAMES, sizeof(audio_frame_t));
    commands = xQueueCreate(12, sizeof(command_t));
    send_lock = xSemaphoreCreateMutex();
    assert(playback && commands && send_lock);
    ESP_ERROR_CHECK(bsp_audio_init());
    ESP_ERROR_CHECK(bsp_audio_set_format(24000, 16, 1));
    bsp_audio_set_volume(settings_get().volume);
    ESP_ERROR_CHECK(bsp_button_init(button, NULL));
    ESP_ERROR_CHECK(xTaskCreate(command_task, "controls", 6144, NULL, 5, NULL) == pdPASS
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
