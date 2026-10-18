#!/usr/bin/env python3
"""動作確認用のテスト曲を作る。キー音は合成した wav（一部は ogg 名で参照して代替を試す）。

使い方: python3 tools/make_test_song.py <出力フォルダ>
"""
import math
import os
import struct
import sys
import wave

RATE = 44100


def write_wav(path, samples, channels=1, rate=RATE):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in samples))


def tone(freq, length, decay=8.0, vol=0.5):
    n = int(RATE * length)
    return [vol * math.sin(2 * math.pi * freq * i / RATE) * math.exp(-decay * i / RATE) for i in range(n)]


def kick():
    n = int(RATE * 0.25)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        f = 50 + 120 * math.exp(-30 * t)
        phase += 2 * math.pi * f / RATE
        out.append(0.8 * math.sin(phase) * math.exp(-10 * t))
    return out


def hat():
    import random
    rnd = random.Random(1)
    n = int(RATE * 0.06)
    return [0.25 * (rnd.random() * 2 - 1) * math.exp(-60 * i / RATE) for i in range(n)]


def write_bmp(path, w, h, pixel):
    """24bit BMP。pixel(x, y) は (r, g, b)"""
    row = (w * 3 + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for x in range(w):
            r, g, b = pixel(x, y)
            line += bytes((b, g, r))
        line += b"\0" * (row - w * 3)
        data += line
    header = b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    open(path, "wb").write(header + info + data)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "testdata/generated/test_song"
    os.makedirs(out, exist_ok=True)
    # 01: キック, 02: ハット, 03..09: 鍵盤（ドレミ…）, 0A: 皿, 0B: LN用の長い音
    write_wav(os.path.join(out, "kick.wav"), kick())
    write_wav(os.path.join(out, "hat.wav"), hat())
    scale = [261.6, 293.7, 329.6, 349.2, 392.0, 440.0, 493.9]
    for i, f in enumerate(scale):
        write_wav(os.path.join(out, f"key{i + 1}.wav"), tone(f, 0.4))
    write_wav(os.path.join(out, "scratch.wav"), tone(880, 0.15, decay=20))
    write_wav(os.path.join(out, "long.wav"), tone(220, 1.5, decay=1.5))

    # BGA：01..04 は色の違う縞模様、05 は黒を抜いた丸（重ねる層）、00 はミスしたときの画像
    colors = [(200, 60, 60), (60, 200, 60), (60, 60, 200), (200, 200, 60)]
    for i, c in enumerate(colors):
        write_bmp(os.path.join(out, f"bga{i + 1}.bmp"), 256, 256,
                  lambda x, y, c=c: c if (x // 32 + y // 32) % 2 == 0 else tuple(v // 3 for v in c))
    write_bmp(os.path.join(out, "layer.bmp"), 256, 256,
              lambda x, y: (255, 255, 255) if (x - 128) ** 2 + (y - 128) ** 2 < 40 ** 2 else (0, 0, 0))
    write_bmp(os.path.join(out, "miss.bmp"), 256, 256, lambda x, y: (120, 0, 120))

    lines = [
        "#PLAYER 1",
        "#GENRE TEST",
        "#TITLE BMjoy テスト曲",
        "#ARTIST BMjoy",
        "#BPM 150",
        "#PLAYLEVEL 1",
        "#RANK 2",
        "#TOTAL 300",
        "#WAV01 kick.wav",
        "#WAV02 HAT.ogg",  # 大文字小文字の違い＋拡張子の代替（実体は hat.wav）
    ]
    for i in range(7):
        lines.append(f"#WAV{i + 3:02d} key{i + 1}.wav")
    lines += ["#WAV0A scratch.wav", "#WAV0B long.wav", "#BPM01 200", "#STOP01 96"]
    lines += [f"#BMP{i + 1:02d} bga{i + 1}.bmp" for i in range(4)] + ["#BMP05 layer.bmp", "#BMP00 miss.bmp", ""]

    key_ch = ["11", "12", "13", "14", "15", "18", "19"]
    ln_ch = ["51", "52", "53", "54", "55", "58", "59"]
    measures = 16
    for m in range(1, measures + 1):
        lines.append(f"#{m:03d}01:01000100010001000100010001000100" if m < 4 else f"#{m:03d}01:0100010001000100")
        lines.append(f"#{m:03d}01:0002000200020002")
        lines.append(f"#{m:03d}04:{(m % 4) + 1:02d}")
        if m == 3:
            lines.append(f"#{m:03d}07:05")
        if m == 6:
            lines.append(f"#{m:03d}08:0001")  # 2拍目から BPM 200
        if m == 10:
            lines.append(f"#{m:03d}03:96")  # BPM 150 に戻す（16進）
        if m == 8:
            lines.append(f"#{m:03d}09:01")  # 小節頭で STOP（1拍）
        if m == 12:
            lines.append(f"#{m:03d}02:0.75")  # 3/4拍子
        if m >= 2 and m not in (4, 13):
            # 階段
            for k in range(7):
                data = ["00"] * 8
                data[k] = f"{k + 3:02d}"
                lines.append(f"#{m:03d}{key_ch[k]}:" + "".join(data))
            lines.append(f"#{m:03d}16:0A0000000A000000")
        if m in (4, 13):
            lines.append(f"#{m:03d}{ln_ch[3]}:0B000000000000000B000000")
    open(os.path.join(out, "test.bme"), "w", encoding="shift_jis").write("\r\n".join(lines) + "\r\n")
    print(os.path.join(out, "test.bme"))


if __name__ == "__main__":
    main()
