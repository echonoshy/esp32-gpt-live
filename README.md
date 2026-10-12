# mic-on

基于 ESP32 的穿戴式实时语音终端，通过 GPT-Live 进行日常对话。

mic-on 当前使用 FoloToy AI Passport，通过 Wi-Fi 连接 Rust 服务，由服务器上已登录的 Codex 创建 GPT-Live 语音会话。支持「VOICE」和「LIVE」，可通过 Codex 原生工具联网查询、获取时间、计算和读写上下文文件；不自动保存日常音频或逐字转写。

GitHub：[echonoshy/mic-on](https://github.com/echonoshy/mic-on)。

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

这不是公开 GPT-Live API key 接入。使用的是 Codex 实验性 `thread/realtime/*`、v3、WebRTC 路径；最初在 Codex CLI 0.159.2 + ChatGPT Pro 上验证；当前服务器安装 Codex CLI 0.162.1。需要保留 Codex 登录，后续版本和账户权限变化需要重新验证。语音消耗订阅用量，不代表无限量或零消耗。

## 使用

1. 开机进入**首页**，依次为 **VOICE / LIVE / 设置**。上／下键选择，短按 OK 进入。
2. **VOICE**：首页选中 VOICE 后短按 OK，进入页面并立即开始录音，再按一次发送；录音最长 60 秒自动发送。思考或播放时短按 OK 停止当前回复，再按可开始下一次录音。
3. **LIVE**：进入并接通后直接开口，持续双向收音与播放，助手说话时也可插话；短按 OK 停止当前回复。
4. **长按 OK 约 0.6 秒返回**：对话页返回首页；设置页返回上一级，设置顶层返回首页。松手不重复触发选择。返回首页会取消未发送的录音、停止当前回复并暂停收音，保留已有对话上下文；从首页重新选择模式继续。
5. **对话页上／下键**：音量 +5／−5，长按连续调整，松手保存。首页和设置内则移动选项。无须同时按两个键。
6. **设置**提供音色、设备设置、网络设置、重置上下文。重置须确认；电源键保持硬件开关机。

**0.8.0 新增三入口首页与单键导航。** 「VOICE」使用显式的开始／发送操作，长按 OK 统一返回。「LIVE」沿用全双工音频：不等待静音或设备排空才开放输入。返回首页或设置后不收音、不播放，重新选择模式沿用同一会话；断线、会话超时和明确重置仍会结束上下文。

停止回复会清空旧播放，阻止迟到的旧音频重新播放，等待新的用户输入和助手输出。主动停止用应用控制指令引导上游停止，本地播放抑制独立生效。0.10.5 起，LIVE 收到新一轮非空用户转写时也会清理设备旧播放，并抑制旧助手尾音，直到新一轮助手增量输出恢复；自然插话不会追加停止指令。没有新增 AEC，不能保证排除扬声器回声误识别；插话延迟包含上游识别与网络传输，真实插话、音质与按键手感仍待实机验收。

可选音色为 **Cove、Juniper、Maple、Spruce、Ember、Vale、Breeze、Arbor、Sol**。这是 Codex 0.159.2 的 v3 原生音色名单，与公开 API 的音色不同。0.7.3 起声音设置只保留音色；语速、回答长短和语言不再设定默认偏好或注入系统提示词，可直接在聊天中提出要求。旧版已保存的风格选项会被忽略。0.7.6 起，设备菜单或手机设置页保存不同音色后，会自动清空原上下文并用新音色开启对话，保留VOICE／LIVE模式；无需另点重置上下文。重复选择相同音色、取消或保存失败不会重开。

独立电源键由硬件控制，约 0.5 秒开机、约 2 秒关机；公开引脚/BSP 没有供固件读取的电源短按事件，设置从首页进入。依据见 [硬件说明](docs/hardware.md)。

0.10.5 起，设备桥连接后先在服务器准备一个账号与临时 thread，不开启实时语音、不上传麦克风，也不提前选择音色；开始对话时复用准备结果，失败或进程退出则按需重新创建。返回首页保留的已有会话继续复用；完整结束后再准备下一次。首次开机尚未准备完成、WebRTC 和音色确认仍需要等待。

VOICE沿用边录边传：模型提前产生的回复只放在服务器内存，按 OK 发送后再按顺序播放。0.8.0 从短按释放后明确开始收音，移除旧版用于区分短按／按住的 200 ms 判断与 320 ms 预录缓存。首次接通、上游处理、网络和起播缓冲仍需要时间。没有唤醒词；明确保存的文件摘要可跨会话保留，默认 3 分钟无对话结束，单次最长 30 分钟；结束、新对话或断线会丢弃内存音频。

**设备界面统一英文，使用黑底、奶油白与洋红／薄荷绿／淡紫点缀。** 首页显示 `VOICE / LIVE / SETTINGS`，选中项使用更大的粗斜体标题；自定义 `mic-on` 字标保留洋红色 i 点，末尾 n 无缺口。标题采用 Barlow Condensed，正文采用 Barlow Semi Condensed，辅助信息采用 IBM Plex Mono，字体与字标均保存在 Flash，LVGL 静态池保持 24 KiB。顶部显示电量与音量，VOICE 上方只保留贯穿屏幕的长分隔线。Voice / Live 中央图案随实际音量变化，菜单保持静态。界面记录与验证范围见 [UI 说明](docs/ui-remix.md)。

音量曲线由驱动默认的 55 → −22.5 dB 调整为 55 → −7.8 dB，codec 最高为 0 dB。播放端另加最多 2 倍（约 +6 dB）语音增益，每个 20 ms 帧先检测峰值并收回过量增益，将 PCM 峰值限制在 30000 以内；增益缓慢恢复，避免直接削波，不新增音频缓存等待。默认音量 70；升级时一次性把原先非静音且小于 70 的音量提升至 70，其他设置保留，之后按用户设置保存。数字限幅不保证功放或扬声器在最大音量下无失真，实际响度与听感需实机确认。

### 设备自动更新与手机设置

**0.10.7 新增设备按钮更新：** 联网后进入 SETTINGS → DEVICE → Firmware update → Check / install latest，确认后自动检查版本、下载、校验、安装并重启。最新版本提示 UP TO DATE，失败保留当前固件并允许重试。旧设备需要先用现有网页 OTA 或 USB 安装一次 0.10.7；当前设备已通过 USB 完成首次安装，现已安装 0.10.9，包含更新页面修复和菜单整理；未做功能测试。

设备设置集中在 SETTINGS：DEVICE 调整音量、亮度、熄屏时间和自动结束时间；Network → Connect Wi-Fi 进入扫码配网，扫描、点选网络并验证连接。0.10.9 删除重复的 Web setup / update 菜单项。参数保存到 NVS，重启后保留；聊天模式从首页选择。

无线升级：SETTINGS → DEVICE → Firmware update → Check / install latest。联网时设备自动下载、校验、安装并重启，无需网页选文件。旧固件的网页上传方式仅作为历史维护说明；0.10.9 已移除开启网页上传窗口的菜单入口。

第一次从第一阶段升级到双槽布局必须通过 USB 更新分区表、bootloader、应用与 OTA 初始数据；以后上传应用 `.bin` 即可。不要上传 bootloader 或合并的全片镜像。

### Wi-Fi 配置

最多保存 **8 个 2.4 GHz 网络**（例如家里、公司和手机热点）。新增网络保留其他网络；同名保存更新密码并移到尝试顺序最前。已保存列表可在手机设置页或配网页查看，点 FORGET 删除；存满时提示先删除，不自动淘汰。旧版单个 Wi-Fi 和旧 `ripple` 命名空间均兼容，首次新增时一起保存到多网络列表。

开机按最近配置的顺序逐个尝试，每个网络最多等待约 10 秒取得 IP，失败就换下一个；已连通时保持连接，断开后先重试当前网络，再尝试其他已保存网络。首次全部失败后，自动开放 `mic-on-XXXX` 热点并显示二维码，同时后台继续重试（手机正在连接配网热点时暂停，避免干扰填写）；稍后打开已保存的手机热点也能恢复（连通后自动重启进入首页）。也可从首页 SETTINGS → NETWORK → Connect Wi-Fi 主动进入配网；主动配网时暂停自动连接，方便编辑。

1. 用手机系统相机扫描设备 **JOIN HOTSPOT** 二维码，确认加入 `mic-on-XXXX` 配网热点；该热点没有互联网，保持连接。
2. 手机加入后设备自动显示下一步。优先使用手机弹出的配网页；若没有弹出，用屏幕上的备用二维码打开 `http://192.168.4.1/`，短按 OK 仍可切换二维码。
3. 页面自动扫描附近 **2.4 GHz Wi-Fi**，点选网络、输入密码，点击 CONNECT；开放网络不要求密码。找不到网络时可以刷新或手动输入隐藏网络名称。
4. 保持页面打开，等待连接结果。取得 IP 后才保存，密码/认证失败、找不到网络或超时可在原页重试，已有网络保留。
5. 连接成功后点击 FINISH & RESTART，设备重启，手机切回日常 Wi-Fi。配置当前手机自己的热点时使用第二部手机或电脑操作配网页，让目标热点在验证期间保持开启。

以上流程已经包含在当前设备安装的 0.10.9 中，功能尚未实机验收。实现、OTA 首次升级注意点和验证边界见 [配网与 OTA 说明](docs/wifi-ota.md)。

常用操作都显示在设备屏幕，手机页面使用英文。配网热点提供本地 DNS 和 HTTP 配网页面；不同手机的自动弹页行为可能不同，因此保留第二张二维码和手动地址。

USB 配网不会把密码放进命令参数或源码：

```bash
uv venv .tools/python
uv pip install --python .tools/python/bin/python pyserial esptool websockets
.tools/python/bin/python scripts/configure_wifi.py --port /dev/cu.usbmodem1101
```

## 服务端

需要 Rust 1.88+、已经通过 ChatGPT 登录的 Codex CLI，以及可运行的 Chromium。监听端口 8787。当前服务器为 **140.143.229.103**，部署目录为 `/home/lake/workspaces/mic-on`，systemd 用户服务名为 `mic-on-voice.service`。固件连接 `ws://140.143.229.103:8787/device`。迁移差异、运行依赖和验证边界见 [当前部署说明](docs/deployment.md)。

```bash
cargo build --release --locked
cp .env.example .env
# 填好个人设备令牌和本机可执行文件路径
# 当前服务器的 Chromium 启动脚本见 docs/deployment.md
set -a
. ./.env
set +a
./target/release/passport-voice
```

`GET /health` 可查看服务和设备连接状态，不创建语音会话。`/device` 使用一个个人随机令牌，并限制同时一个设备；没有用户系统、数据库或 OAuth 管理页面。Codex 凭据始终留在服务器。当前设备链路使用 `ws://`，用于此个人原型；需要加密传输时可在前面部署 TLS 反向代理。

部署为用户服务：

```bash
mkdir -p ~/.config/systemd/user
cp scripts/mic-on-voice.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mic-on-voice.service
loginctl enable-linger "$USER"
journalctl --user -u mic-on-voice.service -f
```

服务不自动保存日常音频或逐字转写。用户要求保存的上下文文件会保留在 `VOICE_CONTEXT_DIR`（默认 `data/context`），新会话读取 `CONTEXT.md`。设置 `RUST_LOG=passport_voice=info` 可记录连接、错误及工具活动类型，不记录查询、命令或文件内容；`.env`、设备令牌、上下文数据、测试录音与构建结果均不进入 Git。

### 语音工具与上下文文件

直接说“现在上海几点”“计算 123.45 乘以 6.7”“上网查一下……”或“记住我们正在做 mic-on”。GPT-Live 将这些请求委派给同一会话的 Codex app-server，完成后用语音回答。联网使用 Codex 内置 web search；时间、计算和文件读写使用内置命令与文件工具，不接第三方服务或新增 API key。服务器需要 `date` 和 `python3`，默认时区为 `Asia/Shanghai`，可用 `VOICE_TIMEZONE` 修改。

记忆直接保存为 Markdown 文档。`CONTEXT.md` 是跨会话摘要（UTF-8、最多 16 KiB），保存“用户怎么称呼助手”“助手怎么称呼用户”和少量重要事项；明确约定或修改称呼时会委派后台保存，未约定的不猜测。用户要求记住、记录或保存的内容，以及明确确认且有后续价值的重要决定，可以简短记录；普通闲聊、假设和本轮表达要求不自动记忆，不归档整段对话。

较长内容按请求整理到 `notes/` 下的 Markdown 笔记，在 `CONTEXT.md` 留下文件名和简要说明；已有笔记可继续补充。简单待办按请求写入 `notes/todos.md`，使用 `- [ ]` / `- [x]` 表示未完成／已完成，日期仅作记录，不提供定时提醒。文件按需创建，没有数据库、账号系统或自动总结服务；这些行为由现有语音委派和文件工具完成，写入后读回核验才报告成功。

重置上下文会清空当前聊天，已保存文件仍在；说“忘掉刚才保存的那条”会要求后台清理相关笔记和摘要引用，“清空已保存的记忆”可删除保存内容。没有保存的旧聊天无法恢复。助手定位为简单查询、记事和文档整理，不承担大型开发、长时间研究或复杂自动化。

命令使用 Codex 的 `mic-on` 权限配置，只开放上下文工作目录读写及系统工具必需的读取路径；命令网络关闭，联网检索由 Codex 托管工具执行。应用、插件、MCP 和定时提醒未启用。详见 [工具实现与验证](docs/voice-tools.md)。

## 固件

当前固件源码版本为 **0.10.9**，已刷入设备版本为 **0.10.9**，服务端版本为 **0.9.4**，音频协议仍为 **8**。固件版本来自 `firmware/CMakeLists.txt`，设备实际版本通过局域网 `/api/info` 的 `version` 查询；服务器 `/health` 的 `version` 只代表服务端。2026-10-12 已编译并刷入 0.10.5，Flash 读回一致。0.10.6 增加扫码配网引导、附近 Wi-Fi 扫描点选、连接结果确认与 OTA 重启版本确认，这些功能已包含在当前安装的 0.10.9 中，尚未实机验收。0.10.7 在此基础上增加设备按钮自动升级，服务 0.9.4 提供私有签名固件发布接口。自然插话与启动优化已完成代码及离线回归，实机语音验收因用户当前不方便而暂停，不能把刷写和离线通过当作体验验收；见 [验证记录](docs/validation.md)。

目标为 ESP32-C3、8 MB Flash、无 PSRAM 的 FoloToy AI Passport。ESP-IDF 固定为 **5.5.3**；Managed Components 的精确版本在 `firmware/dependencies.lock`。

项目对外名称为 `mic-on`。为兼容已部署服务和设备的 OTA 校验，服务程序仍名为 `passport-voice`，固件内部项目名及产物仍为 `passport-gpt-live` / `passport-gpt-live.bin`；已有设备设置继续使用原 NVS 命名空间。

```bash
cp firmware/main/device_config.example.h firmware/main/device_config.h
# 在 device_config.h 设置 VOICE_URL，DEVICE_TOKEN 与服务器一致
. /path/to/esp-idf/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
```

当前固件保持 NVS 位于 `0x9000`、PHY 位于 `0xf000`。`ota_0` 从 `0x10000` 开始，`ota_1` 从 `0x390000` 开始，每槽 `0x380000` 字节；OTA 数据位于 `0x710000`。首次迁移写入 `build/ota_data_initial.bin` 到 OTA 数据分区，不擦除 Wi-Fi NVS。真实设备覆盖前已有完整 Flash 备份。

固件仅嵌入静态界面用字的 4 bpp 英文字形子集，字形数据保存在 Flash，LVGL 静态内存池仍为 24 KB。生成的字体 C 文件已入库，普通固件编译不需要安装字体工具。更换字体后运行 `scripts/build_ui_fonts.py` 重新生成；字体来源、许可和完整构建方法见 [字体说明](firmware/main/fonts/README.md)。二维码仅在配网或设置入口显示；常规对话页面没有持续滚动或动画。

音频采样为 24 kHz、单声道，每帧 480 个采样。当前协议 v7 沿用 v5 的 ADPCM：每个上行包 244 字节，一条消息合并 1–4 包；下行每条 248 字节（含 4 字节音频代号）；媒体桥内部为 960 字节 PCM。协议详情见 [对话状态与边界](docs/continuous.md)。服务端兼容旧版 PTT 协议的松手后上传行为，便于保留旧测试脚本。

0.7.2 的播放队列保存 ADPCM 压缩包，容量 128 帧／2.56 秒，仅比原来 32 帧 PCM 队列多 1280 字节。0.10.1 起，VOICE（chat）与 LIVE 的首次起播和欠载恢复统一以 25 帧／500 ms 为目标。短回复收到结束标记可提前开播；未结束但缺少后续帧时，VOICE 等待上限保持 1500 ms、LIVE 保持 650 ms，避免已有音频无限等待。预缓冲目标不是固定额外延迟，容量上限也不是每次等待时间；恢复到达的语音按序播放，不再在积压超过半秒时大段丢弃，只有 2.56 秒队列真正满时才淘汰最旧帧。静音与播放排空不控制收音。协议 7 每次上行最多合并 4 个 20 ms 包，新增采集等待最多约 60 ms，点击发送时先排空尾包再发送结束信号；小包数降至约原来的四分之一。协议 8 的下行同样合并最多 4 个带独立 epoch 的包（248–992 bytes），最长合包等待 60 ms，控制消息前先排空尾包。服务器分开处理设备的读取与写入，LIVE 上行使用有界 3 秒队列按 20 ms 节奏送入媒体桥，避免重传后的突发收包撑满 32 帧媒体队列。设备发送超时 3 秒、服务端写入超时 5 秒，只在阻塞时生效。

0.10.3 起，上行采集与网络发送分任务运行，用固定 48 帧队列吸收短时发送阻塞；500 ms 播放目标保持不变。WebSocket 部分写入会继续补齐剩余字节，临时不可写在同一个超时期限内重试，同一次故障只安排一次恢复。VOICE 的尾帧仍先于结束信号发送；取消和模式切换会清理旧代次采音。诊断中的 `capture_dropped`／`capture_overruns` 分别记录 LIVE 极端拥塞丢弃旧输入和 VOICE 录音超限取消，详见 [性能与实机验证](docs/performance.md)。

网络采用 12 KiB TCP 接收窗口、16 KiB 发送缓冲、TCP_NODELAY 和有界队列。暂时的语音错误以 2/4/8 秒退避重试，连续失败 3 次后提示短按 OK 重试；登录/额度问题不自动循环重试。断网会重建会话，屏幕显示恢复状态。同一会话内的正常轮换与打断不会重建会话。

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

语音测试会消耗订阅额度。USB 调试命令为 `connect`（当前模式连接）、`live`（切换LIVE）、`stop`。`probe_voice.py` 使用兼容的第一版 PTT 协议，输出 WAV、转写和非静音指标；只有录音文件大小不能证明语音成功。按键／菜单的主机端 C 回归见 `scripts/test_controls.c` 和 `scripts/test_menu.c`。真实 LVGL 界面离屏渲染、文字边界与切页内存检查见 `scripts/ui_preview/README.md`。

实测结果和未完成验收在 [验证记录](docs/validation.md)。

## 来源

- [FoloToy AI Passport 官方仓库](https://github.com/FoloToy/ai-passport)
- [官方硬件引脚](https://github.com/FoloToy/ai-passport/blob/main/components/bsp/include/bsp_pins.h)
- [ChatGPT Voice](https://learn.chatgpt.com/docs/features/voice)
- [Codex App Server](https://learn.chatgpt.com/docs/app-server)

BSP 从本机已有 AI Passport 移植中提取，保留原 MIT 许可，详见 [硬件来源](docs/hardware.md)。


### 0.9.2 音色应用确认

切换音色时仍建立新语音会话。媒体通道打开后，先在实际订阅 V3 数据通道发送所选 `audio.output.voice`，收到 `session.updated` 后才报告 ready、允许麦克风输入与回复播放。10 秒无确认、拒绝或返回音色不匹配均进入错误状态，不回退为默认音色后继续显示成功。设备核对 ready 中的音色是否与 NVS 选择一致；诊断字段 `requested_voice` 为已选音色，`active_voice` 为已确认会话音色，未确认或离线为 null。切换过程中屏幕显示 `APPLYING VOICE`。

这是针对当前 Codex 0.159.2 订阅 V3 通道的已实测启动握手，不应照搬到公开 GPT-Live API：公开 API 要求启动时选声，不能用普通会话更新修改音色。确认事件代表设置命令被接受，真实听感仍需设备试听。

## 文件与产物管理

- `assets/brand/`：保留的品牌设计源图；固件实际使用的转换资源在 `firmware/main/assets/`。
- `firmware/main/fonts/`：固件所需字形及字体许可，正常编译不依赖字体生成工具。
- `artifacts/`：本机验证记录、最终预览、刷机包与回退备份，不提交到 GitHub。逐帧 PPM 和历史构建缓存可在验证后删除；文档中的历史路径是当时的本地记录，不保证仍有全部中间文件。
- `target/`、`.tools/npm-cache/`、`__pycache__/`、`output/`：可重新生成的本地缓存或临时输出。`.tools/` 中的实际工具链和字体源文件继续保留在本机。
- `.env`、设备私有配置与 `data/`：仅限本机使用，始终忽略。
