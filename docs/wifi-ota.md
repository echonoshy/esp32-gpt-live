# 配网与按钮 OTA

## 扫码配网

1. 从 SETTINGS → Network → Connect Wi-Fi 进入配网。没有可用的已保存网络时也会开放 mic-on-XXXX 热点。
2. 手机系统相机扫描 JOIN HOTSPOT 二维码，确认加入设备热点；无互联网提示时保持连接。
3. 优先使用手机自动弹出的配网页；没有弹出时扫描设备自动显示的第二张二维码，打开 http://192.168.4.1/。短按 OK 可切换两张码。
4. 页面扫描附近 2.4 GHz Wi-Fi，按信号排序并去重，点选网络后输入密码。开放网络不要求密码，隐藏网络可手动输入；不支持的认证类型不可选。
5. 点击 CONNECT，保持页面打开。设备取得 IP 后才保存；密码、认证、找不到网络、DHCP 超时或保存失败均显示结果并允许重试。
6. 成功后点击 FINISH & RESTART，设备重启，手机切回日常 Wi-Fi。配置当前手机自己的热点时用第二部手机或电脑操作，确保目标热点在验证期间开启。

最多保存 8 个网络，新增保留其他网络，同名更新密码并优先尝试；存满不自动淘汰。配网页可查看并删除保存记录。主动配网暂停自动重连；扫描和试连共用无线所有权，避免后台切换频道干扰页面。配网页 POST 使用随机 nonce，密码不进入 URL 或日志。

源码 0.10.10 中，试连结果显示独立 WI-FI 页面，不显示 VOICE/LIVE 标题或录音提示。手机扫码兼容性、真实无线状态和错误重试仍待设备验收。

## 在设备上升级

联网后选择 SETTINGS → DEVICE → Firmware update → Check / install latest。确认会结束当前对话，设备自动：

1. 请求当前服务器的签名版本清单。
2. 比较数值版本，仅接受比当前版本新的固件。
3. 验证项目、尺寸、SHA-256 和 P-256 签名，下载到另一 OTA 槽。
4. 完整核对镜像哈希并完成 ESP-IDF 镜像验证，然后切换启动槽并重启。
5. 新固件在本地硬件、任务、网络/配网页面初始化后确认启动；确认前异常重启可回退。

已是最新版显示 UP TO DATE；检查或写入失败显示 UPDATE NOT INSTALLED，按 OK 返回 Firmware update 项重试。下载最多等待约三分钟，单次网络读取超时可能额外耗时。更新期间保持供电，FIRMWARE SAVED 表示待重启确认。

设备需访问 140.143.229.103，服务器必须已经发布使用同一私钥签名的新版本。**GitHub push 不会自动编译或发布设备固件。** 没有手机 App，也不需要选择文件。网页固件上传已删除，首次安装或恢复使用 USB。

## 发布新固件

先递增 PROJECT_VER，编译应用，使用现有签名私钥发布。已安装设备的公钥在 firmware/main/update_public_key.h，不能随意重新生成签名密钥。

当前工作区的原私钥保存在忽略目录 artifacts/auto-update-0.10.7/signing-private.pem；该路径中的版本只是首次生成时间，后续版本继续使用同一密钥。应单独安全备份，清理构建缓存时保留。

```sh
MIC_ON_SIGNING_KEY="$PWD/artifacts/auto-update-0.10.7/signing-private.pem"
.tools/python/bin/python scripts/publish_firmware.py \
  --image firmware/build/passport-gpt-live.bin \
  --key "$MIC_ON_SIGNING_KEY" \
  --out artifacts/release
```

输出为哈希命名的 .bin 和 latest.json。先上传镜像，再原子替换版本清单：

```sh
scp artifacts/release/*.bin lake@140.143.229.103:/home/lake/workspaces/mic-on/data/firmware/
scp artifacts/release/latest.json lake@140.143.229.103:/home/lake/workspaces/mic-on/data/firmware/latest.json.next
ssh lake@140.143.229.103 'mv /home/lake/workspaces/mic-on/data/firmware/latest.json.next /home/lake/workspaces/mic-on/data/firmware/latest.json'
```

发布目录可保留旧哈希镜像，避免清单检查与下载期间换版本造成错配。下载接口 GET /api/firmware/latest 和 GET /api/firmware/{sha256} 使用设备令牌鉴权；私钥不上传服务器，镜像不提交公开仓库。

## 确认范围

/api/info 返回设备版本、device_id、boot_id、完整 ELF image_id 与 ota_state。image_id 与 .bin 文件 SHA-256 是不同标识。升级重启后可据此核对运行镜像；valid 仅确认本地启动，不证明网络或语音功能正常。

按钮 OTA 的完整下载、写入、重启和回退尚未做实机验收；已有 USB 安装确认不能替代该验收。当前状态见 [status.md](status.md)，离线回归见 [testing.md](testing.md)。
