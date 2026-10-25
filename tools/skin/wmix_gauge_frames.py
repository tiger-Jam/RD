#!/usr/bin/env python3
# W-MIX（WMIX_HD）の選曲画面の GAUGE のボタンに、BMjoy の GAS・EX-HARD・ASSIST・CUSTOM1〜9 のコマを足す。
#
#   python3 tools/skin/wmix_gauge_frames.py <スキンの select のフォルダ>
#
# フォルダは LR2files/Theme/WMIX_HD/select（select.csv と system/main.png・system/pannel.png がある所）。
# - system/bmjoy_gauge.png を作る（GAUGE の 18 コマ：0〜5 は元の 6 つ、6=GAS 7=EX-HARD 8=ASSIST 9〜17=CUSTOM1〜9）。
#   上が下の欄の帯（68×14 のコマ）、その下が設定パネルの帯（116×21 のコマ）
# - system/bmjoy_judge.png を作る（設定パネルの左上に置く判定の欄：題が JUDGE の箱と、値 OFF・VERY HARD〜EASY・JUDGE1〜9 の 14 コマ）
# - select.csv に #IMAGE を足し、GAUGE のボタン（下の欄 1 つ・パネル 3 つ）をその絵の 18 コマにする。
#   判定の欄（SP のパネルの左上。BMjoy のボタン 900）とメトロノームの欄（SP のパネルの BATTLE の下。ボタン 902）の行を足す
# - system/bmjoy_mode.png を作り、ASSIST の欄（ボタン 44）を 7k モードの欄（題 PLAY MODE、値 7K+S・7K）にする
#   元の select.csv は select.csv.bak-bmjoy に残す（もう書き換えてあれば、絵だけ作り直して行を直す）
# 字はスキンのほかのボタン・数字の絵から 1 字ずつ切り出して並べる（字の芯がつながった塊を 1 字とし、周りの縁も含める）。
# 下の欄の字は白にそろえる。要るもの：ffmpeg。スキンを入れ直してコマが消えたら、もう一度これを流す
import os
import shutil
import statistics
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    sys.exit(__doc__ or "使い方: wmix_gauge_frames.py <スキンの select のフォルダ>")
SELECT = sys.argv[1]
SYSTEM = os.path.join(SELECT, "system")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glyphs as gl  # noqa: E402
from glyphs import Glyph, compose, fit_height, glyphs, library, raw  # noqa: E402
gl.DIR = SYSTEM
WORDS = ['GAS', 'EX-HARD', 'ASSIST'] + [f'CUSTOM{i}' for i in range(1, 10)]

def strip(img, x, y, w, fh, spec, digits, th, reach, white):
    base = raw(img, x, y, w, fh * 6)
    lib = library(spec, th, reach, white)
    for d in range(10):
        dg = glyphs(*digits(d), th, reach, white)
        assert len(dg) == 1, (d, len(dg))
        lib[str(d)] = fit_height(dg[0], lib['H'].cy1 - lib['H'].cy0 + 1)
    h = lib['H']; a = lib['A']; r = lib['R']; dd = lib['D']
    gap = int(statistics.median([a.cx0 - h.cx1 - 1, r.cx0 - a.cx1 - 1, dd.cx0 - r.cx1 - 1]))
    bg = base[0:4]
    out = bytearray(base)
    for wd in WORDS:
        out += compose(wd, lib, w, fh, gap, bg)
    return bytes(out), lib

G = 'main.png'
bar_spec = [
    ((G, 136, 2250, 68, 14), 'GROOVE'), ((G, 136, 2264, 68, 14), 'HARD'), ((G, 136, 2292, 68, 14), 'EASY'),
    ((G, 136, 2306, 68, 14), 'P-ATK'), ((G, 7, 2264, 39, 14), 'MAX'), ((G, 7, 2278, 39, 14), 'MIN'),
    ((G, 7, 2306, 39, 14), 'CST'), ((G, 586, 2278, 46, 14), 'AUTO'),
]
P = 'pannel.png'
pan_spec = [
    ((P, 658, 1297, 116, 21), 'GROOVE'), ((P, 658, 1318, 116, 21), 'HARD'), ((P, 658, 1360, 116, 21), 'EASY'),
    ((P, 658, 1381, 116, 21), 'P-ATTACK'), ((P, 903, 1521, 116, 21), 'MAXBPM', (1, 44)), ((P, 534, 1318, 116, 21), 'MIRROR'),
    ((P, 782, 1339, 116, 21), 'SUDDEN'), ((P, 782, 1444, 116, 21), 'FLIP'),
]

