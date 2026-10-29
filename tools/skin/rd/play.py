# RD の標準スキン：プレー画面（7 鍵、1P 側、1920×1080）
#   python3 tools/skin/rd/play.py <出力先の LR2 のフォルダ>
# 出力先の LR2files/Theme/RD/Play/ に、絵（PNG）と play_7.lr2skin を書く
import os
import shutil
import sys

from common import Csv, Sheet, label, page, render_html

# ここを変えて作り直す ---------------------------------------------------------
W, H = 1920, 1080
LANE_X = 96            # レーンの左の端
JUDGE_Y = 880          # ノーツの上の端が、判定のときに来る位置
NOTE_H = 16            # ノーツの高さ
GAP = 2                # レーンの間の線
WIDTH = {'S': 118, 'W': 68, 'B': 56}  # 皿・白鍵・青鍵の幅
LANES = ['S', 'W', 'B', 'W', 'B', 'W', 'B', 'W']  # 0 が皿、1〜7 が鍵盤
COLOR = {  # ノーツの色
    'S': ('#ff6b7d', '#e2324a'),
    'W': ('#ffffff', '#c9d2e3'),
    'B': ('#7cc0ff', '#2f86ea'),
}
BEAM = {'S': (255, 110, 130), 'W': (220, 230, 255), 'B': (110, 170, 255)}  # キービームの色
BGA = (1020, 40, 840, 840)  # BGA の位置と大きさ
PANEL_X = 700                # 数字の欄の左の端
GRAY = '#8a8a8a'
LINE = '#333333'
# ---------------------------------------------------------------------------

THEME = 'LR2files\\Theme\\RD\\Play\\'

lane_x, x = [], LANE_X
for t in LANES:
    lane_x.append(x)
    x += WIDTH[t] + GAP
LANES_W = x - GAP - LANE_X
LANE_BOTTOM = JUDGE_Y + NOTE_H
CENTER_X = LANE_X + LANES_W // 2


def lane_w(i):
    return WIDTH[LANES[i]]


def text(s, size, color, stroke=None, italic=False, weight=700, align='center', extra=''):
    """文字（飾らない。stroke と italic は使わない）"""
    return label(s, size, color, weight, align)


