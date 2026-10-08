# 硬件与驱动来源

目标产品：FoloToy AI Passport。2026-10-08 经 esptool 实测：ESP32-C3 QFN32 v1.1、8 MB 内置 XMC Flash、40 MHz 晶振、USB Serial/JTAG。

板卡资源：ST7789P3 240×320 显示屏，ES8311 音频 codec，CW2017 电量计，GPIO0 ADC 三键分压。详细引脚以 BSP 的 `bsp_pins.h` 为准，应用层不重复配置。

`firmware/components/bsp` 提取自用户本机 `ripple-live/firmware/ai-passport` 的现有适配（父仓库 HEAD `b209e43bb051622566dfa4acca584c3caa06ae12`，工作区文件快照；不声称所有文件均来自该提交）。上游项目为 <https://github.com/FoloToy/ai-passport>，许可证保存在 `firmware/LICENSE`。`wifi.c` 的配网逻辑也由该本机适配演化，旧项目原文件未修改。

沿用现有 40 MHz 显示时钟和 20 行单 DMA 缓冲。官方最新分支的显示时钟可能不同；本次不因上游默认变化而调整已用驱动。LVGL 内部池明确为 24576 字节，修正新版本弃用 `LV_MEM_SIZE_KILOBYTES` 后意外采用 65536 字节默认值的问题。

音频通过 BSP `bsp_audio_*` 访问，保持 ES8311 `no_dac_ref=true`、采样率改变时 close/open、驱动管理时钟寄存器。麦克风和扬声器任务独立运行，不在按键或 WebSocket 回调里执行阻塞 I2S。

服务端和固件是新项目；没有改写旧 `ripple-live` 项目或迁移它的应用资源。
