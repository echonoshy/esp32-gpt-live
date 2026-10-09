# AI Passport × GPT-Live

个人使用的订阅语音终端。FoloToy AI Passport 通过 Wi-Fi 连接 Rust 服务，由服务器上已登录的 Codex 创建 GPT-Live 语音会话。

## 实现

```text
AI Passport (ESP-IDF C, ES8311, 24 kHz PCM)
  ⇄ WebSocket / 20 ms 音频帧
Rust 服务 (Axum / Tokio, 单设备、连续会话)
  ⇄ 本机 Chromium AudioWorklet / WebRTC
GPT-Live
  ↑ Codex app-server 的 ChatGPT 订阅登录与会话管理
```

服务端业务和会话管理使用 Rust。浏览器仅承担 WebRTC 编解码及音频时钟，没有 Python 服务端依赖。Python 脚本仅用于烧录、USB 配网和测试。

这不是公开 GPT-Live API key 接入。使用的是 Codex 实验性 `thread/realtime/*`、v3、WebRTC 路径；目前在 Codex CLI 0.159.2 + ChatGPT Pro 上验证。需要保留 Codex 登录，后续版本和账户权限变化需要重新验证。语音消耗订阅用量，不代表无限量或零消耗。

## 使用

1. 设备自动连接已保存的 2.4 GHz Wi-Fi 和服务器，开机默认**按住说话**。
2. **按住 OK**：按下即缓存首段声音，约 200 ms 确认有效按住后开始上传；**松开 OK**：结束输入，等待回复。最长 60 秒自动发送。
3. **短按 OK**：思考或播放中停止当前交互，保留上下文；空闲时只唤亮屏幕。
4. **上／下键**：音量 +5／−5，长按连续调整，松手停止，短暂显示音量；范围 0–100，0 为静音，松手保存。
5. **下键 + OK 起始一起按住约 0.8 秒**：打开快捷菜单。允许开头约 80 ms 的按键先后差；录音或调音量中途加按不打开菜单。菜单首项开启／退出畅聊。上下移动、短按 OK 选择、长按 OK 返回上级，顶层返回聊天。
6. 快捷菜单直接提供「重置上下文」，确认后清空旧对话并新建会话；「音色」直接选择声音；「设备设置」管理设备；「网络设置」配网。电源键保持硬件开关机。

**0.7.0 畅聊始终全双工**：持续收音、持续播放，助手说话期间也能听到用户。自然插话由 GPT-Live 处理，不自动追加停止指令，也不等待静音或设备排空才开放输入。进入菜单暂停收音，返回恢复。畅聊时按住 OK 转为按住说话，同一会话保留上下文；切换聊天模式不需要新建对话。

短按停止会清空旧播放，阻止迟到的旧音频重新播放，等待新的用户输入和助手输出。主动停止用应用控制指令引导上游停止，本地播放抑制独立生效。没有新增 AEC；真实插话、音质和组合键电压窗口仍由用户实机确认。

可选音色为 **Cove、Juniper、Maple、Spruce、Ember、Vale、Breeze、Arbor、Sol**。这是 Codex 0.159.2 的 v3 原生音色名单，与公开 API 的音色不同。0.7.3 起声音设置只保留音色；语速、回答长短和语言不再设定默认偏好或注入系统提示词，可直接在聊天中提出要求。旧版已保存的风格选项会被忽略。0.7.6 起，设备菜单或手机设置页保存不同音色后，会自动清空原上下文并用新音色开启对话，保留按住说话／畅聊模式；无需另点重置上下文。重复选择相同音色、取消或保存失败不会重开。

独立电源键由硬件控制，约 0.5 秒开机、约 2 秒关机；公开引脚/BSP 没有供固件读取的电源短按事件，因此设置入口使用下键 + OK 组合键。依据见 [硬件说明](docs/hardware.md)。

0.5.0 的按住说话采用边录边传；模型提前产生的回复只放在服务器内存，松开后按顺序播放。消除了旧版「松开后按原速重放整段录音」带来的额外等待；首次接通、上游处理、网络和起播缓冲仍需要时间。设备只增加 320 ms 有界开头缓存。没有唤醒词或长期记忆，默认 3 分钟无对话结束，单次最长 30 分钟。结束、新对话或断线会丢弃内存音频。

