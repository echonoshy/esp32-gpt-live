# mic-on

ESP32-C3 穿戴式语音终端，使用 FoloToy AI Passport，通过 Wi-Fi 连接 Rust 服务与 GPT-Live。支持按键录音的 VOICE 和持续双向对话的 LIVE，不需要 App。

仓库：[echonoshy/mic-on](https://github.com/echonoshy/mic-on)。当前服务器为 **140.143.229.103**。

## 当前状态

| 项目 | 版本 |
| --- | --- |
| 固件源码 | 0.10.10 |
| 最近确认安装的设备固件 | 0.10.9（2026-10-12） |
| 服务端 | 0.9.4 |
| 音频协议 | 8 |

0.10.10 清理已失效的网页固件上传路径，并让配网结果使用独立 WI-FI 页面；尚未部署或安装。扫码配网、按钮 OTA、自然插话与启动等待优化已实现，完整实机功能验收仍待进行。构建或刷写完成不代表体验验收，详见 [验证状态](docs/status.md)。

## 使用

1. 开机进入 **VOICE / LIVE / SETTINGS** 首页，上下键选择，短按 OK 进入。
2. **VOICE**：进入时立即开始录音，再按 OK 发送；最长录音 60 秒。思考或播放时按 OK 停止回复，再按开始下一次录音。
3. **LIVE**：接通后直接开口，收音与播放同时运行；按 OK 停止回复。自然插话依赖上游识别，没有新增回声消除，真实效果待验收。
4. **长按 OK 约 0.6 秒返回**：聊天页回首页，设置页回上一级。回首页会停止收音与播放、取消未发送录音，并保留仍有效的会话上下文。
5. 聊天页上下键调节音量；设置页上下键移动选项。电源键为硬件开关。
6. SETTINGS 提供音色、DEVICE、Network 和 Reset context。DEVICE 调整音量、亮度、熄屏时间、会话闲置时间，并提供 Firmware update。

音色包括 Cove、Juniper、Maple、Spruce、Ember、Vale、Breeze、Arbor、Sol。保存不同音色会清空当前上下文，新会话确认音色后才允许收音。语言、语速和回答长度直接在聊天中提出。

## 配网与升级

**配网**：SETTINGS → Network → Connect Wi-Fi。扫码加入 `mic-on-XXXX` 热点，打开配网页，扫描并点选 2.4 GHz 网络，输入密码。取得 IP 后才保存；成功后在手机点击 FINISH & RESTART。最多保存 8 个网络。

**升级**：联网后进入 SETTINGS → DEVICE → Firmware update → Check / install latest。设备从当前服务器检查并下载签名固件、校验、写入另一分区并重启。最新版本显示 UP TO DATE，失败保留当前固件；升级期间保持供电。

新版本必须先编译、使用现有私钥签名并发布到服务器，**只 push GitHub 源码不会自动发布固件**。当前源码已删除网页上传入口；首次安装和恢复维护使用 USB。详细步骤见 [配网与 OTA](docs/wifi-ota.md)。

## 开发与部署

服务端使用 Rust 1.88+、通过 ChatGPT 登录的 Codex CLI 与 Chromium；设备使用 ESP-IDF 5.5.3。该接入依赖 Codex 实验性 `thread/realtime/*` v3 与 WebRTC，并使用订阅额度。

```sh
cargo build --release --locked
cp .env.example .env
# 填写 DEVICE_TOKEN 和本机 Codex / Chromium 路径
set -a
. ./.env
set +a
./target/release/passport-voice
```

固件构建：

```sh
cp firmware/main/device_config.example.h firmware/main/device_config.h
# 设置 VOICE_URL 与 DEVICE_TOKEN，令牌须与服务端一致
. /path/to/esp-idf/export.sh
idf.py -C firmware build
```

`device_config.h`、`.env`、签名私钥、固件镜像、Flash 备份与上下文数据不进入 Git。内部兼容名称保留 `passport-voice`、`passport-gpt-live` 和原 NVS 布局。现有设备维护前核对当前 OTA 槽，避免使用全量 flash 命令覆盖设置或 OTA 元数据，见 [部署说明](docs/deployment.md)。

`GET /health` 只读取服务与设备连接状态，不创建语音会话。设备当前连接 `ws://140.143.229.103:8787/device`，使用个人令牌，服务同时只接纳一个设备。链路仍为未加密的个人原型连接，OTA 签名验证不加密传输。

## 工具与文档记忆

语音可调用 Codex 原生搜索、时间、计算和上下文文件工具。明确要求保存的内容记入 `CONTEXT.md` 或 `notes/`，不自动保存日常音频、逐字转写或整段聊天。重置当前聊天不删除保存的文件。简单待办可记录和完成，日期不代表定时提醒。详见 [语音工具](docs/voice-tools.md)。

## 维护资料

- [架构与音频协议](docs/architecture.md)：会话、音频缓冲、打断与恢复。
- [部署说明](docs/deployment.md)：140.143.229.103、服务运行、固件构建与 USB 维护。
- [配网与 OTA](docs/wifi-ota.md)：设备操作、签名发布与升级边界。
- [验证与测试](docs/testing.md)：离线回归和需明确授权的设备诊断。
- [验证状态](docs/status.md)：已确认结果、待验收事项与本轮 review。
- [硬件与许可](docs/hardware.md)、[设备 UI](docs/ui.md)。

构建缓存和本地记录保存在忽略目录 `.tools/`、`target/`、`firmware/build/` 与 `artifacts/`。签名私钥和回退备份不能当作普通缓存删除。字体许可与上游 BSP 许可随源码保留。
