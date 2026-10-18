#!/usr/bin/env python3
"""曲フォルダの読み込みの速さを測るための、作り物の曲をたくさん作る（キー音は無い）。

使い方: python3 tools/make_many_songs.py <出力フォルダ> [フォルダ数]
"""
import os
import random
import sys


def chart(rnd, title, level, notes_per_measure, measures):
    lines = ["#PLAYER 1", f"#TITLE {title}", "#ARTIST generated", "#GENRE TEST", f"#BPM {rnd.choice([130, 150, 174, 200])}",
             f"#PLAYLEVEL {level}", "#RANK 2"]
    for i in range(1, 37):
        lines.append(f"#WAV{i:02d} key{i}.ogg")
    chans = ["11", "12", "13", "14", "15", "18", "19", "16"]
    for m in range(1, measures + 1):
        lines.append(f"#{m:03d}01:01000100010001000100010001000100")
        for ch in chans:
            data = ["00"] * 16
            for _ in range(notes_per_measure // 8):
                data[rnd.randrange(16)] = f"{rnd.randrange(1, 36):02d}"
            lines.append(f"#{m:03d}{ch}:" + "".join(data))
    return "\r\n".join(lines) + "\r\n"


def main():
    out = sys.argv[1]
    folders = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    rnd = random.Random(42)
    for f in range(folders):
        d = os.path.join(out, f"song{f:05d}")
        os.makedirs(d, exist_ok=True)
        for k, (suffix, npm) in enumerate([("n", 8), ("h", 16), ("a", 32)]):
            with open(os.path.join(d, f"chart_{suffix}.bme"), "w", encoding="shift_jis") as fp:
                fp.write(chart(rnd, f"Song {f} [{suffix.upper()}]", 3 + k * 4, npm, 64))
    print(f"{folders * 3} 譜面")


if __name__ == "__main__":
    main()