# --- 絵を作る
bar, _ = strip(G, 136, 2250, 68, 14, bar_spec, lambda d: (G, 910 + 13 * d, 1985, 13, 15), 120, 2, True)
pan, pan_lib = strip(P, 658, 1297, 116, 21, pan_spec, lambda d: (P, 12 * d, 406, 12, 13), 120, 1, False)
n = 6 + len(WORDS)
BAR_H, PAN_Y, PAN_H = 14 * n, 14 * n + 2, 21 * n
W, H = 116, PAN_Y + PAN_H
img = bytearray(b'\0\0\0\0' * (W * H))
def put(src, sw, sh, oy):
    for yy in range(sh):
        img[((oy + yy) * W) * 4:((oy + yy) * W + sw) * 4] = src[yy * sw * 4:(yy + 1) * sw * 4]
put(bar, 68, BAR_H, 0)
put(pan, 116, PAN_H, PAN_Y)
with tempfile.TemporaryDirectory() as tmp:
    raw_path = os.path.join(tmp, "gauge.raw")
    open(raw_path, "wb").write(img)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{H}", "-i", raw_path,
                    os.path.join(SYSTEM, "bmjoy_gauge.png")], check=True)
print("作った:", os.path.join(SYSTEM, "bmjoy_gauge.png"))

# --- 判定の欄
def make_j(u):
    # J は U から作る：左の縦棒の上 6 割を削る（このスキンの字に J が無い）
    mid = u.cy0 + (u.cy1 - u.cy0) // 2
    stem = u.cx0
    while (stem + 1, mid) in u.core: stem += 1
    cut = u.cy0 + int((u.cy1 - u.cy0 + 1) * 0.6)
    gone = lambda q: q[0] <= stem + 1 and q[1] < cut
    core = {q for q in u.core if not gone(q)}
    j = Glyph({q: v for q, v in u.px.items() if not gone(q)}, core, min(q[0] for q in core), u.cx1, u.cy0, u.cy1)
    j.base = u.base
    return j

pan_lib["J"] = make_j(pan_lib["U"])
gap_pan = int(statistics.median([pan_lib["A"].cx0 - pan_lib["H"].cx1 - 1, pan_lib["R"].cx0 - pan_lib["A"].cx1 - 1, pan_lib["D"].cx0 - pan_lib["R"].cx1 - 1]))
pan_bg = raw(P, 658, 1297, 116, 21)[0:4]
# 値の帯：OFF、本体の判定（VERY HARD … EASY。BMjoy の kStandardJudgeIds の順）、JUDGE1〜9 の 14 コマ
JUDGE_WORDS = ["VERY HARD", "HARD", "NORMAL", "EASY"] + [f"JUDGE{i}" for i in range(1, 10)]
judge_values = raw(P, 782, 1297, 116, 21)  # OFF（SUDDEN+ の欄の 1 コマ目）
for wd in JUDGE_WORDS:
    judge_values += compose(wd, pan_lib, 116, 21, gap_pan, pan_bg)
