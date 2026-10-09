# 内存与音频链路诊断（0.6.0）

## 2026-10-09 发现的问题

ESP32-C3 没有 PSRAM。原实现每 20 ms 发送 960 字节 PCM（24 kHz、16 bit、单声道），单向约 48 KB/s，全双工约 96 KB/s，不含网络开销。TCP 收发缓冲各 32 KiB，Wi-Fi 动态收发缓冲各 32 个；这些网络缓冲与任务、播放队列共用有限的内部 RAM。

故障时服务器服务仍在运行，Rust 进程 RSS 约 9 MB，服务器可用内存约 200 GB。设备 USB 日志显示堆只剩约 6–8 KB，网络重连超时，屏幕按键仍能响应。设备到服务器的 TCP RTT 约 340–440 ms，Wi-Fi 信号约 -41 dBm。

为排除 GPT 响应速度的影响，新增了只传输全零音频的双向网络诊断：不启动 Codex、不创建语音会话、不上传麦克风内容、不播放测试声。仅增加诊断的旧 PCM 版本（0.5.1）在约 3 秒内复现断连，USB 记录剩余堆 6004 字节。HTTP 状态接口已不可达，未取得故障瞬间的分配失败计数；因此结论是网络负载下的严重内存压力，不能据此认定某个 malloc 泄漏。

只压缩音频的中间版本已避免快速耗尽内存，但上行只有约 25 帧/秒。ESP-IDF 5.5 的 WebSocket 发送把帧头和内容拆成两次 TCP 写入，TCP_NODELAY 下占用更多小包队列项。高 RTT 使未确认小包积压，lwIP 的可写低水位会阻塞麦克风发送。这也会拖延控制消息。

## 修复

- 设备链路改为 IMA/DVI ADPCM：每 20 ms 上行 244 字节，下行 248 字节（含 epoch），约为原来四分之一；服务器内部及 GPT 媒体接口继续使用 24 kHz PCM。
- 按住说话的 16 帧预录缓存从 15360 字节降至 3904 字节。播放队列仍为 32 帧（640 ms），预缓冲仍为 12 帧（240 ms），短回复最多等待 350 ms。
- TCP 接收窗口 12 KiB、发送缓冲 16 KiB；TCP oversize 使用四分之一 MSS；Wi-Fi 动态 RX/TX 各 12 个；WebSocket 启用独立发送锁。
- `ws_send.c` 通过链接器包装 `esp_transport_ws_send_raw`，把设备的 WebSocket 帧头与内容合成一次写入。使用固定 1208 字节工作区，由 WebSocket 客户端发送锁串行保护；控制帧仍有随机掩码，部分写入按连接错误处理。其他 transport 或超过上限的帧退回原实现。升级 ESP-IDF/WebSocket 组件后必须重跑封包测试和链路诊断。
- 服务端忽略会话接收端关闭之后迟到的录音帧，避免会话结束误断开整条设备连接。
- 正常播放结束不再计入播放欠载次数。

ADPCM 是有损语音编码，CPU 需求小，不能描述为无损。实际音色与听感由用户检查。增加播放缓存会额外占内存、提高首音延迟，不能代替降低传输压力。

## 协议 5

`start`/`probe` 携带 `protocol: 5`。上行每条二进制消息为一个独立 ADPCM 包；下行前置 4 字节小端 epoch。ADPCM 包格式：

| 偏移 | 长度 | 含义 |
| --- | --- | --- |
| 0 | 2 | 本包编码之前的 predictor，i16 little endian |
| 2 | 1 | 本包编码之前的 step index，0–88 |
| 3 | 1 | 保留，必须为 0 |
| 4 | 240 | 480 个采样，先高 nibble 后低 nibble |

包携带独立解码所需的初始状态，丢弃旧 epoch 不会影响后续包。服务端继续兼容协议 1–4 的原 PCM 格式。新固件应搭配 0.6.0 或更高版本服务端。

## 诊断与回归

