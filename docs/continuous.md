# 语音协议 v3 与多级设置

ESP32-C3 保持 ESP-IDF C，业务服务为 Rust。默认按住说话，保留畅聊；不引入唤醒词、长期记忆和文件管理。

## 启动与设置

设备发送：

```json
{"type":"start","protocol":3,"mode":"ptt","voice":"cove","pace":0,"reply_length":0,"language":0,"duplex":0,"idle_seconds":180}
```

- `mode`：`ptt` 或 `continuous`；`duplex:1` 只在畅聊生效。
- `voice`：cove、juniper、maple、spruce、ember、vale、breeze、arbor、sol。
- `pace`：0 自然，1 慢，2 快；`reply_length`：0 简短，1 一句话，2 详细；`language`：0 普通话，1 英语，2 跟随用户。
- 音色是 Codex 原生参数，其他风格选项为模型指令。参数在会话启动时读取，下一次新对话应用改变。
- 偏好存储在 `prefs_v4`；从 v3/v2 精确读取旧四整数布局，保留音量、亮度和超时，新增模式默认 PTT。Wi-Fi NVS 不变。

当前订阅桥使用 Codex CLI 0.159.2。`thread/realtime/listVoices` 返回 v1/v2 两组；该版本 `core/src/realtime_conversation.rs` 的 `validate_realtime_voice` 将 **V3 映射到 V1**，不能把公开 API 的音色直接填入。来源：[对应版本实现](https://github.com/openai/codex/blob/rust-v0.159.2/codex-rs/core/src/realtime_conversation.rs)。

上行每帧 960 bytes（24 kHz、s16le、mono、20 ms）；协议 v2/v3 下行前加 `u32 little-endian epoch`，共 964 bytes。过期音频丢弃。旧协议 1 的 PTT 脚本继续接收未加代号的 960 字节音频和转写。

## 按住说话

1. 按住 OK 500 ms 后开始收音；画面显示 RECORDING。首次按住会先发 start，再发 ptt_start，接通期间也可以上传录音。
2. 服务器输入队列有界，最多 3008 条帧/控制消息；初始化期间保留上行。会话就绪后 PCM 进入 `PushToTalk` 内存缓存，不送入浏览器/模型。
3. 松开发 ptt_end；服务器按 20 ms 时钟把缓存送入 AudioWorklet。`submitted` 表示已开始发送，不表示模型处理完毕。设备停止上行，保留同一会话。
4. 模型回复经统一的播放代号、预填充、排空流程输出。结束后发 waiting，设备保持麦克风关闭，等待下一次按住。
5. 再次按住立即清空本地旧播放，服务清空尚未送入的上一段缓存并引导模型停止旧回答。结束和断线销毁内存缓存；没有音频落盘。

最大 3000 帧，即 60 秒，单份缓存约 2.88 MB，只在服务器占用。固件到 60 秒自动发送；对异常客户端超限录音整段丢弃，不发送不完整截断。因 Live 没有此通道可用的手动 commit/response-create 控制，采用释放后重放，延迟会随录音长度增加。

## 畅聊与播放排空

1. status:ready 后发 listen，打开输入。
2. 第一个模型非静音帧发 playback_begin，更新代号；半双工关闭输入，全双工保持输入。
3. 连续 650 ms 无非静音输出发 playback_end。设备队列 32 × 20 ms、预填充 12 帧，短回复等待上限 350 ms。
4. 队列及 I2S DMA 排空后再等保护时间，设备发 playback_drained。服务仅接受当前代号，半双工恢复输入。PTT 此时仍等待按键，不自动开麦。
5. 句中长停顿之后如有新输出，会更新代号并丢弃旧排空确认。

进入设置发送 pause，畅聊暂停输入，正在播放的回复可以播完。返回发送 resume，根据播放状态恢复。全双工为实验项：同时收音和播放；收到新用户 turn.started 时清旧输出并引导停止旧回复。没有增加 AEC；靠回声小的使用环境降低误触发概率，不能把此选项视为已验证的消回声算法。

## 打断与新对话

打断立即清空本地音频，并用 `thread/realtime/appendText`、role:developer 发送固定的应用控制指令。上游停止依赖模型遵循指令；本地静音、代号过滤是确定的。旧输出至少出现 200 ms 静音边界且有新的用户话音后才允许新输出，避免旧回答再次响起。

新对话先 stop，等待服务发 **stopped** 后再 start。stopped 在旧任务终止后发出，即使旧任务已经自然结束/不存在也会回复，避免用户在待机新建时无限等待。创建全新临时 Codex thread、浏览器和 WebRTC；不注入旧转写/history。配置切换不伪装成清空屏幕。

## 按键与菜单

- 聊天页：短按 OK 设置；长按 OK 录音/畅聊开始或打断；松开提交 PTT。
- UP/DOWN：聊天页调音量；菜单内移动或调整；长按没有额外快捷动作。
- 菜单：Conversation / Voice / Device / Network，每层 Back，音色长名单按选择分页显示。
- Wi-Fi 配网：扫码加入热点、扫码网页；短按 OK 翻页，长按 OK 重试旧网络。
- 电源为独立硬件键，公开 BSP 仅有 GPIO0 上的三功能键，没有软件电源短按事件。

## 检查边界

Rust 单元测试覆盖播放代号、打断顺序、PTT 缓存上限、松开前不可发送、参数校验。`scripts/test_session.py` 启动真实 Rust 服务与假 Codex/媒体子进程，验证启动期录音、松开后送入模型、音色/指令传递、新会话 ID、菜单收音暂停/恢复和空会话停止确认；只连接本机，不调用真实订阅。它们不能证明实机听感、识别率、模型是否严格遵循语速指令或全双工表现。
