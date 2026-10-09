#include "menu.h"
#include "settings.h"
#include "ui.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    ROOT,
    CHAT,
    DEVICE,
    NETWORK,
    PICK_VOICE,
    EDIT_VOLUME,
    EDIT_BRIGHT,
    EDIT_SCREEN,
    EDIT_IDLE,
    CONFIRM_NEW,
    CONFIRM_WIFI,
    INFO
} page_t;
static page_t page;
static int selected;
static bool live_mode;
static const int sleeps[] = {15, 30, 60, 120, 300, 600};
static const int timeouts[] = {60, 180, 300, 600, 900};
static int count(void)
{
    switch (page) {
    case ROOT:
        return 6;
    case DEVICE:
    case CHAT:
        return 5;
    case NETWORK:
        return 3;
    case PICK_VOICE:
        return VOICE_COUNT + 1;
    case CONFIRM_NEW:
    case CONFIRM_WIFI:
        return 2;
    default:
        return 1;
    }
}
static void enter(page_t next)
{
    page = next;
    selected = 0;
}
static bool editing(void)
{
    return page >= EDIT_VOLUME && page <= EDIT_IDLE;
}
void menu_refresh(void)
{
    settings_t p = settings_get();
    char rows[10][64] = {{0}}, title[48] = "设置", hint[128] = "上下选择，OK 确认";
    switch (page) {
    case ROOT:
        snprintf(title, sizeof(title), "快捷菜单");
        snprintf(rows[0], sizeof(rows[0]), "%s畅聊", live_mode ? "退出" : "开启");
        snprintf(rows[1], sizeof(rows[1]), "对话管理");
        snprintf(rows[2], sizeof(rows[2]), "音色：%s", settings_voice(p.voice));
        snprintf(rows[3], sizeof(rows[3]), "设备设置");
        snprintf(rows[4], sizeof(rows[4]), "网络设置");
        snprintf(rows[5], sizeof(rows[5]), "返回聊天");
        break;
    case CHAT:
        snprintf(title, sizeof(title), "对话管理");
        snprintf(rows[0], sizeof(rows[0]), "新对话（清空上下文）");
        snprintf(rows[1], sizeof(rows[1]), "结束对话");
        snprintf(rows[2], sizeof(rows[2]), "畅聊：支持随时插话");
        snprintf(rows[3], sizeof(rows[3]), "空闲结束：%d 秒", p.idle_seconds);
        snprintf(rows[4], sizeof(rows[4]), "返回上一级");
        snprintf(hint, sizeof(hint), "切换模式保留上下文");
        break;
    case DEVICE:
        snprintf(title, sizeof(title), "设备设置");
        snprintf(rows[0], sizeof(rows[0]), "音量：%d", p.volume);
        snprintf(rows[1], sizeof(rows[1]), "亮度：%d", p.brightness);
        snprintf(rows[2], sizeof(rows[2]), "自动息屏：%d 秒", p.screen_seconds);
        snprintf(rows[3], sizeof(rows[3]), "手机设置与固件更新");
        snprintf(rows[4], sizeof(rows[4]), "返回上一级");
        break;
    case NETWORK:
        snprintf(title, sizeof(title), "网络设置");
        snprintf(rows[0], sizeof(rows[0]), "扫码连接无线网络");
        snprintf(rows[1], sizeof(rows[1]), "查看连接信息");
        snprintf(rows[2], sizeof(rows[2]), "返回上一级");
        break;
    case PICK_VOICE:
        snprintf(title, sizeof(title), "选择音色");
        for (int i = 0; i < VOICE_COUNT; i++)
            snprintf(rows[i], sizeof(rows[i]), "%s%s", i == p.voice ? "* " : "", settings_voice(i));
        snprintf(rows[VOICE_COUNT], sizeof(rows[VOICE_COUNT]), "返回上一级");
        snprintf(hint, sizeof(hint), "OK 保存，新对话生效");
        break;
    case CONFIRM_NEW:
        snprintf(title, sizeof(title), "清空上下文？");
        snprintf(rows[0], sizeof(rows[0]), "确认开启新对话");
        snprintf(rows[1], sizeof(rows[1]), "返回上一级");
        snprintf(hint, sizeof(hint), "将结束当前对话");
        break;
    case CONFIRM_WIFI:
        snprintf(title, sizeof(title), "重新配置网络？");
        snprintf(rows[0], sizeof(rows[0]), "重启并进入配网");
        snprintf(rows[1], sizeof(rows[1]), "返回上一级");
        snprintf(hint, sizeof(hint), "保留原网络，结束对话");
        break;
    case INFO: {
        char ip[20];
        voice_wifi_ip(ip, sizeof(ip));
        snprintf(title, sizeof(title), "连接信息");
        snprintf(rows[0], sizeof(rows[0]), "%s", voice_wifi_connected() ? ip : "无线网络未连接");
        snprintf(hint, sizeof(hint), "短按 OK 返回");
        break;
    }
    default:
        snprintf(title, sizeof(title), "调整设置");
        snprintf(rows[0], sizeof(rows[0]), "%s：%d",
                 page == EDIT_VOLUME   ? "音量"
                 : page == EDIT_BRIGHT ? "亮度"
                 : page == EDIT_SCREEN ? "息屏秒数"
                                       : "空闲秒数",
                 page == EDIT_VOLUME   ? p.volume
                 : page == EDIT_BRIGHT ? p.brightness
                 : page == EDIT_SCREEN ? p.screen_seconds
                                       : p.idle_seconds);
        snprintf(hint, sizeof(hint), "上下调整，OK 返回");
        break;
    }
    const char *items[10];
    // Keep the back gesture visible on every level, including edit/confirmation pages.
    char *line = strchr(hint, '\n');
    if (line)
        *line = 0;
    strncat(hint, "\n长按 OK 返回上一级", sizeof(hint) - strlen(hint) - 1);
    for (int i = 0; i < 10; i++)
        items[i] = rows[i];
    ui_show_menu(title, items, count(), selected, hint);
}
void menu_open(bool live)
{
    live_mode = live;
    enter(ROOT);
    menu_refresh();
}
menu_action_t menu_back(void)
{
    switch (page) {
    case ROOT:
        return MENU_CLOSE;
    case PICK_VOICE:
        enter(ROOT);
        selected = 2;
        break;
    case EDIT_VOLUME:
    case EDIT_BRIGHT:
    case EDIT_SCREEN:
        enter(DEVICE);
        break;
    case EDIT_IDLE:
    case CONFIRM_NEW:
        enter(CHAT);
        break;
    case CONFIRM_WIFI:
    case INFO:
        enter(NETWORK);
        break;
    default:
        enter(ROOT);
        break;
    }
    menu_refresh();
    return MENU_NONE;
}
static int cycle(int v, const int *values, int n, int direction)
{
    int i = 0;
    while (i < n - 1 && values[i] < v)
        i++;
    return values[(i + direction + n) % n];
}
menu_action_t menu_key(int key)
{
    settings_t p = settings_get();
    bool changed = false;
    if (key && editing()) {
        int delta = key < 0 ? 1 : -1;
        if (page == EDIT_VOLUME) {
            p.volume += delta * 5;
            if (p.volume < 0)
                p.volume = 0;
            if (p.volume > 100)
                p.volume = 100;
        }
        if (page == EDIT_BRIGHT) {
            p.brightness += delta * 5;
            if (p.brightness < 5)
                p.brightness = 5;
            if (p.brightness > 100)
                p.brightness = 100;
        }
        if (page == EDIT_SCREEN)
            p.screen_seconds = cycle(p.screen_seconds, sleeps, 6, delta);
        if (page == EDIT_IDLE)
            p.idle_seconds = cycle(p.idle_seconds, timeouts, 5, delta);
        changed = true;
    } else if (key)
        selected = (selected + key + count()) % count();
    else
        switch (page) {
        case ROOT:
            if (selected == 0)
                return MENU_LIVE;
            if (selected == 5)
                return MENU_CLOSE;
            enter((page_t[]){CHAT, PICK_VOICE, DEVICE, NETWORK}[selected - 1]);
            if (page == PICK_VOICE)
                selected = p.voice;
            break;
        case CHAT:
            if (selected == 0)
                enter(CONFIRM_NEW);
            else if (selected == 1)
                return MENU_STOP;
            else if (selected == 2) {
                // Informational: live capture is never gated by playback.
            } else if (selected == 3)
                enter(EDIT_IDLE);
            else
                enter(ROOT);
            break;
        case DEVICE:
            if (selected < 3)
                enter((page_t[]){EDIT_VOLUME, EDIT_BRIGHT, EDIT_SCREEN}[selected]);
            else if (selected == 3)
                return MENU_PHONE;
            else
                enter(ROOT);
            break;
        case NETWORK:
            enter((page_t[]){CONFIRM_WIFI, INFO, ROOT}[selected]);
            break;
        case PICK_VOICE:
            if (selected < VOICE_COUNT) {
                p.voice = selected;
                changed = true;
            }
            enter(ROOT);
            selected = 2;
            break;
        case CONFIRM_NEW:
            if (selected == 0)
                return MENU_NEW_CHAT;
            enter(CHAT);
            break;
        case CONFIRM_WIFI:
            if (selected == 0)
                return MENU_WIFI;
            enter(NETWORK);
            break;
        case INFO:
            enter(NETWORK);
            break;
        case EDIT_IDLE:
            enter(CHAT);
            break;
        default:
            enter(DEVICE);
            break;
        }
    if (changed && settings_save(p) != ESP_OK) {
        const char *message[] = {"设置保存失败", "请按 OK 重试"};
        ui_show_menu("保存失败", message, 2, 0, "设置未更改");
        return MENU_NONE;
    }
    menu_refresh();
    return MENU_NONE;
}