**0.7.6 使用参考 Anthropic 风格的轻量中文界面，不显示字幕、滚动文字或动态波形。** 暖米白底、炭灰正文、陶土橙色标记；大标题和状态使用 Noto Serif CJK SC / Lora 衬线字形，操作文字用 Noto Sans CJK SC / Poppins 无衬线字形，产品名用 16 px Lora。中文正文 18 px、标题 22 px，全部为 Flash 常驻 2 bpp 字形子集，无运行时字体加载。主屏状态以细线强调，菜单选中行用浅陶土底色；手机设置页采用同一方向并提供系统字体回退。顶部显示电量、音量和聊天模式，下方保留操作指南。主屏和扫码页不显示内网 IP；地址仅在「网络设置 → 查看连接信息」按需查看。未知电量显示 `--`，低电量用深陶土色提示。待机默认 60 秒息屏，按键唤亮。

音量曲线由驱动默认的 55 → −22.5 dB 调整为 55 → −7.8 dB，最高为 0 dB；没有对 PCM 强行放大削波。默认音量 70；升级时一次性把原先非静音且小于 70 的音量提升至 70，其他设置保留，之后按用户设置保存。

### 手机设置与无线升级

手机与设备连接同一个 Wi-Fi，从「设备设置 → 手机设置与固件更新」确认后扫码打开设置页；进入前会提示结束并清空当前对话，取消则保留上下文。可以调整音量、亮度、熄屏时间、音色、自动结束时间，以及更换 Wi-Fi。参数保存到 NVS，重启后保留；聊天模式由按键即时切换，开机总是默认按住说话。没有账号系统；页面使用设备本次启动的随机 nonce 防止跨站误操作。

无线升级：设备设置 → 手机设置与固件更新 → 确认进入，自动结束当前会话并开放 5 分钟升级窗口 → 手机设置页选择 **passport-gpt-live.bin** → 上传。设备校验镜像、写入另一固件槽，然后重启；原固件保留。新固件在完成本地硬件、任务、网络/配网页面初始化后确认启动；确认前重启会回退上一槽。它不检测主观音质，也不保证回退启动确认之后发生的错误。

第一次从第一阶段升级到双槽布局必须通过 USB 更新分区表、bootloader、应用与 OTA 初始数据；以后上传应用 `.bin` 即可。不要上传 bootloader 或合并的全片镜像。

### Wi-Fi 配置

优先连接保存在 NVS 的网络；首次迁移仍兼容旧 `ripple` 命名空间。开机 30 秒连不上网络时，自动开放 `GPT-Passport-XXXX` 热点并显示二维码；也可下键 + OK → 网络设置 → 扫码连接无线网络 主动进入。

1. 扫描设备第一张二维码，加入 `GPT-Passport-XXXX` 热点。该热点没有互联网，手机提示时选择保持连接。
2. 手机通常会自动打开配网页面；若没有，在设备上短按 OK，扫描第二张二维码打开 `http://192.168.4.1/`。
3. 在手机填写目标 **2.4 GHz** Wi-Fi 名称和密码，保存后设备重启连接。手机可切回原 Wi-Fi。
4. 配网页面中短按 OK 可切换两张二维码，长按 OK 可取消并重试原网络。原网络不会因打开配网模式而被擦除。

常用操作都显示在设备屏幕，手机页面仍使用中文。配网热点提供本地 DNS 和 HTTP 配网页面；不同手机的自动弹页行为可能不同，因此保留第二张二维码和手动地址。

USB 配网不会把密码放进命令参数或源码：

```bash
uv venv .tools/python
uv pip install --python .tools/python/bin/python pyserial esptool websockets
.tools/python/bin/python scripts/configure_wifi.py --port /dev/cu.usbmodem1101
```

## 服务端

需要 Rust 1.88+、已经通过 ChatGPT 登录的 Codex CLI，以及可运行的 Chromium。当前部署目录：`/home/lake/workspaces/esp32-gpt-live`，监听端口 8787。

```bash
cargo build --release --locked
cp .env.example .env
# 填好个人设备令牌和本机可执行文件路径
set -a
. ./.env
set +a
./target/release/passport-voice
```

`GET /health` 可查看服务和设备连接状态，不创建语音会话。`/device` 使用一个个人随机令牌，并限制同时一个设备；没有用户系统、数据库或 OAuth 管理页面。Codex 凭据始终留在服务器。当前设备链路使用 `ws://`，用于此个人原型；需要加密传输时可在前面部署 TLS 反向代理。

部署为用户服务：

```bash
mkdir -p ~/.config/systemd/user
cp scripts/passport-voice.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now passport-voice.service
loginctl enable-linger "$USER"
journalctl --user -u passport-voice.service -f
```

服务不保存日常音频或转写。设置 `RUST_LOG=passport_voice=info` 可记录连接与错误；`.env`、设备令牌、测试录音与构建结果均不进入 Git。

## 固件

目标为 ESP32-C3、8 MB Flash、无 PSRAM 的 FoloToy AI Passport。ESP-IDF 固定为 **5.5.3**；Managed Components 的精确版本在 `firmware/dependencies.lock`。


