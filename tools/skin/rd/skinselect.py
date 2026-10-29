# RD の標準スキン：スキンセレクト（1920×1080）。黒地に白とグレーの文字、細い線だけ
#   python3 tools/skin/rd/skinselect.py <出力先の LR2 のフォルダ>
import os
import sys

from common import Csv, Sheet, label, page, render_html

W, H = 1920, 1080
GRAY = '#8a8a8a'
LINE = '#333333'
THEME = 'LR2files\\Theme\\RD\\SkinSelect\\'
# スキンの種類のボタン（170 + 種類）
TYPES = [(170, 'PLAY'), (175, 'SELECT'), (176, 'DECIDE'), (177, 'RESULT'), (185, 'COURSE RESULT'), (178, 'KEY CONFIG'),
         (179, 'SKIN SELECT'), (180, 'SOUND')]
PREVIEW = (480, 140, 1280, 720)  # 試し見（画像 105）
ROW = 52


def build(out):
    root = os.path.join(out, 'LR2files', 'Theme', 'RD', 'SkinSelect')
    csv = Csv()
    px, py, pw, ph = PREVIEW

    def at(x, y, w, h, html):
        return f'<div class="s" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px">{html}</div>'

    bg = [at(0, 0, W, H, '<div style="width:100%;height:100%;background:#000"></div>'),
          at(80, 40, 600, 50, label('SKIN', 40, '#fff')),
          at(80, 110, W - 160, 1, f'<div style="width:100%;height:100%;background:{LINE}"></div>'),
          at(px - 1, py - 1, pw + 2, ph + 2, f'<div style="width:100%;height:100%;box-sizing:border-box;border:1px solid {LINE}"></div>'),
          at(px, py + ph + 24, 40, 40, label('&lt;', 30, '#555', align='center')),
          at(px + 760, py + ph + 24, 40, 40, label('&gt;', 30, '#555', align='center'))]
    render_html(page(''.join(bg), W, H), W, H, os.path.join(root, 'bg.png'))

    ui = Sheet('ui', 2048)
    type_rect = {ty: ui.strip(340, 44, 1, 2, lambda i, s=s: label(s, 24, '#fff' if i else GRAY, 700 if i else 400))
                 for ty, s in TYPES}
    blank = ui.add(8, 8, '')
    knob = ui.add(10, 40, '<div style="width:100%;height:100%;background:#888"></div>')
    white = ui.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    ui.render(os.path.join(root, 'ui.png'))

    csv.raw('#INFORMATION,9,RD,RD,' + THEME + 'thumb.png')
    csv.raw('#RESOLUTION,2')
    csv.raw('#ENDOFHEADER')
    csv.raw('#IMAGE,' + THEME + 'bg.png')
    csv.raw('#IMAGE,' + THEME + 'ui.png')
    csv.raw('#FONT,32,1,0,')
    csv.raw('#FONT,22,1,0,')
    csv.raw('#STARTINPUT,100')
    csv.raw('#FADEOUT,300')
    csv.src('IMAGE', 0, 0, (0, 0, W, H))
    csv.dst('IMAGE', 0, [dict(x=0, y=0, w=W, h=H)])

    # 種類（左）
    for i, (ty, s) in enumerate(TYPES):
        csv.src('BUTTON', 0, 1, type_rect[ty], div=(1, 2), ops=(ty, 1, 0, 1))
        csv.dst('BUTTON', 0, [dict(x=80, y=140 + i * ROW, w=340, h=44)])

    # 試し見と、スキンの名前（左半分を押すと前、右半分で次）・作者
    csv.src('IMAGE', 0, 105, (0, 0, pw, ph))
    csv.dst('IMAGE', 0, [dict(x=px, y=py, w=pw, h=ph)])
    csv.src('BUTTON', 0, 1, blank, ops=(190, 1, 0, 0))
    csv.dst('BUTTON', 0, [dict(x=px, y=py + ph + 20, w=800, h=48)])
    csv.raw('#SRC_TEXT,0,0,50,1,0')
    csv.dst('TEXT', 0, [dict(x=px + 400, y=py + ph + 26, w=700, h=32)])
    csv.raw('#SRC_TEXT,0,1,51,1,0')
    csv.dst('TEXT', 0, [dict(x=px + 400, y=py + ph + 70, w=700, h=22, r=138, g=138, b=138)])

    # カスタマイズ（左下。5 行ずつ。右のつまみで送る）
    cy = 140 + len(TYPES) * ROW + 40
    for r in range(5):
        y = cy + r * ROW
        csv.raw(f'#SRC_TEXT,0,1,{100 + r},0,0')
        csv.dst('TEXT', 0, [dict(x=80, y=y, w=340, h=22, r=138, g=138, b=138)])
        csv.src('BUTTON', 0, 1, blank, ops=(220 + r, 1, 0, 0))
        csv.dst('BUTTON', 0, [dict(x=80, y=y + 22, w=340, h=28)])
        csv.raw(f'#SRC_TEXT,0,1,{110 + r},1,0')
        csv.dst('TEXT', 0, [dict(x=80 + 170, y=y + 24, w=300, h=22)])
    csv.src('SLIDER', 0, 1, knob, ops=(2, 5 * ROW - 40, 7, 0))
    csv.dst('SLIDER', 0, [dict(x=440, y=cy, w=10, h=40)])

    full = dict(x=0, y=0, w=W, h=H, r=0, g=0, b=0)
    csv.src('IMAGE', 0, 1, white)
    csv.dst('IMAGE', 0, [dict(full, a=0), dict(full, time=300, a=255)], timer=2)
    csv.write(os.path.join(root, 'skinselect.lr2skin'))
    print('書いた:', os.path.join(root, 'skinselect.lr2skin'))


if __name__ == '__main__':
    build(sys.argv[1])
