# BMjoy の標準スキン：選曲画面（1920×1080）。飾らない：黒地に白とグレーの文字、細い線だけ
#   python3 tools/skin/bmjoy/music_select.py <出力先の LR2 のフォルダ>
import os
import sys

from common import Csv, Sheet, label, page, render_html

# ここを変えて作り直す ---------------------------------------------------------
W, H = 1920, 1080
INFO_X = 80                  # 左の曲の情報の左の端
LIST_X, LIST_W = 1000, 880   # 曲の一覧
ROW_H, ROWS, CENTER = 60, 16, 9  # 1 行の高さ、出す行（1〜ROWS）、選んでいる行
CENTER_Y = 540               # 選んでいる行の上の端
GRAY = '#8a8a8a'
LINE = '#2a2a2a'
LAMP = {1: '#6a3030', 2: '#5fcf6a', 3: '#4f9cff', 4: '#f2f2f2', 5: '#ffd84a', 6: '#ffffff'}  # FAILED … PERFECT
# ---------------------------------------------------------------------------

THEME = 'LR2files\\Theme\\BMjoy\\Select\\'


def build(out):
    root = os.path.join(out, 'LR2files', 'Theme', 'BMjoy', 'Select')
    csv = Csv()
    images = []

    def image(name):
        images.append(name)
        return len(images) - 1

    def at(x, y, w, h, html):
        return f'<div class="s" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px">{html}</div>'

    # ---- 背景（動かない文字と線） ----
    bg = [f'<div class="s" style="left:0;top:0;width:{W}px;height:{H}px;background:#000"></div>']
    bg.append(at(INFO_X, 330, 800, 1, f'<div style="width:100%;height:100%;background:{LINE}"></div>'))
    for x, s in ((0, 'LEVEL'), (200, 'BPM'), (520, 'NOTES')):
        bg.append(at(INFO_X + x, 360, 200, 24, label(s, 20, GRAY)))
    bg.append(at(INFO_X + 330, 400, 30, 40, label('-', 32, GRAY, align='center')))
    bg.append(at(INFO_X, 480, 800, 1, f'<div style="width:100%;height:100%;background:{LINE}"></div>'))
    best = ['EX SCORE', 'RATE', 'MAX COMBO', 'MISS COUNT', 'PLAY COUNT']
    for i, s in enumerate(best):
        bg.append(at(INFO_X, 510 + i * 48, 300, 30, label(s, 22, GRAY)))
    bg.append(at(INFO_X + 470, 558, 30, 30, label('%', 24, GRAY)))
    bg.append(at(INFO_X, 800, 800, 1, f'<div style="width:100%;height:100%;background:{LINE}"></div>'))
    for i, s in enumerate(['MODE', 'RANDOM', 'GAUGE', 'HI-SPEED']):
        bg.append(at(INFO_X + (i % 2) * 400, 830 + (i // 2) * 56, 160, 30, label(s, 22, GRAY)))
    bg.append(at(LIST_X, 20, LIST_W, 1, f'<div style="width:100%;height:100%;background:{LINE}"></div>'))
    render_html(page(''.join(bg), W, H), W, H, os.path.join(root, 'bg.png'))
    gr_bg = image('bg.png')

    # ---- 部品 ----
    ui = Sheet('ui', 2048)
    body = {
        'song': ui.add(LIST_W, ROW_H - 4, '<div style="width:100%;height:100%;background:#0e0e0e"></div>'),
        'folder': ui.add(LIST_W, ROW_H - 4, '<div style="width:100%;height:100%;background:#1c1c1c"></div>'),
    }
    flash = ui.add(LIST_W, ROW_H - 4, '<div style="width:100%;height:100%;box-sizing:border-box;border:2px solid #fff"></div>')
    lamp = {k: ui.add(8, ROW_H - 4, f'<div style="width:100%;height:100%;background:{c}"></div>') for k, c in LAMP.items()}
    digit = ui.strip(16, 28, 10, 1, lambda i: label(str(i), 26, '#fff', align='center'))
    big = ui.strip(26, 44, 10, 1, lambda i: label(str(i), 42, '#fff', align='center'))
    white = ui.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    ui.render(os.path.join(root, 'ui.png'))
    gr_ui = image('ui.png')

    # ---- CSV ----
    csv.raw('#INFORMATION,5,BMjoy,BMjoy,' + THEME + 'thumb.png')
    csv.raw('#RESOLUTION,2')
    csv.raw('#ENDOFHEADER')
    image_at = len(csv.lines)  # #IMAGE の行は、絵を全部作ってからここに入れる
    for size in (56, 32, 28, 24):  # 0 曲名、1 アーティストなど、2 一覧の曲名、3 小さい文字
        csv.raw(f'#FONT,{size},1,0,')
    csv.raw('#STARTINPUT,300')
    csv.raw('#FADEOUT,300')

    csv.src('IMAGE', 0, gr_bg, (0, 0, W, H))
    csv.dst('IMAGE', 0, [dict(x=0, y=0, w=W, h=H)])

    def text(st, font, x, y, w, h, align=0, color=(255, 255, 255)):
        csv.raw(f'#SRC_TEXT,0,{font},{st},{align},0')
        r, g, b = color
        csv.dst('TEXT', 0, [dict(x=x, y=y, w=w, h=h, r=r, g=g, b=b)])

    def number(num, rect, cw, digits, x, y, h, align=0):
        csv.src('NUMBER', 0, gr_ui, rect, div=(10, 1), ops=(num, align, digits))
        csv.dst('NUMBER', 0, [dict(x=x, y=y, w=cw, h=h)])

    # 曲の情報
    gray = (138, 138, 138)
    text(13, 3, INFO_X, 90, 800, 24, color=gray)        # ジャンル
    text(10, 0, INFO_X, 130, 800, 56)                    # 曲名
    text(11, 1, INFO_X, 196, 800, 32)                    # サブタイトル
    text(14, 1, INFO_X, 250, 800, 32, color=gray)        # アーティスト
    number(96, big, 26, 3, INFO_X, 396, 44, align=1)     # レベル
    number(91, big, 26, 4, INFO_X + 220, 396, 44, align=1)  # 最小 BPM
    number(90, big, 26, 4, INFO_X + 370, 396, 44, align=1)  # 最大 BPM
    number(74, big, 26, 5, INFO_X + 520, 396, 44, align=1)  # ノーツ
    for i, num in enumerate((71, 73, 75, 76, 77)):
        number(num, digit, 16, 5, INFO_X + 380, 511 + i * 48, 28)
    # オプション
    for i, st in enumerate((67, 63, 65)):
        text(st, 3, INFO_X + (i % 2) * 400 + 160, 832 + (i // 2) * 56, 220, 26)
    number(10, digit, 16, 4, INFO_X + 560, 833 + 56, 28, align=1)

    # 曲の一覧
    csv.raw(f'#BAR_CENTER,{CENTER}')
    csv.raw(f'#BAR_AVAILABLE,1,{ROWS}')
    for kind in range(10):
        csv.src('BAR_BODY', kind, gr_ui, body['song'] if kind in (0, 5) else body['folder'])
    for i in range(ROWS + 2):
        y = CENTER_Y + (i - CENTER) * ROW_H
        csv.dst('BAR_BODY_OFF', i, [dict(x=LIST_X, y=y, w=LIST_W, h=ROW_H - 4)])
        csv.dst('BAR_BODY_ON', i, [dict(x=LIST_X - 24, y=y, w=LIST_W, h=ROW_H - 4)])
    csv.src('BAR_FLASH', 0, gr_ui, flash)
    csv.dst('BAR_FLASH', 0, [dict(x=0, y=0, w=LIST_W, h=ROW_H - 4)])
    for idx in (0, 1):
        csv.raw(f'#SRC_BAR_TITLE,{idx},2,0,0')
        csv.dst('BAR_TITLE', idx, [dict(x=28, y=(ROW_H - 4 - 28) // 2, w=LIST_W - 140, h=28)])
    for idx in range(7):
        csv.src('BAR_LEVEL', idx, gr_ui, digit, div=(10, 1), ops=(0, 0, 3))
        csv.dst('BAR_LEVEL', idx, [dict(x=LIST_W - 70, y=(ROW_H - 4 - 28) // 2, w=16, h=28)])
    for k, rect in lamp.items():
        csv.src('BAR_LAMP', k, gr_ui, rect)
        csv.dst('BAR_LAMP', k, [dict(x=0, y=0, w=8, h=ROW_H - 4)])
    # 今のフォルダ
    text(30, 3, LIST_X, 30, LIST_W, 24, color=gray)

    # ---- 上のボタン（パネルを開く・キーコンフィグ・スキン） ----
    top = Sheet('top', 2048)
    tabs = [('OPTION', 1), ('SYSTEM', 2), ('KEY CONFIG', 13), ('SKIN', 14)]
    tab_rect = {}
    for name, ty in tabs:
        # 0 = 閉じている（グレー）、1 = 開いている（白）
        tab_rect[ty] = top.strip(200, 40, 1, 2, lambda i, name=name: label(name, 24, '#fff' if i else GRAY, align='center'))
    blank = top.add(8, 8, '')
    judge_names = ['OFF', 'VERY HARD', 'HARD', 'NORMAL', 'EASY'] + [f'JUDGE{n}' for n in range(1, 10)]
    judge_rect = top.strip(300, 40, 1, len(judge_names), lambda i: label(judge_names[i], 26, '#fff', align='center'))
    metro_rect = top.strip(300, 40, 1, 2, lambda i: label(['OFF', 'ON'][i], 26, '#fff', align='center'))
    signed = top.strip(16, 28, 24, 1, lambda i: label({10: '', 11: '+', 22: '', 23: '-'}.get(i, str(i % 12)), 26, '#fff', align='center'))
    knob = top.add(12, 28, '<div style="width:100%;height:100%;background:#fff"></div>')
    top.render(os.path.join(root, 'top.png'))
    gr_top = image('top.png')
    for i, (name, ty) in enumerate(tabs):
        csv.src('BUTTON', 0, gr_top, tab_rect[ty], div=(1, 2), ops=(ty, 1, 0, 1))
        csv.dst('BUTTON', 0, [dict(x=INFO_X + i * 210, y=24, w=200, h=40)])

    # ---- パネル：項目の名前（左）と値（右）。値の左半分を押すと戻り、右半分で進む ----
    PX, PY, PW = INFO_X - 20, 90, 860
    ROW, VX, VW = 52, 400, 420  # 1 行の高さ、値の欄の左の端（パネルから）と幅
    # 項目：(名前, ボタン, 値の出し方)。値は ('text', st) / ('num', num) / ('frames', 絵) / ('slider', type, num) / ('go',)
    panels = {
        1: [('MODE', 11, ('text', 60)), ('SORT', 12, ('text', 61)), ('DIFFICULTY', 10, ('text', 62)),
            ('RANDOM', 42, ('text', 63)), ('GAUGE', 40, ('text', 65)), ('PLAY MODE', 44, ('text', 67)),
            ('HIDDEN / SUDDEN', 50, ('text', 84)), ('LANE COVER', 46, ('text', 73)), ('LIFT', 331, ('frames', metro_rect)),
            ('HS-FIX', 55, ('text', 74)), ('HI-SPEED', 57, ('num', 10)), ('JUDGE', 900, ('frames', judge_rect)),
            ('TARGET', 77, ('text', 1)), ('METRONOME', 902, ('frames', metro_rect))],
        2: [('SCREEN', 80, ('text', 79)), ('BGA', 72, ('text', 76)), ('BGA SIZE', 73, ('text', 75)),
            ('GHOST', 71, ('text', 72)), ('SCORE GRAPH', 70, ('text', 71)), ('JUDGE TIMING', 74, ('signed', 12)),
            ('AUTO ADJUST', 75, ('text', 80)), ('REPLAY SAVE', 83, ('text', 81)), ('TARGET %', 76, ('num', 13)),
            ('MASTER VOLUME', None, ('slider', 17, 57)), ('KEY VOLUME', None, ('slider', 18, 58)),
            ('BGM VOLUME', None, ('slider', 19, 59))],
    }
    for pn, rows in panels.items():
        ph = 960  # 下の欄まで隠す
        pb = [at(0, 0, PW, ph, '<div style="width:100%;height:100%;box-sizing:border-box;background:#000;border:1px solid #444"></div>')]
        for r, (name, ty, kind) in enumerate(rows):
            y = 30 + r * ROW
            pb.append(at(30, y, 360, ROW, label(name, 24, GRAY)))
            if kind[0] != 'slider':
                pb.append(at(VX, y, 30, ROW, label('&lt;', 24, '#555', align='center')))
                pb.append(at(VX + VW - 30, y, 30, ROW, label('&gt;', 24, '#555', align='center')))
            else:
                pb.append(at(VX, y + ROW // 2, 300, 1, f'<div style="width:100%;height:100%;background:#555"></div>'))
        render_html(page(''.join(pb), PW, ph), PW, ph, os.path.join(root, f'panel{pn}.png'))
        gr_panel = image(f'panel{pn}.png')
        show = (20 + pn, 0, 0)  # パネル n を開いている間
        csv.src('IMAGE', 0, gr_panel, (0, 0, PW, ph))
        csv.dst('IMAGE', 0, [dict(x=PX, y=PY, w=PW, h=ph)], ops=show)
        for r, (name, ty, kind) in enumerate(rows):
            y = PY + 30 + r * ROW
            vx = PX + VX + 30
            if ty is not None:
                csv.src('BUTTON', 0, gr_top, blank, ops=(ty, 1, pn, 0))
                csv.dst('BUTTON', 0, [dict(x=PX + VX, y=y, w=VW, h=ROW)], ops=show)
            if kind[0] == 'text':
                csv.raw(f'#SRC_TEXT,0,3,{kind[1]},1,0')
                csv.dst('TEXT', 0, [dict(x=vx + (VW - 60) // 2, y=y + (ROW - 26) // 2, w=VW - 60, h=26)], ops=show)
            elif kind[0] == 'num':
                csv.src('NUMBER', 0, gr_ui, digit, div=(10, 1), ops=(kind[1], 2, 4))
                csv.dst('NUMBER', 0, [dict(x=vx + (VW - 60) // 2 - 32, y=y + (ROW - 28) // 2, w=16, h=28)], ops=show)
            elif kind[0] == 'signed':
                csv.src('NUMBER', 0, gr_top, signed, div=(24, 1), ops=(kind[1], 2, 3))
                csv.dst('NUMBER', 0, [dict(x=vx + (VW - 60) // 2 - 32, y=y + (ROW - 28) // 2, w=16, h=28)], ops=show)
            elif kind[0] == 'frames':
                rect = kind[1]
                csv.src('BUTTON', 0, gr_top, rect, div=(1, rect[3] // 40), ops=(ty, 0, pn, 0))
                csv.dst('BUTTON', 0, [dict(x=vx + (VW - 60) // 2 - 150, y=y + (ROW - 40) // 2, w=300, h=40)], ops=show)
            elif kind[0] == 'slider':
                csv.src('SLIDER', 0, gr_top, knob, ops=(1, 288, kind[1], 0))
                csv.dst('SLIDER', 0, [dict(x=PX + VX, y=y + (ROW - 28) // 2, w=12, h=28)], ops=show)
                csv.src('NUMBER', 0, gr_ui, digit, div=(10, 1), ops=(kind[2], 0, 3))
                csv.dst('NUMBER', 0, [dict(x=PX + VX + 320, y=y + (ROW - 28) // 2, w=16, h=28)], ops=show)

    # 暗転（決めたとき）
    full = dict(x=0, y=0, w=W, h=H, r=0, g=0, b=0)
    csv.src('IMAGE', 0, gr_ui, white)
    csv.dst('IMAGE', 0, [dict(full, a=0), dict(full, time=300, a=255)], timer=2)

    csv.lines[image_at:image_at] = ['#IMAGE,' + THEME + name for name in images]
    csv.write(os.path.join(root, 'select.lr2skin'))
    print('書いた:', os.path.join(root, 'select.lr2skin'))


if __name__ == '__main__':
    build(sys.argv[1])