```bash
cp firmware/main/device_config.example.h firmware/main/device_config.h
# 在 device_config.h 设置 VOICE_URL，DEVICE_TOKEN 与服务器一致
. /path/to/esp-idf/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

当前固件保持 NVS 位于 `0x9000`、PHY 位于 `0xf000`。`ota_0` 从 `0x10000` 开始，`ota_1` 从 `0x390000` 开始，每槽 `0x380000` 字节；OTA 数据位于 `0x710000`。首次迁移写入 `build/ota_data_initial.bin` 到 OTA 数据分区，不擦除 Wi-Fi NVS。真实设备覆盖前已有完整 Flash 备份。

固件仅嵌入静态界面用字的 2 bpp 思源黑体 / Noto Sans CJK SC 子集，字形数据保存在 Flash，LVGL 静态内存池仍为 24 KB。生成的字体 C 文件已入库，普通固件编译不需要安装字体工具。界面文案新增汉字后，使用 `lv_font_conv 1.5.3` 和原始 `NotoSansCJKsc-Regular.otf` 运行 `python scripts/build_ui_fonts.py --font /path/to/NotoSansCJKsc-Regular.otf --converter /path/to/lv_font_conv.js` 重新生成；字体许可见 `firmware/main/fonts/OFL.txt`。二维码仅在配网或设置入口显示；常规对话页面没有持续滚动或动画。

音频采样为 24 kHz、单声道，每帧 480 个采样。当前协议 v7 沿用 v5 的 ADPCM：每个上行包 244 字节，一条消息合并 1–4 包；下行每条 248 字节（含 4 字节音频代号）；媒体桥内部为 960 字节 PCM。协议详情见 [对话状态与边界](docs/continuous.md)。服务端兼容旧版 PTT 协议的松手后上传行为，便于保留旧测试脚本。

0.7.2 的播放队列保存 ADPCM 压缩包，容量 128 帧／2.56 秒，仅比原来 32 帧 PCM 队列多 1280 字节。按住模式通常预缓冲 800 ms，欠载后本段提高到 1200 ms；短回复收到结束标记可提前开播，缺少后续帧时等待上限 1500 ms。正常网络下首音缓冲相比旧版多约 560 ms，容量上限不是固定等待。畅聊仍采用 240 ms 预缓冲，积压超过 480 ms 时丢弃最旧部分至约 240 ms；静音与播放排空不控制收音。协议 7 每次上行最多合并 4 个 20 ms 包，新增采集等待最多约 60 ms，松手先排空尾包再发送结束信号；小包数降至约原来的四分之一。设备发送超时 3 秒、服务端写入超时 5 秒，只在阻塞时生效。

网络采用 12 KiB TCP 接收窗口、16 KiB 发送缓冲、TCP_NODELAY 和有界队列。暂时的语音错误以 2/4/8 秒退避重试，连续失败 3 次后提示按住 OK 重试；登录/额度问题不自动循环重试。断网会重建会话，屏幕显示恢复状态。同一会话内的正常轮换与打断不会重建会话。

硬件引脚只在 `firmware/components/bsp/include/bsp_pins.h` 定义。保留现有音频时钟、共享 I2C、单 ADC1、显示 DMA 和 USB 日志配置。

## 验证

```bash
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
node scripts/test_worklet.cjs
cargo build --locked
.tools/python/bin/python scripts/test_session.py # 全离线，不连接 OpenAI
.tools/python/bin/python scripts/probe_voice.py /path/to/24k-mono.wav --out artifacts/new-probe
.tools/python/bin/python scripts/serial_monitor.py
```

语音测试会消耗订阅额度。USB 调试命令为 `connect`（当前模式连接）、`live`（切换畅聊）、`stop`。`probe_voice.py` 使用兼容的第一版 PTT 协议，输出 WAV、转写和非静音指标；只有录音文件大小不能证明语音成功。按键／菜单的主机端 C 回归见 `scripts/test_controls.c` 和 `scripts/test_menu.c`。

实测结果和未完成验收在 [验证记录](docs/validation.md)。

## 来源

- [FoloToy AI Passport 官方仓库](https://github.com/FoloToy/ai-passport)
- [官方硬件引脚](https://github.com/FoloToy/ai-passport/blob/main/components/bsp/include/bsp_pins.h)
- [ChatGPT Voice](https://learn.chatgpt.com/docs/features/voice)
- [Codex App Server](https://learn.chatgpt.com/docs/app-server)

BSP 从本机已有 AI Passport 移植中提取，保留原 MIT 许可，详见 [硬件来源](docs/hardware.md)。
