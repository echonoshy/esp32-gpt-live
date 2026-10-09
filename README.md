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

1. 给设备供电，自动连接已保存的 Wi-Fi 和语音服务器。
2. **短按一次 OK** 创建会话。显示「正在听」后直接说话，无需按住按键。
3. 说完停顿，等待回复；播完后设备自动恢复收音，可以继续下一句。同一个会话中保留上下文。
4. 回复中按 **OK** 立即静音并打断，显示「正在听」后继续说话。
5. **DOWN** 结束会话；**UP** 循环调整音量并保存；长按 **UP 3 秒** 显示设置地址，并开放 5 分钟的升级窗口。
6. 默认 3 分钟无对话自动结束，可在设置页调整。单次会话最长 30 分钟，到期后按 OK 重新开始。

这版采用轮流收音、播放的连续对话模式。播放期间直接讲话不会上传，插话时按一下 OK。没有唤醒词，也没有跨会话记忆；结束或断网后新会话不会恢复此前上下文。

设备显示中文状态、整句字幕、电池百分比和音量。字幕在服务返回整句转写后更新，最多 180 个字符，长内容自动滚动；不是逐字实时字幕。待机默认 60 秒熄屏，按键唤亮，对话中保持亮屏。电量计读不到时显示 `--`。

### 手机设置与无线升级

手机与设备连接同一个 Wi-Fi，打开待机屏幕上的 `http://设备IP`。可以调整音量、亮度、熄屏时间、无对话自动结束时间，以及更换 Wi-Fi。参数保存到 NVS，重启后保留。没有账号系统；页面使用设备本次启动的随机 nonce 防止跨站误操作。

无线升级：先结束对话 → 长按上键 3 秒 → 手机设置页选择 **passport-gpt-live.bin** → 上传。设备校验镜像、写入另一固件槽，然后重启；原固件保留。新固件在完成本地硬件、任务、网络/配网页面初始化后确认启动；确认前重启会回退上一槽。它不检测主观音质，也不保证回退启动确认之后发生的错误。

第一次从第一阶段升级到双槽布局必须通过 USB 更新分区表、bootloader、应用与 OTA 初始数据；以后上传应用 `.bin` 即可。不要上传 bootloader 或合并的全片镜像。

### Wi-Fi 配置

使用新的 `gpt_live` NVS 命名空间；首次升级可读取旧 `ripple` 命名空间中的 Wi-Fi。不会沿用旧服务地址。保存的网络不可用时，30 秒后开启 `GPT-Passport-XXXX` 配网热点，连接后打开 `http://192.168.4.1` 填写 Wi-Fi 信息。

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

中文使用 Noto Sans CJK SC 16 px/2 bpp 字库，源文件已纳入 Git，不需要构建时下载字体；字体说明与 SIL OFL 许可在 `firmware/main/fonts/`。

协议为 24 kHz、16 位小端、单声道 PCM，每帧 480 个采样。上行 960 字节；连续模式下行是 4 字节小端音频代号加 960 字节 PCM，旧代号的音频会被丢弃。协议详情见 [连续对话状态与边界](docs/continuous.md)。服务端兼容第一版 PTT 协议，便于保留旧测试脚本。

播放队列最多 640 ms，预填充 240 ms，短回复最多等 350 ms；收到结束提示可以提早播出短尾音。服务端检测连续 650 ms 静音后请求排空，设备等待队列和 I2S DMA 播完，再加收音保护时间，才开启下一轮。上游 WebRTC 没有可靠的媒体结束包，所以长句中的停顿可能暂时切回收音；后续音频到来时会重新进入播放并拒绝过期的排空确认。

网络采用 32 KiB TCP 收发窗口、TCP_NODELAY 和有界队列。暂时的语音错误以 2/4/8 秒退避重试，连续失败 3 次后提示按 OK 重试；登录/额度问题不自动循环重试。断网会重建会话，屏幕显示恢复状态。同一会话内的正常轮换与打断不会重建会话。

硬件引脚只在 `firmware/components/bsp/include/bsp_pins.h` 定义。保留现有音频时钟、共享 I2C、单 ADC1、显示 DMA 和 USB 日志配置。

## 验证

```bash
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
node scripts/test_worklet.cjs
.tools/python/bin/python scripts/probe_voice.py /path/to/24k-mono.wav --out artifacts/new-probe
.tools/python/bin/python scripts/serial_monitor.py
```

语音测试会消耗订阅额度。第二版 USB 调试命令是 `connect`、`interrupt`、`stop`。`probe_voice.py` 使用兼容的第一版 PTT 协议，输出 WAV、转写和非静音指标；只有录音文件大小不能证明语音成功。

实测结果和未完成验收在 [验证记录](docs/validation.md)。

## 来源

- [FoloToy AI Passport 官方仓库](https://github.com/FoloToy/ai-passport)
- [官方硬件引脚](https://github.com/FoloToy/ai-passport/blob/main/components/bsp/include/bsp_pins.h)
- [ChatGPT Voice](https://learn.chatgpt.com/docs/features/voice)
- [Codex App Server](https://learn.chatgpt.com/docs/app-server)

BSP 从本机已有 AI Passport 移植中提取，保留原 MIT 许可，详见 [硬件来源](docs/hardware.md)。
