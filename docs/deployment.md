# 当前服务器与版本信息

2026-10-12 核对，以 `140.143.229.103` 的现有服务为准。旧服务器不参与本次同步，也没有停止或修改。

## 连接与部署

| 项目 | 当前配置 |
| --- | --- |
| 设备 WebSocket | `ws://140.143.229.103:8787/device` |
| 健康检查 | `http://140.143.229.103:8787/health` |
| 服务器目录 | `/home/lake/workspaces/mic-on` |
| systemd 用户服务 | `mic-on-voice.service` |
| 运行程序 | `target/release/passport-voice` |
| 监听 | `0.0.0.0:8787` |
| Codex CLI | `0.162.1`，`/home/lake/.nvm/versions/node/v24.15.0/bin/codex` |
| Codex 运行目录授权 | `/home/lake/.nvm/versions/node/v24.15.0` |
| 上下文目录 | `/home/lake/workspaces/mic-on/context` |
| 时区 | `Asia/Shanghai` |

设备主动连接服务器。Rust 服务启动服务器本机的 Codex app-server 和 Chromium；Chromium 承担 WebRTC 音频桥接，Mac 不承担在线语音中转。

私有的 `firmware/main/device_config.h` 与已跟踪的 `device_config.example.h` 均指向当前地址。实际设备令牌必须与运行服务的 `DEVICE_TOKEN` 一致，不写入文档或版本控制。本次比对确认原本地令牌与新服务器运行进程中的令牌已经一致。`scripts/probe_voice.py` 的默认地址也已更新；该脚本会创建语音会话，不能当作无副作用的健康检查。

当前服务器通过 `CHROMIUM_BIN=/home/lake/workspaces/mic-on/chrome-launch` 启动浏览器，脚本为：

```sh
#!/bin/sh
exec /home/lake/.cache/ms-playwright/chromium-1234/chrome-linux64/chrome --disable-extensions --disable-background-networking --no-first-run --password-store=basic "$@" 2>>/home/lake/workspaces/mic-on/chrome.log
```

在其他机器部署时，按实际安装位置创建可执行启动脚本并调整 `.env`；不要直接假设这些绝对路径存在。`.env.example` 和 `scripts/mic-on-voice.service` 已对齐当前机器的配置。服务程序名仍为 `passport-voice`。

只读检查命令：

```sh
curl http://140.143.229.103:8787/health
ssh lake@140.143.229.103 'systemctl --user status mic-on-voice.service --no-pager'
ssh lake@140.143.229.103 'journalctl --user -u mic-on-voice.service -n 50 --no-pager'
```

## 本地与线上源码差异

同步前逐文件比较 `server/`、`Cargo.toml` 和 `Cargo.lock`，仅两处源码不同，已将线上实现同步回本地：

- `server/src/main.rs`：原流程依次等待浏览器 SDP、打开 Codex、创建对话；线上流程使用 `tokio::try_join!` 并行准备媒体 SDP 与 Codex 登录/对话，两者完成后启动实时音频。增加 media offer、account、thread、realtime accepted 四个启动耗时日志。
- `server/src/codex.rs`：从 `start()` 中提取 `prepare()`，提前创建临时对话；`start()` 要求对话已准备，再使用 SDP 打开实时会话。`prepare()` 本身不启动实时音频。

其他服务端文件一致，包括 `context.rs` 的轻量记忆规则、协议处理、音色确认和浏览器媒体代码。依赖锁文件未变化。远端新增的 Chromium 启动脚本属于部署配置。

并行启动消除了两段准备流程的串行等待，但不代表首次连接已稳定在某个耗时内。10 月 11 日线上样本显示媒体 SDP 约 636–657 ms 就绪，账号准备约 4–13 s，完整媒体就绪约 11–17 s；这些是不同会话样本，不是严格的改动前后性能对照。

远端只有服务端部署文件，没有固件源码和完整 Git 仓库。因此不能从远端文件推断 ESP32 实际刷入了哪一版固件。同步前的源码快照和差异保存在本机忽略目录 `artifacts/server-140-analysis/`，不含设备令牌。

## 三种版本各自含义

| 版本 | 来源 | 如何确认 |
| --- | --- | --- |
| 固件源码 `0.10.9` | `firmware/CMakeLists.txt` 的 `PROJECT_VER` | 扫码配网、扫描点选、联网结果、OTA 启动确认与设备按钮自动更新；已首次 USB 安装，功能未实机验收 |
| 设备实际固件 `0.10.9` | 2026-10-12 USB 刷写并读回确认 | 当前设备 `http://192.168.7.66/api/info` → `version`；手机设置页也显示 `v版本号`，局域网地址可能变化 |
| 服务端 `0.9.4` | `server/Cargo.toml` | 当前服务器 `/health` → `version`，新增私有签名固件下载接口 |
| 音频协议 `8` | 服务端/固件协议约定 | 服务器 `/health` → `protocol`；不是软件发布版本 |

