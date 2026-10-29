#!/usr/bin/env python3
# スキンの差分（skinpatch/<元の SHA-256>.patch）を作る。元のファイルと、RD の表示を足したファイルを比べる。
#
#   python3 tools/skin/make_skin_patch.py <RDbody> <元のファイル> <足したファイル> <絵の置き場の名前>
#
# 足したファイルの #IMAGE が指す rd_*.png は skinpatch/<絵の置き場の名前>/ に写し、「rdpatch:<名前>/rd_….png」に書き換える。
# 行の分け方と文字の直し方は RD と同じにするため、RDbody --skin-lines に分けさせる
import difflib
import os
import re
import shutil
import subprocess
import sys

if len(sys.argv) != 5:
    sys.exit("使い方: make_skin_patch.py <RDbody> <元のファイル> <足したファイル> <絵の置き場の名前>")
body, orig, mod, name = sys.argv[1:5]
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "skinpatch")


def lines(path):
    out = subprocess.run([body, "--skin-lines", path], capture_output=True, check=True).stdout.decode("utf-8")
    rows = out.split("\n")
    return rows[0], rows[1:-1]  # 1 行目がハッシュ、最後は空


sha, a = lines(orig)
_, b = lines(mod)
# 足した絵の場所を差分の置き場に替える
pic = re.compile(r"^(#IMAGE,)([^,]*\\)(rd_[^\\,]+\.png)(,.*)$", re.I)
images = []
for i, line in enumerate(b):
    m = pic.match(line)
    if m:
        images.append(m.group(3))
        b[i] = f"{m.group(1)}rdpatch:{name}/{m.group(3)}{m.group(4)}"
out = [f"# {os.path.basename(orig)} に RD の表示を足す差分（tools/skin/make_skin_patch.py で作る）"]
for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=a, b=b, autojunk=False).get_opcodes():
    if tag == "equal":
        continue
    out.append(f"@ {i1} {i2}")
    out += [f"- {x}" for x in a[i1:i2]]
    out += [f"+ {x}" for x in b[j1:j2]]
os.makedirs(os.path.join(ROOT, name), exist_ok=True)
with open(os.path.join(ROOT, sha + ".patch"), "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(out) + "\n")
for img in images:
    shutil.copyfile(os.path.join(os.path.dirname(mod), "system", img), os.path.join(ROOT, name, img))
print(f"{sha}.patch：{sum(1 for x in out if x.startswith('@'))} か所、絵 {len(images)}")
