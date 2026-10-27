# BMjoy の標準スキン：決定画面（1920×1080）。黒地に曲名だけ
#   python3 tools/skin/bmjoy/decide.py <出力先の LR2 のフォルダ>
import os
import sys

from common import Csv, Sheet

W, H = 1920, 1080
THEME = 'LR2files\\Theme\\BMjoy\\Decide\\'


def build(out):
    root = os.path.join(out, 'LR2files', 'Theme', 'BMjoy', 'Decide')
    csv = Csv()
    ui = Sheet('ui', 64)
    white = ui.add(8, 8, '<div style="width:100%;height:100%;background:#fff"></div>')
    ui.render(os.path.join(root, 'ui.png'))

    csv.raw('#INFORMATION,6,BMjoy,BMjoy,' + THEME + 'thumb.png')
    csv.raw('#RESOLUTION,2')
    csv.raw('#ENDOFHEADER')
    csv.raw('#IMAGE,' + THEME + 'ui.png')
    for size in (64, 32, 24):  # 0 曲名、1 アーティスト、2 ジャンル
        csv.raw(f'#FONT,{size},1,0,')
    for k, v in (('STARTINPUT', 300), ('SCENETIME', 2000), ('FADEOUT', 300), ('SKIP', 200)):
        csv.raw(f'#{k},{v}')
    full = dict(x=0, y=0, w=W, h=H, r=0, g=0, b=0)
    csv.src('IMAGE', 0, 0, white)
    csv.dst('IMAGE', 0, [full])
    # ジャンル・曲名・サブタイトル・アーティスト（真ん中にそろえる）
    for st, font, y, h, c in ((13, 2, 400, 24, 138), (10, 0, 450, 64, 255), (11, 1, 530, 32, 255), (14, 1, 600, 32, 138)):
        csv.raw(f'#SRC_TEXT,0,{font},{st},1,0')
        k = dict(x=W // 2, y=y, w=1600, h=h, r=c, g=c, b=c)
        csv.dst('TEXT', 0, [dict(k, a=0), dict(k, time=200)])
    # 暗転（時間が来たとき・飛ばしたとき）
    for timer in (2, 3):
        csv.src('IMAGE', 0, 0, white)
        csv.dst('IMAGE', 0, [dict(full, a=0), dict(full, time=300, a=255)], timer=timer)
    csv.write(os.path.join(root, 'decide.lr2skin'))
    print('書いた:', os.path.join(root, 'decide.lr2skin'))


if __name__ == '__main__':
    build(sys.argv[1])