def build(out):
    root = os.path.join(out, 'LR2files', 'Theme', 'RD', 'Play')
    csv = Csv()
    images = []  # #IMAGE の順（番号はこの並び）

    def image(name):
        images.append(name)
        return len(images) - 1

    # ---- 背景（動かないもの全部）。黒地に細い線と文字だけ ----
    def at(x, y, w, h, html):
        return f'<div class="s" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px">{html}</div>'

    def rect(x, y, w, h, c):
        return at(x, y, w, h, f'<div style="width:100%;height:100%;background:{c}"></div>')

    bg = [rect(0, 0, W, H, '#000')]
    # レーンの両端とレーンの間の線
    bg.append(rect(LANE_X - 1, 0, 1, H, LINE))
    bg.append(rect(LANE_X + LANES_W, 0, 1, H, LINE))
    for i in range(1, len(LANES)):
        bg.append(rect(lane_x[i] - GAP, 0, GAP, LANE_BOTTOM, '#151515'))
    # 鍵盤（押していないとき）
    keys_y = LANE_BOTTOM + 14
    for i, t in enumerate(LANES):
        cx = lane_x[i] + lane_w(i) / 2
        if t == 'S':
            bg.append(at(cx - 42, keys_y, 84, 84, '<div style="width:100%;height:100%;box-sizing:border-box;border-radius:50%;border:2px solid #444"></div>'))
        else:
            kh, top = (74, keys_y + 8) if t == 'W' else (58, keys_y)
            bg.append(rect(lane_x[i] + 6, top, lane_w(i) - 12, kh, '#bbb' if t == 'W' else '#444'))
    bg.append(rect(LANE_X, 1004, LANES_W, 1, LINE))
    bg.append(at(LANE_X, 1046, 200, 22, label('GAUGE', 18, GRAY)))
    bg.append(at(LANE_X + LANES_W - 36, 1042, 28, 28, label('%', 22, GRAY, align='center')))
    # 数字の欄
    for y, s in ((60, 'BPM'), (190, 'EX SCORE'), (300, 'RATE'), (410, 'MAX COMBO')):
        bg.append(at(PANEL_X, y, 270, 24, label(s, 18, GRAY)))
    for j, s in enumerate(['PGREAT', 'GREAT', 'GOOD', 'BAD', 'POOR']):
        bg.append(at(PANEL_X, 566 + j * 40, 120, 30, label(s, 20, GRAY)))
    bg.append(at(PANEL_X + 172, 330, 40, 44, label('%', 30, GRAY, align='center')))
    # スキンの調整の操作（数字キーを押しながら矢印かホイール。Shift＋ホイールで横）
    help_lines = ['SKIN ADJUST: HOLD 1-6 / 8 / 9 + ARROW KEYS / MOUSE WHEEL (SHIFT + WHEEL = LEFT / RIGHT)',
                  '1 POSITION  /  2 SCALE  /  3 JUDGE POSITION  /  4 NOTE SIZE  /  5 DARK  /  6 NOTE POSITION  /  8 JUDGE SIZE  /  9 NOTE OFFSET']
    for i, s_ in enumerate(help_lines):
        bg.append(at(BGA[0], 1024 + i * 24, BGA[2], 22, label(s_.replace('  ', '&nbsp; '), 15, '#5a5a5a', align='flex-end')))
    # BGA の枠
    bx, by, bw, bh = BGA
    bg.append(at(bx - 1, by - 1, bw + 2, bh + 2, f'<div style="width:100%;height:100%;box-sizing:border-box;border:1px solid {LINE}"></div>'))
    render_html(page(''.join(bg), W, H), W, H, os.path.join(root, 'bg.png'))
    gr_bg = image('bg.png')

    # ---- ノーツ ----
    notes = Sheet('notes', 1024)
    note_rect, ln_body_rect, mine_rect = {}, {}, {}
    for t in ('S', 'W', 'B'):
        hi, lo = COLOR[t]
        w = WIDTH[t]
        note_rect[t] = notes.add(w, NOTE_H, f'<div style="width:100%;height:100%;border-radius:3px;background:linear-gradient({hi},{lo});'
                                            f'box-shadow:inset 0 0 0 1px rgba(0,0,0,.25)"></div>')
        # LN の中：押していないとき（暗い）と押しているとき（明るい）
        ln_body_rect[t] = notes.strip(w, 8, 1, 2, lambda i, lo=lo: f'<div style="width:100%;height:100%;background:{lo};'
                                                                   f'opacity:{0.45 if i == 0 else 0.85}"></div>')
        mine_rect[t] = notes.add(w, NOTE_H, '<div style="width:100%;height:100%;border-radius:3px;'
                                            'background:repeating-linear-gradient(135deg,#ff2d3c 0 6px,#5a0d14 6px 12px)"></div>')
    line_rect = notes.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    judgeline_rect = notes.add(64, 8, '<div style="width:100%;height:100%;background:#e8334a"></div>')
    notes.render(os.path.join(root, 'notes.png'))
    gr_notes = image('notes.png')

    # ---- ボム・キービーム・押した鍵盤 ----
    fx = Sheet('fx', 2048)
    bomb_rect = fx.strip(192, 192, 8, 1, lambda i: (
        f'<div style="position:absolute;left:50%;top:50%;width:{30 + i * 18}px;height:{30 + i * 18}px;transform:translate(-50%,-50%);'
        f'box-sizing:border-box;border-radius:50%;border:3px solid rgba(255,255,255,{1 - i / 8:.2f})"></div>'))
    hold_rect = fx.strip(160, 160, 4, 1, lambda i: (
        f'<div style="position:absolute;left:50%;top:50%;width:{60 + (i % 2) * 10}px;height:{60 + (i % 2) * 10}px;transform:translate(-50%,-50%);'
        f'box-sizing:border-box;border-radius:50%;border:3px solid rgba(255,255,255,.8)"></div>'))
    beam_rect = fx.add(64, 600, '<div style="width:100%;height:100%;background:linear-gradient(rgba(255,255,255,0),rgba(255,255,255,.75))"></div>')
    key_on = {}
    for t in ('S', 'W', 'B'):
        if t == 'S':
            key_on[t] = fx.add(84, 84, '<div style="width:100%;height:100%;box-sizing:border-box;border-radius:50%;border:2px solid #fff"></div>')
        else:
            key_on[t] = fx.add(WIDTH[t] - 12, 74 if t == 'W' else 58, '<div style="width:100%;height:100%;background:#fff"></div>')
    white_rect = fx.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    fx.render(os.path.join(root, 'fx.png'))
    gr_fx = image('fx.png')

    # ---- 判定の文字とコンボの数字 ----
    judge = Sheet('judge', 2048)
    JW, JH, CW = 240, 84, 36
    pg_cols = ['#ffffff', '#9fefff', '#ffffff']  # PGREAT は白と水色で点滅（GREAT の黄色と見分ける）
    judge_rect = {
        5: judge.strip(JW, JH, 1, 3, lambda i: text('GREAT', 60, pg_cols[i], '6px #0b2a4a', align='flex-end')),
        4: judge.add(JW, JH, text('GREAT', 60, '#ffd84a', '6px #4a3000', align='flex-end')),
        3: judge.add(JW, JH, text('GOOD', 60, '#7be08f', '6px #0d3a17', align='flex-end')),
        2: judge.add(JW, JH, text('BAD', 60, '#c79bff', '6px #2c1250', align='flex-end')),
        1: judge.add(JW, JH, text('POOR', 60, '#ff7272', '6px #4a0d0d', align='flex-end')),
    }
    judge_rect[0] = judge_rect[1]
    combo_pg = judge.strip(CW, JH, 10, 3, lambda i: text(str(i % 10), 60, pg_cols[i // 10], '6px #0b2a4a'))
    combo_gr = judge.strip(CW, JH, 10, 1, lambda i: text(str(i), 60, '#ffd84a', '6px #4a3000'))
    judge.render(os.path.join(root, 'judge.png'))
    gr_judge = image('judge.png')

    # ---- 数字・ゲージ・文字の絵 ----
    ui = Sheet('ui', 2048)
    big = ui.strip(30, 54, 10, 1, lambda i: text(str(i), 54, '#ffffff', italic=False, weight=700))
    mid = ui.strip(23, 40, 10, 1, lambda i: text(str(i), 40, '#ffffff', italic=False, weight=700))
    mid0 = ui.strip(23, 40, 11, 1, lambda i: text(str(i % 10), 40, '#ffffff', italic=False, weight=700))  # 0 を詰める（小数）
    small = ui.strip(17, 30, 10, 1, lambda i: text(str(i), 30, '#e6e9f0', italic=False, weight=700))
    gauge_rect = ui.strip(9, 26, 4, 1, lambda i: (
        f'<div style="width:100%;height:100%;background:{["#ff4f6d", "#3fa2ff", "#3a1820", "#122238"][i]}"></div>'))
    dot = ui.add(14, 40, text('.', 40, '#ffffff', italic=False, weight=700))
    fc_rect = ui.add(560, 120, text('FULL COMBO', 80, '#ffffff'))
    failed_rect = ui.add(460, 120, text('FAILED', 80, '#ffffff'))
    ready_rect = ui.add(400, 110, text('READY', 80, '#ffffff'))
    cover_rect = ui.add(64, 512, '<div style="width:100%;height:100%;background:#111;box-shadow:inset 0 -1px 0 #555"></div>')
    knob_rect = ui.add(10, 44, '<div style="width:100%;height:100%;background:#888"></div>')
    ui.render(os.path.join(root, 'ui.png'))
    gr_ui = image('ui.png')

    # ---- CSV ----
    csv.raw('#INFORMATION,0,RD,RD,' + THEME + 'thumb.png')
    csv.raw('#RESOLUTION,2')
    csv.raw('#ENDOFHEADER')
    image_at = len(csv.lines)  # #IMAGE の行は、絵を全部作ってからここに入れる
    csv.raw('#FONT,40,2,0,')
    csv.raw('#FONT,28,2,0,')
    for k, v in (('STARTINPUT', 500), ('LOADSTART', 500), ('LOADEND', 1500), ('PLAYSTART', 1200), ('FADEOUT', 800),
                 ('CLOSE', 1800), ('SCRATCHSIDE', '0,0')):
        csv.raw(f'#{k},{v}')
    whole = (0, 0, W, H)
    full = dict(x=0, y=0, w=W, h=H)

    # 背景
    csv.src('IMAGE', 0, gr_bg, whole)
    csv.dst('IMAGE', 0, [full])
    # BGA
    csv.raw('#SRC_BGA,0,0,0,0,0,0,0,0,0,0,0,0,0')
    bx, by, bw, bh = BGA
    csv.dst('BGA', 0, [dict(x=bx, y=by, w=bw, h=bh)])

    # レーン：小節線・判定ライン・ノーツ
    csv.src('LINE', 0, gr_notes, line_rect)
    csv.dst('LINE', 0, [dict(x=LANE_X, y=LANE_BOTTOM - 1, w=LANES_W, h=1, r=70, g=80, b=100)])
    for i, t in enumerate(LANES):
        csv.src('NOTE', i, gr_notes, note_rect[t])
        csv.src('LN_START', i, gr_notes, note_rect[t])
        csv.src('LN_END', i, gr_notes, note_rect[t])
        csv.src('LN_BODY', i, gr_notes, ln_body_rect[t], div=(1, 2), cycle=160, timer=70 + i)
        csv.src('MINE', i, gr_notes, mine_rect[t])
        csv.dst('NOTE', i, [dict(x=lane_x[i], y=JUDGE_Y, w=lane_w(i), h=NOTE_H)])
    csv.src('JUDGELINE', 0, gr_notes, judgeline_rect)
    csv.dst('JUDGELINE', 0, [dict(x=LANE_X, y=LANE_BOTTOM - 4, w=LANES_W, h=8)])

    # キービーム（押している間と、離してから消えるまで）と、押した鍵盤
    for i, t in enumerate(LANES):
        r, g, b = BEAM[t]
        beam = dict(x=lane_x[i], y=LANE_BOTTOM - 600, w=lane_w(i), h=600, r=r, g=g, b=b, blend=2)
        csv.src('IMAGE', 0, gr_fx, beam_rect)
        csv.dst('IMAGE', 0, [dict(beam, a=200)], timer=100 + i)
        csv.src('IMAGE', 0, gr_fx, beam_rect)
        csv.dst('IMAGE', 0, [dict(beam, a=200), dict(beam, time=120, a=0)], loop=-1, timer=120 + i)
        kx, ky, kw, kh = key_on[t]
        cx = lane_x[i] + lane_w(i) / 2
        if t == 'S':
            k = dict(x=cx - 42, y=LANE_BOTTOM + 14, w=84, h=84)
        else:
            k = dict(x=lane_x[i] + 6, y=LANE_BOTTOM + 14 + (8 if t == 'W' else 0), w=kw, h=kh)
        csv.src('IMAGE', 0, gr_fx, key_on[t])
        csv.dst('IMAGE', 0, [dict(k, a=220)], timer=100 + i)

    # ボム（叩いたとき）と、LN を押している間
    for i in range(8):
        cx, cy = lane_x[i] + lane_w(i) / 2, LANE_BOTTOM - NOTE_H / 2
        csv.src('IMAGE', 0, gr_fx, bomb_rect, div=(8, 1), cycle=200, timer=50 + i)
        b = dict(x=cx - 96, y=cy - 96, w=192, h=192, blend=2)
        csv.dst('IMAGE', 0, [b, dict(b, time=199)], loop=-1, timer=50 + i)
        csv.src('IMAGE', 0, gr_fx, hold_rect, div=(4, 1), cycle=160, timer=70 + i)
        hb = dict(x=cx - 80, y=cy - 80, w=160, h=160, blend=2)
        csv.dst('IMAGE', 0, [hb, dict(hb, time=160)], timer=70 + i)

    # レーンカバー（SUD+）と曲の進み
    csv.src('SLIDER', 0, gr_ui, cover_rect, ops=(2, LANE_BOTTOM, 4, 1))
    csv.dst('SLIDER', 0, [dict(x=LANE_X, y=-LANE_BOTTOM, w=LANES_W, h=LANE_BOTTOM)])
    csv.src('SLIDER', 0, gr_ui, knob_rect, ops=(2, LANE_BOTTOM - 44, 6, 1))
    csv.dst('SLIDER', 0, [dict(x=LANE_X - 24, y=0, w=10, h=44)])

    # 判定とコンボ
    jx, jy = CENTER_X - JW // 2, 560
    for j in range(6):
        rect = judge_rect[j]
        frames = (1, 3) if j == 5 else (1, 1)
        csv.src('NOWJUDGE_1P', j, gr_judge, rect, div=frames, cycle=90 if j == 5 else 0)
        jd = dict(x=jx, y=jy, w=JW, h=JH)
        csv.dst('NOWJUDGE_1P', j, [dict(jd, x=jx, y=jy - 6), dict(jd, time=60), dict(jd, time=900)], loop=-1, timer=46)
        if j >= 3:
            if j == 5:
                csv.src('NOWCOMBO_1P', j, gr_judge, combo_pg, div=(10, 3), cycle=90, ops=(0, 1, 6))
            else:
                csv.src('NOWCOMBO_1P', j, gr_judge, combo_gr, div=(10, 1), ops=(0, 1, 6))
            cd = dict(x=JW + 14, y=0, w=CW, h=JH)
            csv.dst('NOWCOMBO_1P', j, [dict(cd, y=-6), dict(cd, time=60), dict(cd, time=900)], loop=-1, timer=46)

    # ゲージ
    csv.src('GROOVEGAUGE', 0, gr_ui, gauge_rect, div=(4, 1), ops=(11, 0))
    csv.dst('GROOVEGAUGE', 0, [dict(x=LANE_X + 10, y=1016, w=9, h=26)])
    csv.src('NUMBER', 0, gr_ui, small, div=(10, 1), ops=(107, 0, 3))
    csv.dst('NUMBER', 0, [dict(x=LANE_X + LANES_W - 90, y=1040, w=17, h=30)])

    # 数字の欄
    def number(num, rect, digits, x, y, w, h, align=0):
        csv.src('NUMBER', 0, gr_ui, rect, div=(rect[2] // w, 1), ops=(num, align, digits))
        csv.dst('NUMBER', 0, [dict(x=x, y=y, w=w, h=h)])
    number(160, big, 3, PANEL_X, 88, 30, 54)
    number(101, big, 5, PANEL_X, 218, 30, 54)
    number(102, mid, 3, PANEL_X + 40, 330, 23, 40)
    csv.src('IMAGE', 0, gr_ui, dot)
    csv.dst('IMAGE', 0, [dict(x=PANEL_X + 109, y=330, w=14, h=40)])
    number(103, mid0, 2, PANEL_X + 123, 330, 23, 40)
    number(105, big, 5, PANEL_X, 438, 30, 54)
    for j in range(5):
        number(110 + j, small, 5, PANEL_X + 180, 566 + j * 40, 17, 30)

    # 曲名・アーティスト・ジャンル
    bx, by, bw, bh = BGA
    for st, font, y in ((12, 0, by + bh + 40), (14, 1, by + bh + 100)):
        csv.raw(f'#SRC_TEXT,0,{font},{st},0,0')
        csv.dst('TEXT', 0, [dict(x=bx, y=y, w=bw, h=40 if font == 0 else 28)])

    # 始まり（READY）・フルコンボ・閉店・終わり
    rx, ry, rw, rh = ready_rect
    r = dict(x=CENTER_X - rw / 2, y=400, w=rw, h=rh)
    csv.src('IMAGE', 0, gr_ui, ready_rect)
    csv.dst('IMAGE', 0, [dict(r, a=0), dict(r, time=200), dict(r, time=1000), dict(r, time=1200, a=0)], loop=-1, timer=40)
    fw_ = fc_rect[2]
    f = dict(x=CENTER_X - fw_ / 2, y=380, w=fw_, h=120)
    csv.src('IMAGE', 0, gr_ui, fc_rect)
    csv.dst('IMAGE', 0, [dict(f, a=0, y=420), dict(f, time=200), dict(f, time=2400), dict(f, time=2800, a=0)], loop=-1, timer=48)
    lanes_box = dict(x=LANE_X, y=0, w=LANES_W, h=LANE_BOTTOM, r=0, g=0, b=0)
    csv.src('IMAGE', 0, gr_fx, white_rect)
    csv.dst('IMAGE', 0, [dict(lanes_box, a=0), dict(lanes_box, time=600, a=170)], timer=3)
    fl = dict(x=CENTER_X - failed_rect[2] / 2, y=380, w=failed_rect[2], h=120)
    csv.src('IMAGE', 0, gr_ui, failed_rect)
    csv.dst('IMAGE', 0, [dict(fl, a=0), dict(fl, time=300)], timer=3)
    csv.src('IMAGE', 0, gr_fx, white_rect)
    csv.dst('IMAGE', 0, [dict(full, a=0, r=0, g=0, b=0), dict(full, time=800, a=255, r=0, g=0, b=0)], timer=2)

    # スキンを選ぶ画面の見本（遊んでいる画面を撮ったもの）
    thumb = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'play_thumb.png')
    if os.path.exists(thumb):
        shutil.copy(thumb, os.path.join(root, 'thumb.png'))
    csv.lines[image_at:image_at] = ['#IMAGE,' + THEME + name for name in images]
    csv.write(os.path.join(root, 'play_7.lr2skin'))
    print('書いた:', os.path.join(root, 'play_7.lr2skin'))


if __name__ == '__main__':
    build(sys.argv[1])
