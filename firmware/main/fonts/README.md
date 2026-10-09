# Passport 中文点阵字库

`passport_cjk_16.c` 是 Noto Sans CJK SC Regular **2.004** 的 16 px、2 bpp 子集，保留 ASCII、基础汉字、中文标点与方向箭头。字体数据常驻 Flash。

源字体版权：© 2014–2021 Adobe (http://www.adobe.com/)。许可为 [SIL Open Font License 1.1](OFL.txt)，不适用项目 BSP 的 MIT 许可。

- 来源：https://github.com/notofonts/noto-cjk/tree/main/Sans
- 源文件：`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`
- SHA-256：`2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`
- 转换器：`lv_font_conv@1.5.3`
- 仓库根目录执行 `scripts/generate_font.sh` 可以复现；通常无需重新生成。

基础字库不包含 emoji、生僻扩展汉字和其他语言的完整字符集。显示字幕最多 180 个 Unicode 字符，避免挤占 ESP32-C3 的有限 RAM。
