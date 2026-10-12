# 部署与固件维护

## 当前服务器

以下是 2026-10-12 的部署配置；本轮固件发布只更新签名镜像与版本清单，不操作设备或重启服务。

| 项目 | 配置 |
| --- | --- |
| 服务器 | 140.143.229.103 |
| 工作目录 | /home/lake/workspaces/mic-on |
| 用户服务 | mic-on-voice.service |
| 服务程序 / 版本 | target/release/passport-voice / 0.9.4 |
| 设备连接 | ws://140.143.229.103:8787/device |
| 健康信息 | http://140.143.229.103:8787/health |
| Codex CLI | 0.162.1；/home/lake/.nvm/versions/node/v24.15.0/bin/codex |
| Codex 运行授权目录 | /home/lake/.nvm/versions/node/v24.15.0 |
| Chromium 启动脚本 | /home/lake/workspaces/mic-on/chrome-launch |
| 上下文 / 时区 | /home/lake/workspaces/mic-on/context / Asia/Shanghai |
| 签名固件目录 | /home/lake/workspaces/mic-on/data/firmware；已发布 0.10.10 |

环境示例为 .env.example。DEVICE_TOKEN 必须与设备私有配置一致；令牌、登录凭据与上下文数据不提交 Git。FIRMWARE_DIR 默认是工作目录下的 data/firmware。

Chromium 当前脚本调用 `/home/lake/.cache/ms-playwright/chromium-1234/chrome-linux64/chrome`，添加 `--disable-extensions --disable-background-networking --no-first-run --password-store=basic` 并把错误写入 chrome.log。迁移时使用新机器的实际路径，不假定此绝对路径存在。

## 服务构建与安装

```sh
cargo build --release --locked
mkdir -p ~/.config/systemd/user
cp scripts/mic-on-voice.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now mic-on-voice.service
loginctl enable-linger "$USER"
```

模板使用当前部署目录和 .env。服务启动需要已登录的 Codex CLI、可执行 Chromium、date 和 python3。升级前备份现有二进制与运行配置；服务重启会中断现有会话。

只读运维：

```sh
curl -fsS http://140.143.229.103:8787/health
ssh lake@140.143.229.103 'systemctl --user status mic-on-voice.service --no-pager'
ssh lake@140.143.229.103 'journalctl --user -u mic-on-voice.service -n 50 --no-pager'
```

health 中 version 是服务版本，protocol 是音频协议，device_connected 仅表示设备桥连接，不代表语音体验已验收。

## 固件构建

ESP-IDF 固定为 5.5.3，组件版本由 firmware/dependencies.lock 锁定。固件源码版本取 firmware/CMakeLists.txt 的 PROJECT_VER。

```sh
cp firmware/main/device_config.example.h firmware/main/device_config.h
# 配置 VOICE_URL 和 DEVICE_TOKEN
. /path/to/esp-idf/export.sh
idf.py -C firmware build
```

当前工作区已有工具链时使用 .tools/esp-idf/export.sh，并设置 IDF_TOOLS_PATH 为本项目的 .tools/espressif。生成字体已入库，普通编译不需要字体工具。

应用为 firmware/build/passport-gpt-live.bin，包含私有设备配置，不能上传公开 GitHub Release。面向设备发布使用 [签名固件发布流程](wifi-ota.md)。

## USB 首次安装与恢复

目标为 ESP32-C3、8 MB Flash。维护前读回实际分区表和 OTA 元数据，并保存完整 Flash 备份；不能凭历史记录猜测当前启动槽。

| 分区 | 地址 | 大小 |
| --- | --- | --- |
| NVS | 0x9000 | 0x6000 |
| PHY | 0xf000 | 0x1000 |
| ota_0 | 0x10000 | 0x380000 |
| ota_1 | 0x390000 | 0x380000 |
| otadata | 0x710000 | 0x2000 |

已有双槽设备仅写已确认的应用槽，可保留 bootloader、分区表、Wi-Fi NVS 与 OTA 元数据。首次建立或修复分区布局需要完整安装，不能把全量 idf.py flash 当作普通更新。

刷写后核对应用读回、设备 /api/info 的 version 与 ota_state。启动 valid 表示本地初始化已确认，不验证语音、互联网或主观听感。最近安装记录与当前源码的区别见 [验证状态](status.md)。
