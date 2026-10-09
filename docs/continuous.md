# 语音协议 v4 与多级设置

ESP32-C3 保持 ESP-IDF C，业务服务为 Rust。默认按住说话，保留畅聊；不引入唤醒词、长期记忆和文件管理。

## 启动与设置

设备发送：

```json
{"type":"start","protocol":4,"mode":"ptt","voice":"cove","pace":0,"reply_length":0,"language":0,"duplex":0,"idle_seconds":180}
```

- `mode`：`ptt` 或 `continuous`；`duplex:1` 只在畅聊生效。
- `voice`：cove、juniper、maple、spruce、ember、vale、breeze、arbor、sol。
- `pace`：0 自然，1 慢，2 快；`reply_length`：0 简短，1 一句话，2 详细；`language`：0 普通话，1 英语，2 跟随用户。
- 音色是 Codex 原生参数，其他风格选项为模型指令。参数在会话启动时读取，下一次新对话应用改变。
- 偏好存储在 `prefs_v4`；从 v3/v2 精确读取旧四整数布局，保留音量、亮度和超时。旧 `mode` 字段保留布局兼容但不用于启动，开机总是 PTT，模式不落盘。Wi-Fi NVS 不变。

当前订阅桥使用 Codex CLI 0.159.2。`thread/realtime/listVoices` 返回 v1/v2 两组；该版本 `core/src/realtime_conversation.rs` 的 `validate_realtime_voice` 将 **V3 映射到 V1**，不能把公开 API 的音色直接填入。来源：[对应版本实现](https://github.com/openai/codex/blob/rust-v0.159.2/codex-rs/core/src/realtime_conversation.rs)。

上行每帧 960 bytes（24 kHz、s16le、mono、20 ms）；协议 v2/v3/v4 下行前加 `u32 little-endian epoch`，共 964 bytes。过期音频丢弃。旧协议 1 的 PTT 脚本继续接收未加代号的 960 字节音频和转写。协议 1/3 继续兼容原先松手后才重放输入的行为。

## 按住说话

1. OK 按下立即进入本地预录，约 200 ms 确认有效按住；短碰只取消本地预录，不打开设置，不新建会话。16 帧／320 ms 缓冲保留首音节。首次有效按住发送 start，再发 ptt_start，按序发送开头缓存和后续录音；接通期间也可以上传。
2. 服务器入口最多 3008 条消息，覆盖启动期间按下键再转 PTT 的情况。媒体就绪后，`PushToTalk` 按 20 ms 时钟持续向 AudioWorklet 送入录音，不等松手。丢失时钟不累加逐帧延迟。
3. 模型在按住期间提前产生的音频进入服务器 `ReplyBuffer`，设备此时不收到播放帧。若输入静音后再次说话，丢弃尚未播出的早期回复并引导模型停止旧回答。
4. 松开发 ptt_end，停止上行；未发送的少量输入继续排空，再留 600 ms 给 worklet 尾部。`submitted` 仅表示已松手。缓存回复按 20 ms 时钟输出，保留开头；不同播放代号之间等待设备排空确认，避免队列溢出或尾音丢失。PTT 结束后麦克风保持关闭。
5. 再次有效按住清空旧播放、旧输入和旧回复，保留同一上游会话。结束和断线销毁内存缓存，没有音频落盘。

一轮最多 3000 个输入帧，即 60 秒，计数包括已发送的帧；固件达到此值自动松手，服务器拒绝超限。输入积压和回复缓冲各有 3000 帧上限，单份约 2.88 MB，在服务器分配。Live 此通道没有手动 commit/response-create，PTT 的松手控制的是应用播放，不保证模型到松手才开始推理。它消除了旧版完整录音重放的线性等待，首次接通和上游响应耗时仍然存在。

日志仅记录耗时／数量：媒体建立耗时、松手时输入与回复积压、首个模型音频、首个下发音频，以及设备的 `release_to_speaker_ms`。不会记录录音或转写。服务器下发时间不含设备 240 ms 起播缓冲。

## 畅聊与播放排空

1. status:ready 后发 listen，打开输入。
2. 第一个模型非静音帧发 playback_begin，更新代号；半双工关闭输入，全双工保持输入。
3. 连续 650 ms 无非静音输出发 playback_end。设备队列 32 × 20 ms、预填充 12 帧，短回复等待上限 350 ms。
4. 队列及 I2S DMA 排空后再等保护时间，设备发 playback_drained。服务仅接受当前代号，半双工恢复输入。PTT 此时仍等待按键，不自动开麦。
5. 句中长停顿之后如有新输出，会更新代号并丢弃旧排空确认。

进入设置发送 pause，畅聊暂停输入，正在播放的回复可以播完。返回发送 resume，根据播放状态恢复。全双工为实验项：同时收音和播放；收到新用户 turn.started 时清旧输出并引导停止旧回复。没有增加 AEC；靠回声小的使用环境降低误触发概率，不能把此选项视为已验证的消回声算法。

## 打断与新对话

`{"type":"mode","mode":"continuous"}` 开启畅聊，`mode:ptt` 回到按住说话；两者清除旧播放并切换输入，不新建 Codex thread。畅聊中 ptt_start 同样转入 PTT。模式变化与新对话是不同操作，音色／全双工等会话参数仍在下一次新对话应用。

打断立即清空本地音频，并用 `thread/realtime/appendText`、role:developer 发送固定的应用控制指令。上游停止依赖模型遵循指令；本地静音、代号过滤是确定的。旧输出至少出现 200 ms 静音边界且有新的用户话音后才允许新输出，避免旧回答再次响起。

新对话先 stop，等待服务发 **stopped** 后再 start。stopped 在旧任务终止后发出，即使旧任务已经自然结束/不存在也会回复，避免用户在待机新建时无限等待。创建全新临时 Codex thread、浏览器和 WebRTC；不注入旧转写/history。配置切换不伪装成清空屏幕。

## 按键与菜单

- 聊天页：UP 打开设置，DOWN 开启／关闭畅聊；按住 OK 转为 PTT，松开提交，短碰无操作。屏幕唤醒和该次按键操作一起执行，不启用双键。
- 菜单：UP/DOWN 移动或调整，短按 OK 选择，长按 OK 返回上级、顶层退出。按下时绑定页面，长按返回后松手不会变成录音或选择。
- 菜单：Conversation / Voice / Device / Network，每层保留 Back，音色长名单按选择分页显示。音量放在 Device → Volume。
- Wi-Fi 配网：扫码加入热点、扫码网页；短按 OK 翻页，长按 OK 重试旧网络。
- 电源为独立硬件键，公开 BSP 仅有 GPIO0 上的三功能键，没有软件电源短按事件。

## 检查边界

Rust 测试覆盖播放代号、打断顺序、新旧 PTT 输入策略、60 秒总量上限、有界回复及跨代号排空。`scripts/test_session.py` 启动真实 Rust 服务与假 Codex/媒体进程，验证启动录音、新协议边录边传、松手前不播放、完整回复、同一 thread 切模式、音色/指令、新建上下文、全双工、菜单暂停/恢复和停止确认。C 主机回归覆盖按键绑定和菜单返回路径。测试不调用真实订阅，不能证明实际模型响应时间、实机听感或全双工效果。
