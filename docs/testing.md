# 验证与测试

默认回归在本机运行，不连接 ESP32、140.143.229.103 或真实订阅语音。测试文件仍放 scripts/，与构建/诊断脚本保持现有路径。不要因创建时间较早就删除仍覆盖现役实现的测试。

## 服务与媒体

```sh
cargo test --locked
cargo clippy --all-targets --locked -- -D warnings
cargo build --locked
.tools/python/bin/python scripts/test_session.py
node scripts/test_worklet.cjs
node scripts/test_voice_setup.cjs
```

Rust 单元测试覆盖音频编码、播放代号、回复缓冲、PTT、准备会话、上下文文件和固件接口。test_session 使用本地真实 Rust 服务与假的 Codex/媒体进程，保留现役协议 8、旧客户端兼容、模式切换、暂停、音色、停止、插话与准备失败回归。需安装 Python websockets；不调用真实订阅。

## 固件主机回归

| 测试 | 覆盖 |
| --- | --- |
| test_adpcm.py | C ADPCM 与独立参考向量 |
| test_ws_send.py | 部分写入、发送背压、超时与恢复 |
| test_capture_transport.py | 采集发送隔离、尾帧、取消与拥塞 |
| test_voice_navigation.py | 导航、换音色、暂停/重建状态 |
| test_wifi_connection.py | 保存网络轮换、DHCP 超时、无线所有权与页面语法 |
| test_wifi_setup.py | 取得 IP 后保存、错误与凭据清理、删除网络重启失败时释放锁 |
| test_setup_page.cjs | 扫描、点选、隐藏/开放网络与连接结果 |
| test_auto_update.py | 签名发布、篡改拒绝、私有下载、版本与摘要解析 |
| test_controls.c / test_menu.c | 三键动作、返回、设置与更新确认 |
| test_audio_transport.c / test_playback_gain.c | 缓冲策略与播放增益 |
| test_wifi_profiles.c | 多网络 NVS、迁移、失败与保存顺序 |

Python 主机替身通过系统 C 编译器运行实际函数，不接硬件。固件构建按 [deployment.md](deployment.md)，主机 C 编译例子：

```sh
cc -std=c11 -Wall -Wextra -Werror -Iscripts/fixtures/wifi -Ifirmware/main scripts/test_menu.c firmware/main/menu.c -o /tmp/mic-on-menu
/tmp/mic-on-menu
cc -std=c11 -Wall -Wextra -Werror -pthread -Iscripts/fixtures/wifi -Ifirmware/main scripts/test_wifi_profiles.c firmware/main/wifi_profiles.c -o /tmp/mic-on-wifi-profiles
/tmp/mic-on-wifi-profiles
```

## UI 离屏渲染

scripts/ui_preview 编译实际 ui.c、menu.c、字体与匹配 LVGL，写出 PPM；覆盖标签边界、导航、音频动画与切页内存。运行方法见 [预览工具](../scripts/ui_preview/README.md)。这不是设备照片或实机交互验收。

## 设备诊断（需另行安排）

读取 /api/info 或服务器 /health 不创建语音会话。scripts/serial_monitor.py、scripts/configure_wifi.py 与 scripts/netcheck.py 会接触设备；netcheck 虽然不上传麦克风、不调用模型，仍会发送测试数据。

netcheck 要求显式 --url，不能用默认历史地址。USB 的 connect / live / record / send 会实际发起语音，可能消耗额度；stop / home 改变设备状态。这些诊断不放入默认离线回归，本轮不执行。

完整 OTA 与语音体验仍需设备验收，当前边界见 [status.md](status.md)。
