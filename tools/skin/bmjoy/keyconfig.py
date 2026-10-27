# BMjoy の標準スキン：キーコンフィグ（1920×1080）。黒地に白とグレーの文字、細い線だけ
#   python3 tools/skin/bmjoy/keyconfig.py <出力先の LR2 のフォルダ>
import os
import sys

from common import Csv, Sheet, label, page, render_html

W, H = 1920, 1080
GRAY = '#8a8a8a'
LINE = '#333333'
THEME = 'LR2files\\Theme\\BMjoy\\KeyConfig\\'
# 項目のボタン（番号 - 100 が LR2 の項目）：皿の 2 方向、鍵盤 1〜7、START、SELECT
ITEMS = [(110, 'SC A'), (111, 'SC B'), (101, '1'), (102, '2'), (103, '3'), (104, '4'), (105, '5'), (106, '6'), (107, '7'),
         (112, 'START'), (113, 'SELECT')]
ITEM_W, ITEM_H, ITEM_Y = 130, 110, 220
SLOT_X, SLOT_Y, SLOT_W, SLOT_H = 80, 440, 820, 60  # スロット（2 列 × 4 行）


def build(out):
    root = os.path.join(out, 'LR2files', 'Theme', 'BMjoy', 'KeyConfig')
    csv = Csv()

    def at(x, y, w, h, html):
        return f'<div class="s" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px">{html}</div>'

    bg = [at(0, 0, W, H, '<div style="width:100%;height:100%;background:#000"></div>'),
          at(80, 60, 600, 50, label('KEY CONFIG', 40, '#fff')),
          at(80, 130, W - 160, 1, f'<div style="width:100%;height:100%;background:{LINE}"></div>'),
          at(80, 380, 600, 30, label('SLOT', 22, GRAY))]
    for i in range(8):
        x, y = SLOT_X + (i // 4) * (SLOT_W + 80), SLOT_Y + (i % 4) * (SLOT_H + 16)
        bg.append(at(x - 50, y, 40, SLOT_H, label(str(i + 1), 24, GRAY, align='center')))
    render_html(page(''.join(bg), W, H), W, H, os.path.join(root, 'bg.png'))

    ui = Sheet('ui', 2048)
    box = lambda on, s: (f'<div style="width:100%;height:100%;box-sizing:border-box;border:1px solid {"#fff" if on else "#444"};'
                         f'background:{"#fff" if on else "#000"}">{label(s, 28, "#000" if on else "#ccc", 700 if on else 400, "center")}</div>')
    item_rect = {ty: ui.strip(ITEM_W, ITEM_H, 1, 2, lambda i, s=s: box(i == 1, s)) for ty, s in ITEMS}
    slot_rect = ui.strip(SLOT_W, SLOT_H, 1, 2, lambda i: (f'<div style="width:100%;height:100%;box-sizing:border-box;'
                                                         f'border:{2 if i else 1}px solid {"#fff" if i else "#444"}"></div>'))
    white = ui.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    ui.render(os.path.join(root, 'ui.png'))

    csv.raw('#INFORMATION,8,BMjoy,BMjoy,' + THEME + 'thumb.png')
    csv.raw('#RESOLUTION,2')
    csv.raw('#ENDOFHEADER')
    csv.raw('#IMAGE,' + THEME + 'bg.png')
    csv.raw('#IMAGE,' + THEME + 'ui.png')
    csv.raw('#FONT,30,1,0,')
    csv.raw('#STARTINPUT,100')
    csv.raw('#FADEOUT,300')
    csv.src('IMAGE', 0, 0, (0, 0, W, H))
    csv.dst('IMAGE', 0, [dict(x=0, y=0, w=W, h=H)])
    for i, (ty, s) in enumerate(ITEMS):
        x = 80 + i * (ITEM_W + 10) + (40 if ty >= 112 else 0)  # START・SELECT は少し離す
        csv.src('BUTTON', 0, 1, item_rect[ty], div=(1, 2), ops=(ty, 1, 0, 1))
        csv.dst('BUTTON', 0, [dict(x=x, y=ITEM_Y, w=ITEM_W, h=ITEM_H)])
    for i in range(8):
        x, y = SLOT_X + (i // 4) * (SLOT_W + 80), SLOT_Y + (i % 4) * (SLOT_H + 16)
        csv.src('BUTTON', 0, 1, slot_rect, div=(1, 2), ops=(150 + i, 1, 0, 1))
        csv.dst('BUTTON', 0, [dict(x=x, y=y, w=SLOT_W, h=SLOT_H)])
        csv.raw(f'#SRC_TEXT,0,0,{40 + i},0,0')
        csv.dst('TEXT', 0, [dict(x=x + 24, y=y + (SLOT_H - 30) // 2, w=SLOT_W - 48, h=30)])
    full = dict(x=0, y=0, w=W, h=H, r=0, g=0, b=0)
    csv.src('IMAGE', 0, 1, white)
    csv.dst('IMAGE', 0, [dict(full, a=0), dict(full, time=300, a=255)], timer=2)
    csv.write(os.path.join(root, 'keyconfig.lr2skin'))
    print('書いた:', os.path.join(root, 'keyconfig.lr2skin'))


if __name__ == '__main__':
    build(sys.argv[1])