JN = 1 + len(JUDGE_WORDS)
# 箱：TARGET の箱を灰色にし（STYLE の箱と同じ色合い）、題を JUDGE に描き替える。題の字は SUDDEN+ と GROOVE GAUGE の箱から
# 箱の枠の内側（左 2・右 4・上 2 画素を除いた 136×20）だけを見る（枠の線が字とつながらないように）
title_spec = [((P, 192, 1321, 136, 20), "SUDDEN+"), ((P, 192, 1385, 136, 20), "GROOVEGAUGE"), ((P, 337, 1684, 136, 20), "HI-SPEED"), ((P, 825, 1159, 136, 20), "TARGET")]
title_lib = library(title_spec, 120, 1, False)
title_lib["J"] = make_j(title_lib["U"])
title_lib["M"] = gl.make_m(title_lib["H"], title_lib["V"])  # 箱の題の字に M は無い
gs = [title_lib[c] for c in "GRO"]
gap_title = int(statistics.median([gs[1].cx0 - gs[0].cx1 - 1, gs[2].cx0 - gs[1].cx1 - 1]))
def make_box(word):
    # TARGET の箱を灰色にし（STYLE の箱と同じ色合い）、題を word に描き替える
    box = bytearray(raw(P, 823, 1157, 142, 59))
    for q in range(0, len(box), 4):
        box[q] = box[q + 1] = box[q + 2] = max(box[q], box[q + 1], box[q + 2])
    inside = bytes(box[(5 * 142 + 5) * 4:(5 * 142 + 5) * 4 + 4])
    title = compose(word, title_lib, 138, 20, gap_title, inside)
    for yy in range(20):
        box[((yy + 2) * 142 + 2) * 4:((yy + 2) * 142 + 140) * 4] = title[yy * 138 * 4:(yy + 1) * 138 * 4]
    return box


box = make_box("JUDGE")
JW, JH = 142, 61 + 21 * JN
jimg = bytearray(b'\0\0\0\0' * (JW * JH))
for yy in range(59):
    jimg[yy * JW * 4:(yy * JW + 142) * 4] = box[yy * 142 * 4:(yy + 1) * 142 * 4]
for yy in range(21 * JN):
    jimg[((61 + yy) * JW) * 4:((61 + yy) * JW + 116) * 4] = judge_values[yy * 116 * 4:(yy + 1) * 116 * 4]
with tempfile.TemporaryDirectory() as tmp:
    raw_path = os.path.join(tmp, "judge.raw")
    open(raw_path, "wb").write(jimg)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{JW}x{JH}", "-i", raw_path,
                    os.path.join(SYSTEM, "bmjoy_judge.png")], check=True)
print("作った:", os.path.join(SYSTEM, "bmjoy_judge.png"))

# --- メトロノームの欄（SP のパネルの BATTLE の下。DP では DP FLIP の箱がある場所）：題が METRONOME の箱と、値 OFF・ON
mbox = make_box("METRONOME")
MW, MH = 142, 61 + 21 * 2
mimg = bytearray(b'\0\0\0\0' * (MW * MH))
for yy in range(59):
    mimg[yy * MW * 4:(yy * MW + 142) * 4] = mbox[yy * 142 * 4:(yy + 1) * 142 * 4]
onoff = raw(P, 782, 1381, 116, 42)  # SUDDEN+ の欄の OFF・ON
for yy in range(42):
    mimg[((61 + yy) * MW) * 4:((61 + yy) * MW + 116) * 4] = onoff[yy * 116 * 4:(yy + 1) * 116 * 4]
with tempfile.TemporaryDirectory() as tmp:
    raw_path = os.path.join(tmp, "metro.raw")
    open(raw_path, "wb").write(mimg)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{MW}x{MH}", "-i", raw_path,
                    os.path.join(SYSTEM, "bmjoy_metronome.png")], check=True)
print("作った:", os.path.join(SYSTEM, "bmjoy_metronome.png"))

