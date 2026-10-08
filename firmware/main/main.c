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

#define SAMPLES 480
#define FRAME_BYTES (SAMPLES * sizeof(int16_t))
#define PLAYBACK_FRAMES 32       // 640 ms capacity, bounded for the C3's RAM
#define PLAYBACK_PREFILL 12      // 240 ms before starting / resuming playback
typedef struct { int16_t samples[SAMPLES]; } audio_frame_t;
typedef enum { CMD_CONNECT, CMD_PRESS, CMD_RELEASE, CMD_STOP, CMD_VOLUME, CMD_TONE } command_t;
static const char *TAG = "gpt_live";
static esp_websocket_client_handle_t ws;
static esp_transport_handle_t tcp_transport;
static QueueHandle_t playback, commands;
static SemaphoreHandle_t send_lock;
static atomic_bool connected, ready, recording, starting;
static atomic_uint mic_frames, speaker_frames, dropped_frames;
static atomic_uint playback_refills;
static atomic_int mic_peak;
static int volume = 55;

static bool control_send(const char *type)
{
    char message[64];
    int n = snprintf(message, sizeof(message), "{\"type\":\"%s\"}", type);
    if (!atomic_load(&connected)) return false;
    xSemaphoreTake(send_lock, portMAX_DELAY);
    int sent = esp_websocket_client_send_text(ws, message, n, pdMS_TO_TICKS(1000));
    xSemaphoreGive(send_lock);
    return sent == n;
}

static void ws_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        // Small, frequent audio frames must not wait for Nagle + delayed ACK.
        int no_delay = 1;
        int socket = esp_transport_get_socket(tcp_transport);
        if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0)
            ESP_LOGW(TAG, "could not enable TCP_NODELAY");
        atomic_store(&connected, true);
        ui_set("CONNECTED", "Tap OK to start a voice chat");
        ESP_LOGI(TAG, "device bridge connected");
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
        atomic_store(&connected, false); atomic_store(&ready, false);
        atomic_store(&recording, false); atomic_store(&starting, false);
        xQueueReset(playback);
        ui_set("RECONNECTING", "Checking the voice server...");
    } else if (event == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *frame = data;
        // esp_websocket_client splits a WS payload across buffer events.
        static char payload[4096];
        static size_t received;
        static int opcode;
        if (frame->op_code != 0 && frame->op_code != 1 && frame->op_code != 2) return;
        if (frame->payload_offset == 0) { received = 0; opcode = frame->op_code; }
        if (frame->payload_len <= 0 || frame->payload_len >= sizeof(payload) ||
            frame->payload_offset != received || frame->data_len < 0 ||
            received + frame->data_len > sizeof(payload) - 1) return;
        memcpy(payload + received, frame->data_ptr, frame->data_len);
        received += frame->data_len;
        if (received != frame->payload_len) return;
        if (opcode == 2 && received == FRAME_BYTES && !atomic_load(&recording) && atomic_load(&ready)) {
            if (xQueueSend(playback, payload, 0) != pdTRUE) {
                xQueueReset(playback); // discard stale playback instead of increasing latency
                atomic_fetch_add(&dropped_frames, 1);
                xQueueSend(playback, payload, 0);
            }
        } else if (opcode == 1) {
            payload[received] = 0;
            cJSON *message = cJSON_Parse(payload);
            cJSON *type = cJSON_GetObjectItem(message, "type");
            cJSON *state = cJSON_GetObjectItem(message, "state");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "status") == 0 && cJSON_IsString(state)) {
                ESP_LOGI(TAG, "session %s", state->valuestring);
                if (strcmp(state->valuestring, "ready") == 0) {
                    atomic_store(&ready, true); atomic_store(&starting, false);
                    ui_set("READY", "Hold OK and speak\nRelease to hear the reply");
                } else if (strcmp(state->valuestring, "connecting") == 0) {
                    ui_set("CONNECTING", "Opening subscription voice...");
                } else if (strcmp(state->valuestring, "idle") == 0) {
                    atomic_store(&ready, false); atomic_store(&starting, false); atomic_store(&recording, false);
                    xQueueReset(playback);
                    ui_set("IDLE", "Tap OK to start a voice chat");
                }
            } else if (cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0) {
                atomic_store(&ready, false); atomic_store(&recording, false); atomic_store(&starting, false);
                ui_set("RETRY", "Voice unavailable\nTap OK to reconnect");
                ESP_LOGW(TAG, "voice session error");
            }
            cJSON_Delete(message);
        }
    }
}

static void button(bsp_btn_t key, bsp_btn_ev_t event, void *user)
{
    command_t command;
    if (key == BSP_BTN_OK && event == BSP_BTN_PRESS) command = CMD_PRESS;
    else if (key == BSP_BTN_OK && event == BSP_BTN_RELEASE) command = CMD_RELEASE;
    else if (key == BSP_BTN_DOWN && event == BSP_BTN_CLICK) command = CMD_STOP;
    else if (key == BSP_BTN_UP && event == BSP_BTN_CLICK) command = CMD_VOLUME;
    else return;
    xQueueSend(commands, &command, 0);
}

