# 静态界面字形

设备只加载编译后的 2 bpp 字形子集。完整字体只用于构建，不打包进设备 RAM，也不在设备解析字体。

- `ui_font_16.c`：Lora，顶部产品名称。
- `ui_font_18.c`：Noto Sans CJK SC 中文与 Poppins ASCII，菜单和操作提示。
- `ui_font_22.c`：Noto Serif CJK SC 中文与 Lora ASCII，大标题和状态。

字体均遵循 SIL Open Font License 1.1，各字体版权与许可在同目录 OFL 文件中。中文字体是适合本设备的视觉替代，并非 Anthropic 专有字体。

将以下字体保存到 `.tools/fonts/`，安装 `lv_font_conv` 后运行 `scripts/build_ui_fonts.py`；也可使用脚本参数指定路径。

- [NotoSansCJKsc-Regular.otf](https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf)
  SHA-256: `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`
- [NotoSerifCJKsc-Regular.otf](https://raw.githubusercontent.com/notofonts/noto-cjk/main/Serif/OTF/SimplifiedChinese/NotoSerifCJKsc-Regular.otf)
  SHA-256: `2a2eae2628df83556c54018c41e20fa532c1b862c5256ae8b3f23feb918d12ca`
- [Poppins-Regular.ttf](https://raw.githubusercontent.com/google/fonts/main/ofl/poppins/Poppins-Regular.ttf)
  SHA-256: `7e65201e9b79159e2300267cc885e16c8dcef2424cdfa09a29bfb0980a94a7ba`
- [Lora-Regular.ttf](https://raw.githubusercontent.com/google/fonts/main/ofl/lora/Lora%5Bwght%5D.ttf)
  SHA-256: `822a6621ccbe8d97d20ac88c1c41f5615c9c2c202eaa75f272cd452aac6475a7`
