#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .tools/fonts .tools/font-converter
font_path=.tools/fonts/NotoSansCJKsc-Regular.otf
if [[ ! -f "$font_path" ]]; then
  curl --fail --location --retry 3 'https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf' -o "$font_path"
fi
python3 - "$font_path" <<'PY'
import hashlib, pathlib, sys
expected = '2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b'
if hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest() != expected:
    raise SystemExit('Font source changed; review its source/license before regenerating.')
PY
npm install --prefix .tools/font-converter --no-audit --no-fund lv_font_conv@1.5.3
node .tools/font-converter/node_modules/lv_font_conv/lv_font_conv.js \
  --size 16 --bpp 2 --format lvgl --font "$font_path" \
  --range '0x20-0x7e,0x2000-0x206f,0x2190-0x2193,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef' \
  --no-kerning --lv-font-name passport_cjk_16 -o firmware/main/fonts/passport_cjk_16.c

python3 - <<'PYFONT'
from pathlib import Path
p = Path('firmware/main/fonts/passport_cjk_16.c')
p.write_text(p.read_text().rstrip() + '\n')
PYFONT