# --- 7k モードの欄（ASSIST の欄を使う。BMjoy のボタン 44：0 = 7K+S 1 = 7K）
# 上が設定パネルの箱（ASSIST の紫の箱の題を PLAY MODE に描き替えたもの 142×59）、その下に値 7K+S・7K（パネルの 116×21 のコマ）、
# その下に下の欄の値（73×14 のコマ）
title_lib2 = library(title_spec + [((P, 37, 1684, 136, 20), "STYLE", (1, 63))], 120, 1, False)
title_lib2["M"] = gl.make_m(title_lib2["H"], title_lib2["V"])
abox = bytearray(raw(P, 336, 1383, 142, 59))
inside = bytes(abox[(5 * 142 + 5) * 4:(5 * 142 + 5) * 4 + 4])
atitle = compose("PLAY MODE", title_lib2, 138, 20, gap_title, inside)
for yy in range(20):
    abox[((yy + 2) * 142 + 2) * 4:((yy + 2) * 142 + 140) * 4] = atitle[yy * 138 * 4:(yy + 1) * 138 * 4]
mode_pan_lib = library(pan_spec + [((P, 534, 1542, 116, 21), "7KEYS"), ((P, 782, 1360, 116, 21), "SUD+HID")], 120, 1, False)
mode_pan = b"".join(compose(wd, mode_pan_lib, 116, 21, gap_pan, pan_bg) for wd in ("7K+S", "7K"))
mode_bar_lib = library(bar_spec + [((G, 300, 2292, 90, 14), "HID+SUD")], 120, 2, True)
seven = glyphs(G, 910 + 13 * 7, 1985, 13, 15, 120, 2, True)
assert len(seven) == 1
mode_bar_lib["7"] = fit_height(seven[0], mode_bar_lib["H"].cy1 - mode_bar_lib["H"].cy0 + 1)
mb = [mode_bar_lib[c] for c in "HARD"]
gap_bar = int(statistics.median([mb[i + 1].cx0 - mb[i].cx1 - 1 for i in range(3)]))
mode_bar = b"".join(compose(wd, mode_bar_lib, 73, 14, gap_bar, raw(G, 220, 2250, 73, 14)[0:4]) for wd in ("7K+S", "7K"))
AW, AH = 142, 61 + 42 + 2 + 28
aimg = bytearray(b'\0\0\0\0' * (AW * AH))
def aput(src, sw, sh, oy):
    for yy in range(sh):
        aimg[((oy + yy) * AW) * 4:((oy + yy) * AW + sw) * 4] = src[yy * sw * 4:(yy + 1) * sw * 4]
aput(abox, 142, 59, 0)
aput(mode_pan, 116, 42, 61)
aput(mode_bar, 73, 28, 105)
with tempfile.TemporaryDirectory() as tmp:
    raw_path = os.path.join(tmp, "mode.raw")
    open(raw_path, "wb").write(aimg)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{AW}x{AH}", "-i", raw_path,
                    os.path.join(SYSTEM, "bmjoy_mode.png")], check=True)
print("作った:", os.path.join(SYSTEM, "bmjoy_mode.png"))

# --- select.csv を書き換える（Shift_JIS のまま、行の区切りも変えない）
csv = os.path.join(SELECT, "select.csv")
backup = csv + ".bak-bmjoy"
if not os.path.exists(backup):
    shutil.copyfile(csv, backup)
data = open(csv, "rb").read()
nl = b"\r\n" if b"\r\n" in data else b"\n"
lines = data.split(nl)
IMAGE_LINE = b"#IMAGE,LR2files\\Theme\\WMIX_HD\\select\\system\\bmjoy_gauge.png,,,,,,,,,,,,,"
gr = gl.add_image_line(lines, IMAGE_LINE)
changed = 0
for i, l in enumerate(lines):
    f = l.split(b",")
    if not l.startswith(b"#SRC_BUTTON,") or len(f) < 12 or f[11] != b"40":
        continue
    key = (f[2], f[3], f[4])
    if key in ((b"0", b"136", b"2250"), (str(gr).encode(), b"0", b"0")):        # 下の欄
        f[2:9] = [str(gr).encode(), b"0", b"0", b"68", str(BAR_H).encode(), b"1", str(n).encode()]
    elif key in ((b"1", b"658", b"1297"), (str(gr).encode(), b"0", str(PAN_Y).encode())):  # 設定パネル
        f[2:9] = [str(gr).encode(), b"0", str(PAN_Y).encode(), b"116", str(PAN_H).encode(), b"1", str(n).encode()]
    else:
        continue
    lines[i] = b",".join(f)
    changed += 1
