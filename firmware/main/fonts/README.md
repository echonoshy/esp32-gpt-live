# 英文字体与字标

0.9.0 起设备界面统一英文，使用 4 bpp / 16 级灰度字形。字形保存在 Flash，不在运行时加载完整字体；LVGL 静态池保持 24 KiB。

| 文件 | 字体 | 用途 |
| --- | --- | --- |
| `ui_font_14.c` | Barlow Semi Condensed Medium 14 px | 紧凑按键提示、电量与音量 |
| `ui_font_16.c` | IBM Plex Mono Regular 16 px | 小节标签 |
| `ui_font_18.c` | Barlow Semi Condensed Medium 18 px | 设置列表和正文 |
| `ui_font_22.c` | Barlow Condensed Black Italic 22 px | 状态、设置标题、调节数值 |
| `ui_font_comic_44.c` | Barlow Condensed Black Italic 44 px | 首页未选中项、Voice / Live 标题 |
| `ui_font_comic_52.c` | Barlow Condensed Black Italic 52 px | 首页选中的 Settings |
| `ui_font_comic_80.c` | Barlow Condensed Black Italic 80 px | 首页选中的 Voice / Live |

设备字标使用 `../assets/ui_wordmark.c`，来源与生成方式见该目录 README。普通 UI 字体只含可打印 ASCII；44/52/80 px 标题字体只包含对应标题所需字符。手机设置页内联 Orbitron WOFF2 字标，无外部字体请求。移除了旧版专用于中文首页名称的额外字形文件。

四款字体的 SIL OFL 1.1 许可分别保存在同目录 `OFL-ibmplexmono.txt`、`OFL-barlowsemicondensed.txt`、`OFL-barlowcondensed.txt`、`OFL-orbitron.txt`；其他许可供历史版本保留。

将以下原始字体放到 `.tools/fonts/`，使用 `fonttools[woff]` 4.66.1 与 `lv_font_conv` 1.5.3 生成。Orbitron 的变量字重固定为 900。生成的 C 和 HTML 已入库，正常固件编译不需要字体工具。

```sh
uv pip install --python .tools/python/bin/python 'fonttools[woff]==4.66.1'
.tools/python/bin/python scripts/build_ui_fonts.py
```

- [IBMPlexMono-Regular.ttf](https://raw.githubusercontent.com/google/fonts/main/ofl/ibmplexmono/IBMPlexMono-Regular.ttf)
  SHA-256: `6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46`
- [BarlowSemiCondensed-Medium.ttf](https://raw.githubusercontent.com/google/fonts/main/ofl/barlowsemicondensed/BarlowSemiCondensed-Medium.ttf)
  SHA-256: `4998b693a190b76f2b11315844345a8546c1e2e6a320905507a9eed04edf8b48`
- [BarlowCondensed-BlackItalic.ttf](https://raw.githubusercontent.com/google/fonts/main/ofl/barlowcondensed/BarlowCondensed-BlackItalic.ttf)
  SHA-256: `75338266e60b15dfc5ac0bae8b78fdc5535ea50703ed73c14dc598498ffe8173`
- [Orbitron-Variable.ttf](https://raw.githubusercontent.com/google/fonts/main/ofl/orbitron/Orbitron%5Bwght%5D.ttf)
  SHA-256: `f42db2dd16e642258e35782916eceb1dcdbea06fb958d77ad71dc5963587e8fd`
