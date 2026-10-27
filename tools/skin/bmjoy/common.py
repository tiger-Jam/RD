# BMjoy の標準スキンを作る道具の共通部分。
# 絵は HTML で描いて Chrome で透明な PNG にし、切り出す位置を覚えておいて LR2 形式の CSV に書く。
import os
import shutil
import subprocess
import tempfile

CHROME = os.environ.get('CHROME', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome')

# 文字の書体（Google Fonts の、自由に配れるもの）
FONTS = ('https://fonts.googleapis.com/css2?family=Barlow+Condensed:ital,wght@0,600;0,700;1,800'
         '&family=Noto+Sans:wght@400;700&display=block')
DISPLAY = "'Barlow Condensed'"
PLAIN = "'Noto Sans'"  # 飾らない文字（ラベルと数字）


def label(s, size, color='#fff', weight=400, align='flex-start'):
    """飾らない文字（枠いっぱいに置く。align は flex-start / center / flex-end）"""
    return (f'<div style="width:100%;height:100%;display:flex;align-items:center;justify-content:{align};'
            f'font:{weight} {size}px {PLAIN};color:{color};white-space:nowrap;line-height:1">{s}</div>')


def render_html(html, w, h, png):
    """HTML を w×h の透明な PNG にする"""
    with tempfile.TemporaryDirectory() as d:
        page = os.path.join(d, 'page.html')
        with open(page, 'w', encoding='utf-8') as f:
            f.write(html)
        out = os.path.join(d, 'out.png')
        subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                        '--default-background-color=00000000', '--virtual-time-budget=8000',
                        f'--window-size={w},{h}', f'--screenshot={out}', 'file://' + page],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120, check=True)
        os.makedirs(os.path.dirname(png), exist_ok=True)
        shutil.copy(out, png)


def page(body, w, h, css=''):
    return f'''<!doctype html><html><head><meta charset="utf-8"><link href="{FONTS}" rel="stylesheet">
<style>html,body{{margin:0;padding:0;background:transparent;width:{w}px;height:{h}px;overflow:hidden}}
.s{{position:absolute;overflow:hidden}}{css}</style></head><body>{body}</body></html>'''


class Sheet:
    """部品を 1 枚の画像に並べる。add は切り出す位置 (x, y, w, h) を返す"""

    def __init__(self, name, width=2048, css=''):
        self.name, self.width, self.css = name, width, css
        self.items = []  # (x, y, w, h, html)
        self.x = self.y = self.row_h = 0

    def _place(self, w, h):
        if self.x + w > self.width:
            self.x, self.y, self.row_h = 0, self.y + self.row_h + 2, 0
        pos = (self.x, self.y)
        self.x += w + 2
        self.row_h = max(self.row_h, h)
        return pos

    def add(self, w, h, html):
        x, y = self._place(w, h)
        self.items.append((x, y, w, h, html))
        return (x, y, w, h)

    def strip(self, fw, fh, cols, rows, frame_html):
        """アニメーションのコマ（左から右、上から下）。frame_html(i) が i 番目のコマ"""
        x, y = self._place(fw * cols, fh * rows)
        for i in range(cols * rows):
            self.items.append((x + (i % cols) * fw, y + (i // cols) * fh, fw, fh, frame_html(i)))
        return (x, y, fw * cols, fh * rows)

    def render(self, png):
        h = self.y + self.row_h
        body = ''.join(f'<div class="s" style="left:{x}px;top:{y}px;width:{w}px;height:{hh}px">{html}</div>'
                       for x, y, w, hh, html in self.items)
        render_html(page(body, self.width, h, self.css), self.width, h, png)


class Csv:
    """LR2 形式のスキンの行を書く"""

    def __init__(self):
        self.lines = []

    def raw(self, s):
        self.lines.append(s)

    def src(self, cmd, n, gr, rect, div=(1, 1), cycle=0, timer=0, ops=()):
        x, y, w, h = rect
        vals = [n, gr, x, y, w, h, div[0], div[1], cycle, timer, *ops]
        self.lines.append(f'#SRC_{cmd},' + ','.join(str(v) for v in vals))

    def dst(self, cmd, n, keys, loop=None, timer=0, ops=(0, 0, 0)):
        """keys は dict の並び：time x y w h と、任意で a r g b blend acc angle center filter。
        loop を省くと、最後のキーの形で止める（LR2 は loop の時刻へ戻って繰り返すので、0 だと頭から繰り返す）"""
        if loop is None:
            loop = keys[-1].get('time', 0)
        for i, k in enumerate(keys):
            vals = [n, k.get('time', 0), round(k['x']), round(k['y']), round(k['w']), round(k['h']), k.get('acc', 0),
                    k.get('a', 255), k.get('r', 255), k.get('g', 255), k.get('b', 255), k.get('blend', 1),
                    k.get('filter', 0), k.get('angle', 0), k.get('center', 0)]
            if i == 0:
                vals += [loop, timer, *ops]
            self.lines.append(f'#DST_{cmd},' + ','.join(str(v) for v in vals))

    def write(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # LR2 でも読めるように、中身は ASCII だけにする（Shift_JIS と同じになる）
        text = '\r\n'.join(self.lines) + '\r\n'
        text.encode('ascii')
        with open(path, 'w', encoding='ascii', newline='') as f:
            f.write(text)
