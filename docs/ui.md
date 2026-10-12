# 设备 UI

设备界面为英文，240×320，黑底与奶油白文字，洋红、薄荷绿、淡紫、黄色与杏橙点缀。字体为 Barlow Condensed、Barlow Semi Condensed 与 IBM Plex Mono；自定义 mic-on 字标保留洋红色 i 点和连续的末尾 n。

| 页面 | 规则 |
| --- | --- |
| 首页 | VOICE / LIVE / SETTINGS；选中标题放大，字号和几何一次更新 |
| VOICE / LIVE | 对应标题、状态、音频驱动图案与聊天操作提示 |
| SETTINGS / DEVICE / Network | 静态列表，六行可见，上下选择，OK 确认，长按返回 |
| 数值调节 | 显示实际值及单位，音量 0 为静音 |
| 配网二维码 | 两步引导：加入热点，再打开配网页；保留二维码安静区 |
| FIRMWARE / WI-FI 结果 | 独立状态页面，不绘制聊天图形或录音提示 |

DEVICE 项为 Volume、Brightness、Screen off、Idle end、Firmware update、Back。配网统一在 Network → Connect Wi-Fi。菜单、首页与二维码页不被后台语音状态覆盖。

图案由 LVGL 原生绘制，音频动画只在聊天页运行。字体、字标保存在 Flash，LVGL 静态池为 24 KiB。普通固件编译使用已生成的资源；字体与字标生成脚本、源图和许可见 firmware/main/fonts、firmware/main/assets 与 assets/brand。

实际 LVGL 离屏预览在 scripts/ui_preview，使用固件相同源码与字体；可检查裁切、导航、动画和内存。离屏图像不代表物理屏幕或按键体验验收，操作方式见 [testing.md](testing.md)。
