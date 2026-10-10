#!/usr/bin/env python3
"""Run the actual connection task with deterministic Wi-Fi/FreeRTOS events."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'firmware/main/wifi.c').read_text()
task = source[source.index('static void connection_task('):source.index('static int hex_value(')]
preamble = r'''
#include "wifi_profiles.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define WIFI_CONNECTED 1
#define WIFI_DISCONNECTED 2
#define WIFI_EXHAUSTED 4
#define WIFI_RETRY 8
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_AUTH_OPEN 0
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
typedef unsigned EventBits_t;
typedef struct { int num; } wifi_sta_list_t;
static int ap_clients, setup_pauses;
static int esp_wifi_ap_get_sta_list(wifi_sta_list_t *clients) { clients->num = ap_clients; return ESP_OK; }
typedef struct { struct { unsigned char ssid[32], password[64]; struct {int authmode;} threshold; } sta; } wifi_config_t;
static wifi_profiles_t saved_profiles;
static int s_wifi_events, position, limit, cleanup, exhausted, resumes, stable, restarted;
static bool provisioning;
static unsigned outcomes[16];
static const char *expected[16];
static jmp_buf done;
static unsigned flags;
static bool voice_wifi_provisioning(void) { return provisioning; }
static void ui_set(const char *a, const char *b) { (void)a; (void)b; }
static void vTaskDelay(unsigned ticks) { (void)ticks; if (ap_clients) { ap_clients--; setup_pauses++; } }
static void esp_restart(void) { restarted++; longjmp(done, 1); }
static unsigned xEventGroupClearBits(int group, unsigned bits)
{ (void)group; flags &= ~bits; return flags; }
static unsigned xEventGroupSetBits(int group, unsigned bits)
{ (void)group; flags |= bits; if (bits & WIFI_EXHAUSTED) exhausted++; return flags; }
static unsigned xEventGroupWaitBits(int group, unsigned bits, int clear, int all, unsigned timeout)
{
    (void)group; (void)clear; (void)all;
    if (bits == (WIFI_CONNECTED | WIFI_DISCONNECTED)) return outcomes[position - 1];
    if (bits == WIFI_RETRY) { resumes++; provisioning = true; ap_clients = 3; return WIFI_RETRY; }
    assert(bits == WIFI_DISCONNECTED);
    if (timeout == portMAX_DELAY) stable++;
    else cleanup++;
    return WIFI_DISCONNECTED;
}
static int esp_wifi_set_config(int interface, const wifi_config_t *config)
{
    (void)interface;
    assert(!ap_clients);
    if (position == limit) longjmp(done, 1);
    assert(!strcmp((const char *)config->sta.ssid, expected[position]));
    assert(config->sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
    position++;
    return ESP_OK;
}
static int esp_wifi_connect(void) { return ESP_OK; }
static int esp_wifi_disconnect(void) { return ESP_OK; }
'''
tests = r'''
static void reset(void)
{
    position = cleanup = exhausted = resumes = stable = restarted = 0;
    provisioning = false; flags = 0; ap_clients = setup_pauses = 0;
    memset(&saved_profiles, 0, sizeof(saved_profiles));
    saved_profiles.count = 3;
    const char *names[] = {"Home", "Company", "Phone"};
    for (int i = 0; i < 3; i++) {
        strcpy(saved_profiles.entries[i].ssid, names[i]);
        strcpy(saved_profiles.entries[i].password, "12345678");
    }
}
int main(void)
{
    // Home absent, company authentication rejected, phone connects. Stay there
    // until an actual disconnect; then retry it once before moving to Home.
    reset(); limit = 5;
    const char *order[] = {"Home", "Company", "Phone", "Phone", "Home"};
    for (int i = 0; i < limit; i++) { expected[i] = order[i]; outcomes[i] = WIFI_DISCONNECTED; }
    outcomes[2] = outcomes[4] = WIFI_CONNECTED;
    if (!setjmp(done)) connection_task(NULL);
    assert(position == 5 && stable == 2 && exhausted == 0 && restarted == 0);

    // A reachable AP that never provides DHCP times out and gets disconnected.
    reset(); limit = 2;
    expected[0] = "Home"; expected[1] = "Company";
    outcomes[0] = 0; outcomes[1] = WIFI_CONNECTED;
    if (!setjmp(done)) connection_task(NULL);
    assert(cleanup == 1 && stable == 1);

    // Everything unavailable: publish fallback once, retry saved profiles in
    // AP+STA mode, and restart normal boot after the hotspot is turned on later.
    reset(); limit = 6;
    for (int i = 0; i < limit; i++) {
        expected[i] = saved_profiles.entries[i % 3].ssid;
        outcomes[i] = WIFI_DISCONNECTED;
    }
    outcomes[5] = WIFI_CONNECTED;
    if (!setjmp(done)) connection_task(NULL);
    assert(position == 6 && exhausted == 1 && resumes == 1 && restarted == 1 && setup_pauses == 3);

    // One legacy profile also keeps retrying without dividing by zero.
    reset(); saved_profiles.count = 1; limit = 2;
    expected[0] = expected[1] = "Home";
    outcomes[0] = WIFI_DISCONNECTED; outcomes[1] = WIFI_CONNECTED;
    if (!setjmp(done)) connection_task(NULL);
    assert(exhausted == 1 && restarted == 1);
    puts("PASS: missing AP/auth failure fallthrough, DHCP timeout cleanup, stable connection, disconnect roaming, fallback AP, late hotspot, setup-client pause, legacy single profile");
}
'''
with tempfile.TemporaryDirectory(prefix='mic-on-wifi-test-') as folder:
    path = Path(folder)
    (path / 'test.c').write_text(preamble + task + tests)
    compiler = [os.environ.get('CC', 'cc')]
    sdk = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX15.2.sdk')
    if sdk.exists():
        compiler += ['-isysroot', str(sdk)]
    subprocess.run(compiler + ['-std=c11', '-Wall', '-Wextra', '-Werror',
                              '-fsanitize=address,undefined', '-I'+str(root/'scripts/fixtures/wifi'),
                              '-I'+str(root/'firmware/main'), str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)

# Both embedded browser scripts must still parse after the network controls change.
html = (root/'firmware/main/settings.html').read_text()
page = source[source.index('static const char page[]'):source.index('httpd_resp_set_type(request, "text/html')]
import ast
html += ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', page))
for script in re.findall(r'<script>(.*?)</script>', html, re.S):
    subprocess.run(['node', '--check'], input=script, text=True, check=True)
print('PASS: settings and captive-portal JavaScript syntax')