# 判定の欄：#IMAGE を足し、SP のパネルの左上（DP では RIGHT LANE の箱がある場所）に箱・値のボタン・マウスの光を置く。
# 前に足した分（目印の行のあいだ）は消してから足し直す
JIMAGE_LINE = b"#IMAGE,LR2files\\Theme\\WMIX_HD\\select\\system\\bmjoy_judge.png,,,,,,,,,,,,,"
jgr = str(gl.add_image_line(lines, JIMAGE_LINE)).encode()
BEGIN, END = b"//BMjoy judge begin", b"//BMjoy judge end"
if BEGIN in lines:
    a, b = lines.index(BEGIN), lines.index(END)
    del lines[a:b + 1]
block = [
    BEGIN,
    b"//\x94\xbb\x92\xe8\x82\xcc\x97\x93\x81i\x83J\x83X\x83^\x83\x80\x94\xbb\x92\xe8\x81j button900 0:OFF 1-4:VERY HARD-EASY 5-13:JUDGE1-9",
    b"#SRC_IMAGE,0," + jgr + b",0,0,142,59,1,1,0,21,0,0,0,,,,,,,",
    b"#DST_IMAGE,0,0,-480,116,142,59,2,255,255,255,255,1,0,0,0,166,21,-12,0,0",
    b"#DST_IMAGE,0,166,410,116,142,59,2,255,255,255,255,1,0,0,0,,,,,",
    b"#SRC_BUTTON,0," + jgr + b",0,61,116," + str(21 * JN).encode() + b",1," + str(JN).encode() + b",0,21,900,1,1,,,,,,,",
    b"#DST_BUTTON,0,0,-467,145,116,21,2,255,255,255,255,1,0,0,0,166,21,-12,0,0",
    b"#DST_BUTTON,0,166,423,145,116,21,2,255,255,255,255,1,0,0,0,,,,,",
    b"#SRC_ONMOUSE,0,1,821,889,144,49,1,1,0,0,1,0,0,144,49,,,,,",
    b"#DST_ONMOUSE,0,0,409,130,144,49,2,0,255,255,255,2,0,0,0,1000,21,-12,0,0",
    b"#DST_ONMOUSE,0,600,409,130,144,49,0,0,255,255,255,2,0,0,0,,,,,",
    b"#DST_ONMOUSE,0,1000,409,130,144,49,0,125,255,255,255,2,0,0,0,,,,,",
    b"#DST_ONMOUSE,0,1500,409,130,144,49,0,255,255,255,255,2,0,0,0,,,,,",
    b"#DST_ONMOUSE,0,2000,409,130,144,49,0,125,255,255,255,2,0,0,0,,,,,",
    END,
]
# メトロノームの欄（BMjoy のボタン 902）。SP のパネルの BATTLE の下
MIMAGE_LINE = b"#IMAGE,LR2files\\Theme\\WMIX_HD\\select\\system\\bmjoy_metronome.png,,,,,,,,,,,,,"
mgr = str(gl.add_image_line(lines, MIMAGE_LINE)).encode()
MBEGIN, MEND = b"//BMjoy metronome begin", b"//BMjoy metronome end"
if MBEGIN in lines:
    a, b = lines.index(MBEGIN), lines.index(MEND)
    del lines[a:b + 1]