static void command_task(void *unused)
{
    command_t command;
    for (;;) {
        xQueueReceive(commands, &command, portMAX_DELAY);
        if (command == CMD_CONNECT || (command == CMD_PRESS && !atomic_load(&ready))) {
            if (!atomic_load(&starting) && atomic_load(&connected)) {
                atomic_store(&starting, true);
                if (!control_send("start")) atomic_store(&starting, false);
                ui_set("CONNECTING", "Wait for READY, then hold OK");
            }
        } else if (command == CMD_PRESS && atomic_load(&ready)) {
            xSemaphoreTake(send_lock, portMAX_DELAY);
            const char *message = "{\"type\":\"ptt_start\"}";
            bool sent = esp_websocket_client_send_text(ws, message, strlen(message), pdMS_TO_TICKS(1000)) == strlen(message);
            atomic_store(&recording, sent);
            xQueueReset(playback);
            xSemaphoreGive(send_lock);
            ui_set("LISTENING", "Speak now...\nRelease OK when finished");
        } else if (command == CMD_RELEASE && atomic_load(&recording)) {
            // Serialize the final audio frame before the end-of-utterance marker.
            xSemaphoreTake(send_lock, portMAX_DELAY);
            atomic_store(&recording, false);
            const char *message = "{\"type\":\"ptt_end\"}";
            esp_websocket_client_send_text(ws, message, strlen(message), pdMS_TO_TICKS(1000));
            xSemaphoreGive(send_lock);
            ui_set("THINKING", "Waiting for a reply...\nHold OK to speak again");
        } else if (command == CMD_STOP) {
            atomic_store(&recording, false); atomic_store(&ready, false);
            xQueueReset(playback); control_send("stop");
            ui_set("ENDING", "Closing voice conversation...");
        } else if (command == CMD_VOLUME) {
            volume = volume >= 75 ? 25 : volume + 10;
            bsp_audio_set_volume(volume);
            ESP_LOGI(TAG, "volume %d", volume);
        } else if (command == CMD_TONE && !atomic_load(&ready) && !atomic_load(&starting)) {
            audio_frame_t tone;
            for (size_t i = 0; i < SAMPLES; ++i) tone.samples[i] = (i % 60 < 30) ? 1800 : -1800;
            for (int i = 0; i < 15; ++i) xQueueSend(playback, &tone, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "audio self-test tone queued");
        }
    }
}

static void microphone_task(void *unused)
{
    audio_frame_t frame;
    for (;;) {
        if (bsp_audio_read(&frame, FRAME_BYTES) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        int peak = 0;
        for (size_t i = 0; i < SAMPLES; ++i) { int value = abs((int)frame.samples[i]); if (value > peak) peak = value; }
        atomic_store(&mic_peak, peak);
        if (!atomic_load(&recording) || !atomic_load(&connected)) continue;
        xSemaphoreTake(send_lock, portMAX_DELAY);
        if (atomic_load(&recording)) {
            int sent = esp_websocket_client_send_bin(ws, (const char *)&frame, FRAME_BYTES, pdMS_TO_TICKS(1000));
            if (sent == FRAME_BYTES) atomic_fetch_add(&mic_frames, 1);
            else { atomic_store(&recording, false); command_t stop = CMD_STOP; xQueueSend(commands, &stop, 0); }
        }
        xSemaphoreGive(send_lock);
    }
}

static void speaker_task(void *unused)
{
    audio_frame_t frame;
    int64_t last_speech = 0;
    int64_t prefill_since = 0;
    bool playing = false;
    bool speaking = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        UBaseType_t queued = uxQueueMessagesWaiting(playback);
        memset(&frame, 0, sizeof(frame));
        if (atomic_load(&recording)) {
            playing = false; speaking = false; prefill_since = 0;
        } else {
            if (!playing && queued) {
                if (!prefill_since) prefill_since = now;
                // The deadline also lets very short answers drain at end of turn.
                if (queued >= PLAYBACK_PREFILL || now - prefill_since >= 350000) {
                    playing = true; prefill_since = 0;
                }
            }
            if (playing && xQueueReceive(playback, &frame, 0) != pdTRUE) {
                playing = false; prefill_since = 0;
                if (now - last_speech < 150000) atomic_fetch_add(&playback_refills, 1);
            }
        }
        int peak = 0;
        for (size_t i = 0; i < SAMPLES; ++i) { int value = abs((int)frame.samples[i]); if (value > peak) peak = value; }
        if (peak > 150) {
            if (!speaking) ui_set("SPEAKING", "Hold OK to speak again\nDOWN ends the conversation");
            speaking = true; last_speech = now;
            atomic_fetch_add(&speaker_frames, 1);
        } else if (speaking && now - last_speech > 700000 && !queued) {
            speaking = false;
            if (atomic_load(&ready)) ui_set("READY", "Hold OK and speak\nRelease to hear the reply");
        }
        // Keep the I2S clock running at 20 ms per frame, even during prefill.
        if (bsp_audio_write(&frame, FRAME_BYTES) != ESP_OK) {
            ESP_LOGW(TAG, "speaker write failed");
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void console_task(void *unused)
{
    // USB diagnostics allow repeatable tests without accessing the Wi-Fi secret.
    char line[256];
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20));continue;}
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (used + 1 < sizeof(line)) line[used++] = (char)ch;
            else overflow = true;
            continue;
        }
        line[used] = 0; used = 0;
        if (overflow) {overflow = false; memset(line, 0, sizeof(line));continue;}
        if (strncmp(line, "wifi ", 5) == 0) {
            cJSON *config = cJSON_Parse(line + 5);
            cJSON *ssid = cJSON_GetObjectItem(config, "ssid");
            cJSON *password = cJSON_GetObjectItem(config, "password");
            bool saved = cJSON_IsString(ssid) && cJSON_IsString(password) &&
                voice_wifi_save(ssid->valuestring, password->valuestring) == ESP_OK;
            cJSON_Delete(config);
            memset(line, 0, sizeof(line));
            puts(saved ? "WIFI_CONFIG_SAVED" : "WIFI_CONFIG_INVALID");
            fflush(stdout);
            if (saved) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
            continue;
        }
        command_t command;
        if (strncmp(line, "connect", 7) == 0) command = CMD_CONNECT;
        else if (strncmp(line, "record", 6) == 0) command = CMD_PRESS;
        else if (strncmp(line, "listen", 6) == 0) command = CMD_RELEASE;
        else if (strncmp(line, "stop", 4) == 0) command = CMD_STOP;
        else if (strcmp(line, "tone") == 0) command = CMD_TONE;
        else continue;
        xQueueSend(commands, &command, 0);
    }
}

