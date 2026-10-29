#!/usr/bin/env python3
# walkure-offline（https://github.com/naktazdim/walkure-offline、MIT）の同梱データを、MOD「リコメンド」の model.tsv にする。
#   python3 tools/mods/walkure_model.py <walkure-offline のフォルダ> mods/walkure/model.tsv
# 1 行目は★の換算表（発狂難易度表★1〜25 に当たる難度。25 個）。2 行目からは譜面ごとに
#   md5  地力譜面度  難度（EASY NORMAL HARD FULLCOMBO）  ★の難度（同じ順。達成者がいなければ -）
# コース（段位）は BMjoy のコースの鍵と合わないので入れない
import json
import sys
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
data = src / "assets" / "data"
entries = json.loads((data / "recommendation-model-entries.json").read_text(encoding="utf-8"))
mapping = json.loads((data / "recommendation-star-rating-mapping.json").read_text(encoding="utf-8"))
assert len(mapping) == 25
lamps = ["easy", "normal", "hard", "fullCombo"]
lines = ["\t".join(repr(float(v)) for v in mapping)]
for e in entries:
    if e["entryType"] != "chart":
        continue
    cols = [e["md5"].lower(), repr(float(e["chartDiscrimination"]))]
    cols += [repr(float(e["clearDifficulty"][l])) for l in lamps]
    cols += ["-" if e["clearDifficultyStarRatings"][l] is None else repr(float(e["clearDifficultyStarRatings"][l])) for l in lamps]
    lines.append("\t".join(cols))
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"{len(lines) - 1} 譜面")