此前 `0.10.4` 只修改固件目标服务器配置和版本标识，没有修改采音、播放或按键逻辑。源码版本更新不会自动升级设备；应区分源码、构建镜像、烧录完成和设备读回四种状态。0.9.3 在并行准备基础上增加待机准备；需结合源码和启动阶段日志核对是否复用了准备结果。

## 本次验证边界

已通过 SSH 读取新服务器源码、服务配置、版本和日志，并从 Mac 访问健康接口；检查时健康接口报告设备已连接。历史日志存在真实 VOICE 模型回复与模式切换，也存在设备连接重置和心跳超时，不能把单次健康检查解释为长期语音稳定性验收。

最初同步 0.10.4 时未重启服务，随后安装 ESP-IDF、编译刷写并完成静音 probe。之后为修复自然插话和启动等待，已部署服务 0.9.3，并刷入 0.10.5，Flash 读回一致。实机语音测试按用户要求中止，设备返回首页并关闭收音；自然插话尚未完成实机验收。后续仅做代码检查和离线回归，记录见 `docs/validation.md`。

## 0.9.3 补丁部署记录（2026-10-12）

“预备进程退出时自动重建”已合入本次部署。源代码和锁文件先上传至 `/home/lake/workspaces/mic-on-stage-remaining`，运行 `cargo build --locked --release`，再替换正式目录的源码与程序并重启 `mic-on-voice.service`。环境变量、登录信息与 context 文件保持原配置。

本次部署前的服务源码及程序备份在 `/home/lake/workspaces/mic-on-backup-remaining-FaHWua`；新程序 SHA-256 为 `b8675b8db9864415a2a70e40eb2a92822cc29da9c33f625bf4e2eb0e11f1f0ea`。服务显示 active，健康接口版本仍为 0.9.3、协议 8；实际语音验收继续暂停。源码提交与固件刷写是独立动作，本次没有再次刷写设备。

## 本机编译环境

ESP-IDF 固定为 `v5.5.3`，安装在项目的 `.tools/esp-idf`，芯片工具链和 IDF Python 环境放在 `.tools/espressif`。这些目录均被 Git 忽略。macOS 依赖为 CMake、Ninja 和 dfu-util；Python 使用 uv 管理的 3.13。官方安装步骤见 [ESP-IDF 5.5.3 macOS 安装说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/get-started/linux-macos-setup.html)。

首次安装（从项目根目录运行）：

```sh
brew install cmake ninja dfu-util
uv python install 3.13
git clone --branch v5.5.3 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .tools/esp-idf
export IDF_TOOLS_PATH="$PWD/.tools/espressif"
export PATH="$(dirname "$(uv python find 3.13)"):$PATH"
.tools/esp-idf/install.sh esp32c3
```

从项目根目录重新构建：

```sh
export IDF_TOOLS_PATH="$PWD/.tools/espressif"
export PATH="$(dirname "$(uv python find 3.13)"):$PATH"
. .tools/esp-idf/export.sh
idf.py -C firmware build
```

应用镜像是 `firmware/build/passport-gpt-live.bin`，包含本机私有设备令牌，不应提交到公开仓库。完整 Flash 备份还包含 Wi-Fi 等 NVS 设置，同样仅保存在被忽略的 `artifacts/`。

已有设备更新前先读回分区表和 OTA 元数据，确认当前启动槽并备份。本项目设备的 `ota_0` 位于 `0x10000`，`ota_1` 位于 `0x390000`，每槽 `0x380000` 字节；不能仅按历史记录猜测当前槽。仅写已确认的应用槽即可保留现有分区表、bootloader 和 NVS。

## 0.10.6 本地固件改动（历史记录）

本轮配网与 OTA 修改均在固件中，不需要修改 140.143.229.103 的语音服务。按用户要求仅更新源码、文档并执行离线检查，没有部署、刷写、上传 OTA 或实际联网测试。该轮结束时设备仍为 0.10.5；随后已安装 0.10.9，当前版本见上表。新流程与首次 OTA 升级的页面兼容说明见 [wifi-ota.md](wifi-ota.md)。

## 0.9.4 自动更新服务

服务源码已在远端锁定依赖构建并部署，固件目录为 /home/lake/workspaces/mic-on/data/firmware，发布的 0.10.9 使用设备令牌鉴权下载。发布接口不自动推送或升级设备，实际设备先通过 USB 完成首次安装 0.10.7，随后安装 0.10.8 修正更新页面、0.10.9 整理菜单；未做自动更新功能测试。部署前备份目录 /home/lake/workspaces/mic-on-backup-auto-update-m912IC。签名私钥仅留在本地忽略目录，服务器只保存签名信息和镜像。按钮更新的使用和发布方式见 [wifi-ota.md](wifi-ota.md)。