static void health_task(void *unused)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int peak = atomic_load(&mic_peak);
        ui_level(atomic_load(&recording) ? peak / 100 : 0);
        ESP_LOGI(TAG, "health heap=%lu mic_peak=%d tx=%u speaker=%u dropped=%u buffered=%u refills=%u connected=%d ready=%d",
            (unsigned long)esp_get_free_heap_size(), peak, atomic_load(&mic_frames), atomic_load(&speaker_frames),
            atomic_load(&dropped_frames), (unsigned)uxQueueMessagesWaiting(playback), atomic_load(&playback_refills),
            atomic_load(&connected), atomic_load(&ready));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ui_init();
    playback = xQueueCreate(PLAYBACK_FRAMES, sizeof(audio_frame_t));
    commands = xQueueCreate(12, sizeof(command_t));
    send_lock = xSemaphoreCreateMutex();
    assert(playback && commands && send_lock);
    ESP_ERROR_CHECK(bsp_audio_init());
    ESP_ERROR_CHECK(bsp_audio_set_format(24000, 16, 1));
    bsp_audio_set_volume(volume);
    ESP_ERROR_CHECK(bsp_button_init(button, NULL));
    xTaskCreate(command_task, "controls", 4096, NULL, 5, NULL);
    xTaskCreate(microphone_task, "mic", 4096, NULL, 6, NULL);
    xTaskCreate(speaker_task, "speaker", 4096, NULL, 6, NULL);
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
    xTaskCreate(health_task, "health", 3072, NULL, 2, NULL);
    esp_err_t wifi = voice_wifi_start();
    if (wifi != ESP_OK) {ESP_LOGW(TAG, "Wi-Fi setup required: %s", esp_err_to_name(wifi)); return;}
    static const char headers[] = "Authorization: Bearer " DEVICE_TOKEN "\r\n";
    // Keep a transport handle so each reconnect can configure its new socket.
    assert(strncmp(VOICE_URL, "ws://", 5) == 0);
    tcp_transport = esp_transport_tcp_init();
    assert(tcp_transport);
    esp_transport_handle_t transport = esp_transport_ws_init(tcp_transport);
    assert(transport);
    const char *path = strchr(VOICE_URL + 5, '/');
    esp_transport_ws_config_t transport_config = {
        .ws_path = path ? path : "/", .headers = headers,
        // The client owns heartbeat tracking and must receive the PONG events.
        .propagate_control_frames = true,
    };
    ESP_ERROR_CHECK(esp_transport_ws_set_config(transport, &transport_config));
    esp_websocket_client_config_t config = {
        .uri = VOICE_URL, .headers = headers, .buffer_size = 1200,
        .ext_transport = transport,
        .task_stack = 6144, .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 5000, .ping_interval_sec = 15,
    };
    ws = esp_websocket_client_init(&config);
    assert(ws);
    ESP_ERROR_CHECK(esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(ws));
}
