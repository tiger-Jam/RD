#!/usr/bin/env python3
# W-MIX（WMIX_HD）のリザルト画面に、BMjoy のゲージ（GAS・EX-HARD・ASSIST・CUSTOM1〜9）の名前のコマを足し、
# 「今回のクリア」の絵を BMjoy のボタン 901（ランプの色に染めて出す）にする。
#
#   python3 tools/skin/wmix_result_frames.py <スキンの result のフォルダ>
#
# フォルダは LR2files/Theme/WMIX_HD/result（result_L.csv などと system/main.png がある所）。
# - system/bmjoy_result.png を作る（ゲージの欄の 18 コマ：0〜5 は元の 6 つ、6=GAS 7=EX-HARD 8=ASSIST 9〜17=CUSTOM1〜9）。
#   足したコマは白い字。BMjoy が遊んだゲージの色に染めて出す
# - result_*.csv のゲージの欄（ボタン 40）をその絵の 18 コマにし、「今回のクリア」（CLEAR.png のボタン 40）をボタン 901 にする。
#   元の CSV は <名前>.csv.bak-bmjoy に残す（もう書き換えてあれば、絵だけ作り直して行を直す）
# 字はゲージの名前（GROOVE … NONSTOP）から切り出す。この字に無い I・X・M は H・K・V から作り、数字はリザルトの数字の絵に縁を付けて使う
import os
import shutil
import statistics
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    sys.exit("使い方: wmix_result_frames.py <スキンの result のフォルダ>")
RESULT = sys.argv[1]
SYSTEM = os.path.join(RESULT, "system")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glyphs as gl  # noqa: E402
from glyphs import add_outline, compose, fit_height, library, raw  # noqa: E402
gl.DIR = SYSTEM

M = "main.png"
X0, W, FH = 805, 121, 18
spec = [((M, X0, 7 + FH * i, W, FH), name) for i, name in enumerate(["GROOVE", "HARD", "HAZARD", "EASY", "P-ATTACK", "G-ATTACK"])]
spec += [((M, X0, 131, W, FH), "COURSE"), ((M, X0, 149, W, FH), "GRADE"), ((M, X0, 167, W, FH), "NONSTOP")]
lib = library(spec, 120, 1, True)


H = lib["H"]
lib["I"] = gl.make_i(H)
lib["X"] = gl.make_x(lib["K"])
lib["M"] = gl.make_m(H, lib["V"])
# 数字：リザルトの数字の絵（縁取り無し）に、ゲージの名前の字と同じ縁を付け、字の高さにそろえる
outline = next(c for q, c in H.px.items() if q not in H.core and c[3] > 200 and max(c[:3]) < 90)
digits = library([((M, 955, 56, 185, 16), "0123456789")], 120, 1, True)
for d in "0123456789":
    lib[d] = fit_height(add_outline(digits[d], outline), H.cy1 - H.cy0 + 1)  # 下端は数字の下端（字の下端に合わせて置かれる）
gap = int(statistics.median([lib["A"].cx0 - H.cx1 - 1, lib["R"].cx0 - lib["A"].cx1 - 1, lib["D"].cx0 - lib["R"].cx1 - 1]))

WORDS = ["GAS", "EX-HARD", "ASSIST"] + [f"CUSTOM{i}" for i in range(1, 10)]
strip = bytearray(raw(M, X0, 7, W, FH * 6))
bg = bytes(4)
for wd in WORDS:
    strip += compose(wd, lib, W, FH, gap, bg, "right")
n = 6 + len(WORDS)
with tempfile.TemporaryDirectory() as tmp:
    raw_path = os.path.join(tmp, "result.raw")
    open(raw_path, "wb").write(strip)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{FH * n}", "-i", raw_path,
                    os.path.join(SYSTEM, "bmjoy_result.png")], check=True)
print("作った:", os.path.join(SYSTEM, "bmjoy_result.png"))

# --- result_*.csv を書き換える（Shift_JIS のまま、行の区切りも変えない）
IMAGE_LINE = b"#IMAGE,LR2files\\Theme\\WMIX_HD\\result\\system\\bmjoy_result.png,,,,,,,,,,"
for name in sorted(os.listdir(RESULT)):
    if not (name.startswith("result") and name.endswith(".csv")):
        continue
    csv = os.path.join(RESULT, name)
    if not os.path.exists(csv + ".bak-bmjoy"):
        shutil.copyfile(csv, csv + ".bak-bmjoy")
    data = open(csv, "rb").read()
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    lines = data.split(nl)
    # 前に #IF の中へ足してしまった行は消してから足し直す（#IMAGE の番号がずれていた）
    while IMAGE_LINE in lines and any(l.upper().startswith(b"#ELSEIF") for l in lines[max(0, lines.index(IMAGE_LINE) - 4):lines.index(IMAGE_LINE)]):
        del lines[lines.index(IMAGE_LINE)]
    gr = str(gl.add_image_line(lines, IMAGE_LINE)).encode()
    gauge = label = 0
    for i, l in enumerate(lines):
        f = l.split(b",")
        if not l.startswith(b"#SRC_BUTTON,") or len(f) < 12:
            continue
        if f[11] == b"40" and ((f[2], f[3], f[4]) == (b"0", b"805", b"7") or (f[3], f[4], f[5], f[6]) == (b"0", b"0", str(W).encode(), str(FH * n).encode())):
            f[2:9] = [gr, b"0", b"0", str(W).encode(), str(FH * n).encode(), b"1", str(n).encode()]
            gauge += 1
        elif f[11] in (b"40", b"901") and f[2] == b"9" and f[5] == b"328":  # 今回のクリア（CLEAR.png）
            f[11] = b"901"
            label += 1
        else:
            continue
        lines[i] = b",".join(f)
    open(csv, "wb").write(nl.join(lines))
    print(f"{name}：ゲージの欄 {gauge} 個・今回のクリア {label} 個を書き換えた（画像の番号 {gr.decode()}）")
