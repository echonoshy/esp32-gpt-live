# AI Passport × GPT-Live

个人使用的订阅语音终端。FoloToy AI Passport 通过 Wi-Fi 连接 Rust 服务，由服务器上已登录的 Codex 创建 GPT-Live 语音会话。

## 实现

```text
AI Passport (ESP-IDF C, ES8311, 24 kHz PCM)
  ⇄ WebSocket / 20 ms 音频帧
Rust 服务 (Axum / Tokio, 单设备、按键会话)
  ⇄ 本机 Chromium AudioWorklet / WebRTC
GPT-Live
  ↑ Codex app-server 的 ChatGPT 订阅登录与会话管理
```

服务端业务和会话管理使用 Rust。浏览器仅承担 WebRTC 编解码及音频时钟，没有 Python 服务端依赖。Python 脚本仅用于烧录、USB 配网和测试。

这不是公开 GPT-Live API key 接入。使用的是 Codex 实验性 `thread/realtime/*`、v3、WebRTC 路径；目前在 Codex CLI 0.159.2 + ChatGPT Pro 上验证。需要保留 Codex 登录，后续版本和账户权限变化需要重新验证。语音消耗订阅用量，不代表无限量或零消耗。

## 使用

1. 给设备供电。设备自动连接已保存的 2.4 GHz Wi-Fi 和语音服务器。
2. 短按 **OK** 创建会话，等屏幕显示 **READY**。
3. **按住 OK 说话，说完及时松开听回复**。再次按住可以继续说话。
4. **DOWN** 结束会话；**UP** 在 25–75% 范围循环调节音量。
5. 120 秒无语音活动会自动关闭会话；单次会话最长 30 分钟。重新按 OK 开启新的会话。

同一次会话中保留对话上下文。结束或网络断线后重新开始，不承诺跨会话记忆。第一版不带待办、笔记、唤醒词、回声消除或离线识别。

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

当前固件保持 NVS 位于 `0x9000`，只写 bootloader、分区表和 factory 应用，不全片擦除 Wi-Fi 信息。真实设备首次覆盖前已备份完整 Flash 到本机忽略目录。

协议为 24 kHz、16 位小端、单声道 PCM，每帧 480 个采样、960 字节。固件播放队列最多 640 ms，起播和欠载后恢复都先预填充 240 ms；短回复最多等 350 ms 就开始播放。浏览器输入缓存最多 500 ms。网络拥塞时清理积压或结束连接，避免无限堆积。为约 300 ms 的公网往返延迟配置了 32 KiB TCP 收发窗口和双端 TCP_NODELAY；发送等待最多 1 秒，单方向 PCM 约 48 KB/s。自定义 WS transport 将控制帧交给客户端，保证 PONG 能更新心跳状态。

第一版用按键隔离收音和播放，语音模型仍根据语音停顿判断何时回答。说完后应及时松开 OK；长时间按住不说话会屏蔽期间的回复。真正免按键、可自然打断的双工模式需要回声消除和进一步适配。

硬件引脚只在 `firmware/components/bsp/include/bsp_pins.h` 定义。保留现有音频时钟、共享 I2C、单 ADC1、显示 DMA 和 USB 日志配置。

## 验证

```bash
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
.tools/python/bin/python scripts/probe_voice.py /path/to/24k-mono.wav --out artifacts/new-probe
.tools/python/bin/python scripts/serial_monitor.py
```

语音测试会消耗订阅额度。USB 调试命令是 `connect`、`record`、`listen`、`stop` 和离线 `tone`。`probe_voice.py` 与真机使用相同设备协议，输出 WAV、转写和非静音指标；只有录音文件大小不能证明语音成功。

实测结果和未完成验收在 [验证记录](docs/validation.md)。

## 来源

- [FoloToy AI Passport 官方仓库](https://github.com/FoloToy/ai-passport)
- [官方硬件引脚](https://github.com/FoloToy/ai-passport/blob/main/components/bsp/include/bsp_pins.h)
- [ChatGPT Voice](https://learn.chatgpt.com/docs/features/voice)
- [Codex App Server](https://learn.chatgpt.com/docs/app-server)

BSP 从本机已有 AI Passport 移植中提取，保留原 MIT 许可，详见 [硬件来源](docs/hardware.md)。