mblock = [
    MBEGIN,
    b"//metronome button902 0:OFF 1:ON",
    b"#SRC_IMAGE,0," + mgr + b",0,0,142,59,1,1,0,21,0,0,0,,,,,,,",
    b"#DST_IMAGE,0,0,-334,543,142,59,2,255,255,255,255,1,0,0,0,166,21,-12,0,0",
    b"#DST_IMAGE,0,166,556,543,142,59,2,255,255,255,255,1,0,0,0,,,,,",
    b"#SRC_BUTTON,0," + mgr + b",0,61,116,42,1,2,0,21,902,1,1,,,,,,,",
    b"#DST_BUTTON,0,0,-321,572,116,21,2,255,255,255,255,1,0,0,0,166,21,-12,0,0",
    b"#DST_BUTTON,0,166,569,572,116,21,2,255,255,255,255,1,0,0,0,,,,,",
    b"#SRC_ONMOUSE,0,1,821,889,144,49,1,1,0,0,1,0,0,144,49,,,,,",
    b"#DST_ONMOUSE,0,0,555,557,144,49,2,0,255,255,255,2,0,0,0,1000,21,-12,0,0",
    b"#DST_ONMOUSE,0,600,555,557,144,49,0,0,255,255,255,2,0,0,0,,,,,",
    b"#DST_ONMOUSE,0,1000,555,557,144,49,0,125,255,255,255,2,0,0,0,,,,,",
    b"#DST_ONMOUSE,0,1500,555,557,144,49,0,255,255,255,255,2,0,0,0,,,,,",
    b"#DST_ONMOUSE,0,2000,555,557,144,49,0,125,255,255,255,2,0,0,0,,,,,",
    MEND,
]
# 7k モードの欄：ASSIST のボタン（下の欄 1 つ・パネルの 1P 側）をその絵のコマにし、パネルの ASSIST の箱の上に PLAY MODE の箱を重ねる（SP と DP）
AIMAGE_LINE = b"#IMAGE,LR2files\\Theme\\WMIX_HD\\select\\system\\bmjoy_mode.png,,,,,,,,,,,,,"
agr = str(gl.add_image_line(lines, AIMAGE_LINE)).encode()
for i, l in enumerate(lines):
    f = l.split(b",")
    if not l.startswith(b"#SRC_BUTTON,") or len(f) < 12 or f[11] != b"44":
        continue
    if (f[2], f[3], f[4]) in ((b"0", b"220", b"2250"), (agr, b"0", b"105")):    # 下の欄
        f[2:7] = [agr, b"0", b"105", b"73", b"28"]
    elif (f[2], f[3], f[4]) in ((b"1", b"534", b"1423"), (agr, b"0", b"61")):  # 設定パネル
        f[2:7] = [agr, b"0", b"61", b"116", b"42"]
    else:
        continue
    lines[i] = b",".join(f)
    changed += 1
ABEGIN, AEND = b"//BMjoy mode begin", b"//BMjoy mode end"
if ABEGIN in lines:
    a, b = lines.index(ABEGIN), lines.index(AEND)
    del lines[a:b + 1]
ablock = [
    ABEGIN,
    b"//PLAY MODE button44 0:7K+S 1:7K",
    b"#SRC_IMAGE,0," + agr + b",0,0,142,59,1,1,0,21,0,0,0,,,,,,,",
    b"#DST_IMAGE,0,0,-325,180,142,59,2,255,255,255,255,1,0,0,0,166,21,-12,0,0",
    b"#DST_IMAGE,0,166,704,180,142,59,2,255,255,255,255,1,0,0,0,,,,,",
    b"#SRC_IMAGE,0," + agr + b",0,0,142,59,1,1,0,21,0,0,0,,,,,,,",
    b"#DST_IMAGE,0,0,-325,180,142,59,2,255,255,255,255,1,0,0,0,166,21,10,-11,0",
    b"#DST_IMAGE,0,166,704,180,142,59,2,255,255,255,255,1,0,0,0,,,,,",
    AEND,
]
# パネルの SP の部品（出力の四角）のあとに置く（背景より手前に描くため）
anchor = next(i for i, l in enumerate(lines) if l.startswith(b"#DST_IMAGE,0,166,402,543,142,59,"))
lines[anchor + 1:anchor + 1] = block + mblock + ablock
open(csv, "wb").write(nl.join(lines))
print(f"select.csv：GAUGE・ASSIST のボタン {changed} 個を書き換え、判定・メトロノーム・PLAY MODE の欄を足した"
      f"（画像の番号 {gr}・{jgr.decode()}・{mgr.decode()}・{agr.decode()}。元は {backup}）")