`GET /api/info` 的 `diagnostics` 提供当前/历史最低空闲堆、最大空闲块、分配失败次数、发送耗时/错误、收发帧数、播放欠载和 Wi-Fi RSSI。累计字段从启动开始计数；`heap_min` 不会随释放回升，比较多轮时应看当前空闲堆和累计计数差值。

设备空闲时运行：

```sh
python scripts/netcheck.py --url http://192.168.7.17 --out artifacts/diagnostics/netcheck.json
```

脚本先获取设备 nonce，再调用 `POST /api/netcheck`，双向传输 12 秒全零音频并采样状态。音频数据由固件清零，服务端验证静音；结束后等待播放队列排空，设备有 20 秒保护性停止。用户操作也会中止检查。日志不保存 nonce 或录音，输出默认在 Git 忽略的 `artifacts/` 下。检查不会调用 GPT，不代表完整真实对话已验收。

离线回归：

```sh
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
cargo build --locked
python scripts/test_session.py
python scripts/test_adpcm.py
python scripts/test_ws_send.py
```

C 编码器和 Rust 编码器使用 `scripts/fixtures/adpcm.json` 的独立 Python 3.9 `audioop` 参考向量交叉校验，涵盖静音、平滑音频、斜坡和饱和极值。封包测试覆盖短帧/扩展长度、控制帧/分片、部分写入错误和回退路径。进程回归使用模拟 Codex/媒体端，不访问 OpenAI。

本轮没有调用真实 GPT 语音；网络固有 RTT、上游会话建立与模型回答耗时仍需与用户实际体验分开评估。短时压力检查也不能排除所有长时间运行问题。

## 实机对比结果

两轮最终链路检查均为双向 12 秒静音传输，随后排空播放队列，没有重启设备：

| 项目 | 第一轮 | 第二轮 |
| --- | --- | --- |
| 传输期间每秒采样的空闲堆 | 38524–41264 B | 39260–42876 B |
| 结束后空闲堆 | 60036 B | 60048 B |
| 启动至检查完成的最低堆（非每秒采样） | 23912 B | 23912 B |
| 设备上行速率 | 约 50 帧/秒 | 约 50 帧/秒 |
| 服务端发出的下行 / 设备播放 | 600 / 600 帧 | 600 / 600 帧 |
| 分配失败 / 发送错误 / 丢帧 / 欠载 | 均为 0 | 均为 0 |
| 单帧发送最长耗时（启动累计） | 4 ms | 4 ms |

原始日志保留于本机 `artifacts/diagnostics/baseline-netcheck.json`、`baseline-usb.log`、`coalesced-netcheck-{1,2}.json`。中间版本的 `compressed-netcheck-{1,2}.json` 用于定位合包修复前的发送阻塞，不代表最终版本结果。TCP RTT 会随时变化：诊断过程中读到约 220–440 ms，因此这些是同设备、同网络路径的现场结果，并非控制变量实验。

### 仍需区分的 Wi-Fi 问题

完成两轮传输后的空闲观察又捕捉到 Wi-Fi 断连。USB 日志显示 `Wi-Fi disconnected, reason=6`，紧接着 TCP `Software caused connection abort`，随后重新关联并恢复连接。当时空闲堆约 61 KB，没有分配失败。ESP-IDF 中原因码 6 对应 `WIFI_REASON_NOT_AUTHED`（Class 2 frame from non-authenticated station），表示无线认证/关联状态异常；仅凭此码不能断定是路由器配置、AP 状态还是兼容性问题。Wi-Fi 省电已关闭，信号约 -41 dBm。

这类断线独立于已修复的音频缓冲内存压力，不能宣称所有掉线已根治。`diagnostics.wifi_disconnects` 和 `wifi_last_reason` 记录首次连网完成后的断线次数与最近原因码；USB 健康日志同时记录堆最低值、分配失败与网络发送耗时。若真实使用仍偶发整段停顿，可先用手机 2.4 GHz 热点做同设备对照，再决定是否调整现有 AP；本轮未修改路由器配置或保存的 Wi-Fi。
