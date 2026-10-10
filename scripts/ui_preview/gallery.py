#!/usr/bin/env python3
"""Package real LVGL captures and optionally serve an offline UI review (requires Pillow)."""
import argparse
import html
import json
import re
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--serve', type=int, metavar='PORT')
args = parser.parse_args()
out = args.output.resolve()
shots = out / 'preview'
for file in shots.glob('*.ppm'):
    Image.open(file).save(file.with_suffix('.png'))
frames = [Image.open(shots / 'home-0.png')]
durations = [800]
for target in (1, 2):
    for file in sorted(shots.glob(f'switch-{target}-*.png')):
        frames.append(Image.open(file))
        durations.append(20)
    durations[-1] = 800
frames[0].save(out / 'mode-switch.gif', save_all=True, append_images=frames[1:],
               duration=durations, loop=0)
names = ['home-0', 'home-1', 'home-2', 'voice-ready', 'free-dialogue', 'settings',
         'device', 'voices', 'reset-context', 'qr-settings', 'low-battery', 'volume-popup']
board = Image.new('RGB', (1040, 1050), '#303030')
draw = ImageDraw.Draw(board)
for i, name in enumerate(names):
    x, y = i % 4 * 260 + 10, i // 4 * 350 + 20
    board.paste(Image.open(shots / f'{name}.png'), (x, y))
    draw.text((x, y - 15), name, fill='white')
board.save(out / 'overview.png')
# The embedded captive portal is extracted verbatim for browser review.
wifi = (ROOT / 'firmware/main/wifi.c').read_text()
wifi = wifi.split('static const char page[] =', 1)[1].split(';\n', 1)[0]
(out / 'wifi.html').write_text(''.join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', wifi)))
(out / 'settings.html').write_text((ROOT / 'firmware/main/settings.html').read_text())
labels = {'home-0': '首页 · VOICE', 'home-1': '首页 · LIVE', 'home-2': '首页 · SETTINGS',
          'voice-ready': '等待录音', 'voice-recording': '录音中', 'voice-speaking': '回复播放',
          'free-dialogue': 'LIVE 收听', 'settings': '设置', 'voices': '音色',
          'voices-last': '音色 · 末页', 'device': '设备', 'volume': '音量',
          'brightness': '亮度', 'screen-timeout': '息屏时间', 'idle-timeout': '空闲超时',
          'network': '网络', 'connection-info': '连接信息', 'phone-confirm': '打开手机设置确认',
          'network-confirm': '配网确认', 'reset-context': '重置确认',
          'qr-hotspot': '热点二维码', 'qr-setup': '配网二维码', 'qr-settings': '设置二维码',
          'low-battery': '低电量 / 离线', 'volume-popup': '音量浮层'}
state_labels = ['启动', '连接中', '思考中', '重连', '语音不可用', '会话结束', '录音超限',
                '升级中', '升级失败', '升级完成', '设置错误', '已暂停', '应用音色']
labels.update({f'state-{i:02d}': title for i, title in enumerate(state_labels)})
cards = ''.join(f'<figure><img src="preview/{name}.png" width="240" height="320" alt="{html.escape(label)}"><figcaption>{html.escape(label)}</figcaption></figure>' for name, label in labels.items())
(out / 'index.html').write_text('''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>mic-on · 漫画界面预览</title><style>
*{box-sizing:border-box}body{background:#141414;color:#f3eedb;font:16px system-ui,sans-serif;margin:0;padding:32px}h1{font-size:32px;margin:0 0 12px}p{color:#c8c3b3;line-height:1.7;max-width:800px}a{color:#ffe900}nav{display:flex;flex-wrap:wrap;gap:24px;margin:24px 0}h2{margin:40px 0 20px;font-size:22px}main{display:grid;grid-template-columns:repeat(auto-fit,240px);gap:24px}figure{margin:0}figure img{display:block;image-rendering:pixelated}figcaption{margin:8px 0 16px;color:#c8c3b3;font-size:14px}.phones{display:flex;gap:24px;flex-wrap:wrap}iframe{border:1px solid #8c52c5;width:390px;height:750px;background:#141414;max-width:100%}@media(max-width:500px){body{padding:20px}}
</style><h1>mic-on / COMIC UI</h1><p>实际固件 UI 的 240 × 320 离屏渲染。下方动画来自同一份 LVGL 代码的逐帧输出。手机页面使用离线示例数据，保存和上传不会修改真实设备。</p><nav><a href="#screens">设备页面</a><a href="#motion">切换动画</a><a href="#phone">手机页面</a><a href="overview.png">页面总览</a></nav><h2 id="motion">模式切换 · 160 ms</h2><img src="mode-switch.gif" width="240" height="320" alt="VOICE 到 LIVE 再到 SETTINGS 的实际动画帧"><h2 id="screens">设备页面</h2><main>''' + cards + '''</main><h2 id="phone">手机设置与配网</h2><p>这是本地交互预览，网络与保存结果均为示例；真实设备页面保留原有 API。</p><div class="phones"><iframe src="settings.html" title="手机设置"></iframe><iframe src="wifi.html" title="手机配网"></iframe></div></html>''')
if args.serve:
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(out), **kw)
        def do_GET(self):
            if self.path.startswith('/api/info'):
                payload = dict(nonce='offline-preview', state='SETTINGS', battery=86, version='preview',
                               volume=70, brightness=65, screen_seconds=60, idle_seconds=180,
                               mode=0, voice=0, duplex=1)
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers()
                self.wfile.write(json.dumps(payload).encode()); return
            super().do_GET()
        def do_POST(self):
            self.rfile.read(int(self.headers.get('Content-Length', 0)))
            self.send_response(200); self.send_header('Content-Type', 'text/plain; charset=utf-8'); self.end_headers()
            self.wfile.write(b'Preview only. No device settings were changed.')
    print(f'Offline UI review: http://127.0.0.1:{args.serve}', flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.serve), Handler).serve_forever()
