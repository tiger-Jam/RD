# BMjoy の標準スキン：リザルト画面とコースのリザルト画面（1920×1080）。黒地に白とグレーの文字、細い線だけ
#   python3 tools/skin/bmjoy/result.py <出力先の LR2 のフォルダ>
import os
import sys

from common import Csv, Sheet, label, page, render_html

# ここを変えて作り直す ---------------------------------------------------------
W, H = 1920, 1080
LEFT = 80                    # 左の端
TABLE_X = 960                # 数字の表の左の端
GRAPH = (80, 620, 760, 360)  # ゲージの推移の位置と大きさ（左上、幅、高さ）
GRAY = '#8a8a8a'
LINE = '#333333'
# ---------------------------------------------------------------------------

RANKS = ['AAA', 'AA', 'A', 'B', 'C', 'D', 'E', 'F']


def build(out, course=False):
    folder = 'CourseResult' if course else 'Result'
    THEME = 'LR2files\\Theme\\BMjoy\\' + folder + '\\'
    root = os.path.join(out, 'LR2files', 'Theme', 'BMjoy', folder)
    csv = Csv()
    images = []

    def image(name):
        images.append(name)
        return len(images) - 1

    def at(x, y, w, h, html):
        return f'<div class="s" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px">{html}</div>'

    def rect(x, y, w, h, c):
        return at(x, y, w, h, f'<div style="width:100%;height:100%;background:{c}"></div>')

    # ---- 背景 ----
    gx, gy, gw, gh = GRAPH
    rows = ['EX SCORE', 'RATE', 'MAX COMBO', 'MISS COUNT', '', 'PGREAT', 'GREAT', 'GOOD', 'BAD', 'POOR']
    bg = [rect(0, 0, W, H, '#000'), rect(LEFT, 180, W - LEFT * 2, 1, LINE)]
    bg.append(at(TABLE_X + 300, 210, 160, 26, label('NOW', 20, GRAY, align='flex-end')))
    bg.append(at(TABLE_X + 500, 210, 160, 26, label('BEST', 20, GRAY, align='flex-end')))
    bg.append(at(TABLE_X + 700, 210, 160, 26, label('DIFF', 20, GRAY)))
    for i, s in enumerate(rows):
        if s:
            bg.append(at(TABLE_X, 250 + i * 64, 280, 40, label(s, 26, GRAY)))
    bg.append(at(TABLE_X + 470, 250 + 64, 40, 40, label('%', 28, GRAY, align='center')))
    bg.append(at(gx, gy - 36, 300, 26, label('GAUGE', 20, GRAY)))
    bg.append(at(gx - 1, gy - 1, gw + 2, gh + 2, f'<div style="width:100%;height:100%;box-sizing:border-box;border:1px solid {LINE}"></div>'))
    bg.append(rect(gx, gy + gh * 0.2, gw, 1, '#222'))  # 80% の線
    render_html(page(''.join(bg), W, H), W, H, os.path.join(root, 'bg.png'))
    gr_bg = image('bg.png')

    # ---- 部品 ----
    ui = Sheet('ui', 2048)
    clear = ui.add(600, 120, label('CLEAR', 110, '#fff', 700))
    failed = ui.add(600, 120, label('FAILED', 110, GRAY, 700))
    rank = {r: ui.add(400, 220, label(r, 200, '#fff', 700)) for r in RANKS}
    big = ui.strip(26, 44, 10, 1, lambda i: label(str(i), 42, '#fff', 700, 'center'))
    big0 = ui.strip(26, 44, 11, 1, lambda i: label(str(i % 10), 42, '#fff', 700, 'center'))
    small = ui.strip(20, 34, 10, 1, lambda i: label(str(i), 32, GRAY, 400, 'center'))
    signed = ui.strip(20, 34, 24, 1, lambda i: label({10: '', 11: '+', 22: '', 23: '-'}.get(i, str(i % 12)), 32, '#fff', 400, 'center'))
    dot = ui.add(12, 44, label('.', 42, '#fff', 700, 'center'))
    point_lo = ui.add(4, 4, '<div style="width:100%;height:100%;background:#3fa2ff"></div>')
    point_hi = ui.add(4, 4, '<div style="width:100%;height:100%;background:#ff4f6d"></div>')
    white = ui.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    ui.render(os.path.join(root, 'ui.png'))
    gr_ui = image('ui.png')

    # ---- CSV ----
    csv.raw(f'#INFORMATION,{15 if course else 7},BMjoy,BMjoy,' + THEME + 'thumb.png')
    csv.raw('#RESOLUTION,2')
    csv.raw('#ENDOFHEADER')
    csv.lines += ['#IMAGE,' + THEME + n for n in images]
    for size in (48, 28):  # 0 曲名、1 アーティスト
        csv.raw(f'#FONT,{size},1,0,')
    csv.raw('#STARTINPUT,300,300,300')
    csv.raw('#FADEOUT,300')

    csv.src('IMAGE', 0, gr_bg, (0, 0, W, H))
    csv.dst('IMAGE', 0, [dict(x=0, y=0, w=W, h=H)])
    # 曲名とアーティスト（コースならコースの名前だけ）
    for st, font, y, h, c in (((170, 0, 60, 48, 255),) if course else ((12, 0, 60, 48, 255), (14, 1, 125, 28, 138))):
        csv.raw(f'#SRC_TEXT,0,{font},{st},0,0')
        csv.dst('TEXT', 0, [dict(x=LEFT, y=y, w=W - LEFT * 2, h=h, r=c, g=c, b=c)])

    # クリアか失敗か、ランク
    for rect_, op in ((clear, 90), (failed, 91)):
        csv.src('IMAGE', 0, gr_ui, rect_)
        k = dict(x=LEFT, y=220, w=600, h=120)
        csv.dst('IMAGE', 0, [dict(k, a=0), dict(k, time=200)], ops=(op, 0, 0))
    for i, r in enumerate(RANKS):
        csv.src('IMAGE', 0, gr_ui, rank[r])
        k = dict(x=LEFT, y=350, w=400, h=220)
        csv.dst('IMAGE', 0, [dict(k, a=0), dict(k, time=300)], ops=(300 + i, 0, 0))

    def number(num, rect_, cw, h, digits, x, y, align=0):
        div = rect_[2] // cw
        csv.src('NUMBER', 0, gr_ui, rect_, div=(div, 1), ops=(num, align, digits))
        csv.dst('NUMBER', 0, [dict(x=x, y=y, w=cw, h=h)])

    # 表：今回・自己ベスト（更新前）・差
    def row(i, now, best, diff, digits=5):
        y = 250 + i * 64
        number(now, big, 26, 44, digits, TABLE_X + 460 - 26 * digits, y - 2)
        if best is not None:
            number(best, small, 20, 34, digits, TABLE_X + 660 - 20 * digits, y + 4)
            number(diff, signed, 20, 34, digits, TABLE_X + 700, y + 4, align=1)  # 符号のすぐ後ろに数字
    row(0, 171, 170, 172)
    number(102, big, 26, 44, 3, TABLE_X + 300, 250 + 64 - 2)
    csv.src('IMAGE', 0, gr_ui, dot)
    csv.dst('IMAGE', 0, [dict(x=TABLE_X + 378, y=250 + 64 - 2, w=12, h=44)])
    number(103, big0, 26, 44, 2, TABLE_X + 390, 250 + 64 - 2)
    row(2, 174, 173, 175)
    row(3, 177, 176, 178)
    for j in range(5):
        row(5 + j, 110 + j, None, None)

    # ゲージの推移（0 は線より下、1 は上の点。下の端から上へ）
    for idx, pt in ((0, point_lo), (1, point_hi)):
        csv.src('GAUGECHART_1P', idx, gr_ui, pt, ops=(gw, gh, 300, 1300))
        csv.dst('GAUGECHART_1P', idx, [dict(x=gx, y=gy + gh - 3, w=2, h=3)])

    # 暗転
    full = dict(x=0, y=0, w=W, h=H, r=0, g=0, b=0)
    csv.src('IMAGE', 0, gr_ui, white)
    csv.dst('IMAGE', 0, [dict(full, a=0), dict(full, time=300, a=255)], timer=2)

    # コースの各ステージの曲名（表の下）
    if course:
        for i in range(5):
            csv.raw(f'#SRC_TEXT,0,1,{150 + i},0,0')
            csv.dst('TEXT', 0, [dict(x=TABLE_X, y=900 + i * 34, w=W - TABLE_X - LEFT, h=28, r=138, g=138, b=138)])
    name = 'courseresult.lr2skin' if course else 'result.lr2skin'
    csv.write(os.path.join(root, name))
    print('書いた:', os.path.join(root, name))


if __name__ == '__main__':
    build(sys.argv[1])
    build(sys.argv[1], course=True)
