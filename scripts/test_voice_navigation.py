#!/usr/bin/env python3
"""Exercise the actual session-transition functions with deterministic host stubs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'firmware/main/main.c').read_text()
functions = []
for name, following in [('start_session', 'stop_session'), ('stop_session', 'new_conversation'),
                        ('new_conversation', 'enter_home')]:
    start = source.index('static void ' + name + '(')
    end = source.index('static void ' + following + '(', start)
    functions.append(source[start:end])
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static atomic_bool menu_active, settings_open, wanted, starting, ready, restart_pending;
static atomic_bool live_mode, full_duplex, can_listen, ptt_active, sticky_error, interaction_active;
static atomic_bool connected, recovering;
static atomic_uint session_generation;
static atomic_int active_voice, started_at;
static bool menu_notice;
static int guide_calls, start_calls, stop_calls, queued;
typedef struct { int voice, idle_seconds; } settings_t;
static settings_t prefs = { 2, 180 };
static settings_t settings_get(void) { return prefs; }
static const char *settings_voice(int i) { return i == 2 ? "maple" : "ember"; }
static int now_ms(void) { return 100; }
static void ui_mode(bool live) { (void)live; }
static void ui_show_guide(void) { guide_calls++; }
static void ui_set(const char *s, const char *d) { (void)s; (void)d; }
static void mute(void) {}
static void retry(void) { assert(false); }
static bool send_json(const char *s) {
    if (strstr(s, "\"start\"")) { start_calls++; assert(strstr(s, "maple")); }
    if (strstr(s, "\"stop\"")) stop_calls++;
    return true;
}
enum { CMD_NEW_START };
static void enqueue(int c, int e) { (void)e; assert(c == CMD_NEW_START); queued++; }
static void stop_session(void);
'''
harness += '\n'.join(functions)
harness += r'''
static void reset(void) {
    menu_active = settings_open = wanted = starting = ready = restart_pending = false;
    connected = true;
    recovering = false;
    session_generation = 0;
    guide_calls = start_calls = stop_calls = queued = 0;
}
static void acknowledge_stop(void) {
    assert(atomic_exchange(&restart_pending, false));
    wanted = true;
    start_session();
}
int main(void) {
    reset(); menu_active = true; ready = wanted = true;
    new_conversation(true);
    assert(menu_active && !settings_open && !ready && !guide_calls && stop_calls == 1);
    new_conversation(true); // Rapid changes reuse the pending stop, not a second restart.
    assert(stop_calls == 1 && !guide_calls);
    acknowledge_stop();
    assert(!start_calls && !guide_calls && menu_active);
    menu_active = false; start_session();
    assert(start_calls == 1 && guide_calls == 1 && starting);

    reset(); settings_open = true; ready = wanted = true;
    new_conversation(true); acknowledge_stop();
    assert(settings_open && !start_calls && !guide_calls);
    settings_open = false; menu_active = true; start_session();
    assert(!start_calls); // Leaving the phone page returns to settings.
    menu_active = false; start_session(); assert(start_calls == 1);

    reset(); menu_active = true; connected = false;
    new_conversation(true);
    assert(menu_active && queued == 1 && !restart_pending && !guide_calls);
    wanted = true; connected = true; start_session(); assert(!start_calls);
    menu_active = false; start_session(); assert(start_calls == 1);

    reset(); menu_active = true; new_conversation(false);
    assert(!menu_active && guide_calls == 1); // Explicit reset still opens a new chat.
    acknowledge_stop(); assert(start_calls == 1);
    puts("PASS: voice change stays in picker/phone settings; restart deferred; rapid changes, offline and explicit reset");
}
'''
with tempfile.TemporaryDirectory(prefix='mic-on-navigation-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
