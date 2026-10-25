# W-MIX のスキンの絵から字を 1 字ずつ切り出し、並べて新しいコマを作る部品（wmix_*_frames.py が使う）。
# 字の芯（明るい画素）がつながった塊を 1 字とし、周りの縁も含める。要るもの：ffmpeg
import os
import statistics
import subprocess

DIR = "."  # 絵のあるフォルダ（使う側が決める）

_cache = {}
def raw(path, x, y, w, h):
    k = (path, x, y, w, h)
    if k not in _cache:
        _cache[k] = bytes(subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', os.path.join(DIR, path), '-vf', f'crop={w}:{h}:{x}:{y}',
                                          '-f', 'rawvideo', '-pix_fmt', 'rgba', '-'], capture_output=True, check=True).stdout)
    return _cache[k]

class Glyph:
    def __init__(self, px, core, x0, x1, y0, y1):
        self.px, self.core = px, core  # {(x,y): rgba}, 芯の範囲
        self.cx0, self.cx1, self.cy0, self.cy1 = x0, x1, y0, y1

def glyphs(path, x, y, w, h, th, reach, white, split=None):
    b = raw(path, x, y, w, h)
    def p(xx, yy): i = (yy * w + xx) * 4; return b[i:i + 4]
    core = {(xx, yy) for yy in range(h) for xx in range(w) if p(xx, yy)[3] > 100 and max(p(xx, yy)[:3]) > th}
    comps, seen = [], set()
    for s in sorted(core):
        if s in seen: continue
        st, comp = [s], set()
        seen.add(s)
        while st:
            c = st.pop(); comp.add(c)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    n = (c[0] + dx, c[1] + dy)
                    if n in core and n not in seen: seen.add(n); st.append(n)
        comps.append(comp)
    # 横に重なる塊（芯が途切れた字）は 1 字にまとめる
    comps.sort(key=lambda c: min(q[0] for q in c))
    merged = []
    for c in comps:
        if merged:
            m = merged[-1]
            a0, a1 = min(q[0] for q in m), max(q[0] for q in m)
            b0, b1 = min(q[0] for q in c), max(q[0] for q in c)
            if min(a1, b1) - max(a0, b0) >= min(a1 - a0, b1 - b0) * 0.6: m |= c; continue
        merged.append(c)
    # 芯がつながってしまっている 2 字は、指定の列で分ける（split = (塊の番号, 右の字が始まる列)）
    if split and split[0] < len(merged):
        c = merged[split[0]]
        merged[split[0]:split[0] + 1] = [{q for q in c if q[0] < split[1]}, {q for q in c if q[0] >= split[1]}]
    # 芯でない画素は、reach 以内でいちばん近い字のもの
    owner = {}
    for gi, c in enumerate(merged):
        for q in c: owner[q] = (0, gi)
    out = []
    for gi, c in enumerate(merged):
        out.append({})
    for yy in range(h):
        for xx in range(w):
            if p(xx, yy)[3] == 0 and max(p(xx, yy)[:3]) == 0: continue
            best = None
            for gi, c in enumerate(merged):
                d = min(max(abs(q[0] - xx), abs(q[1] - yy)) for q in c)
                if d <= reach and (best is None or d < best[0]): best = (d, gi)
            if best is None: continue
            out[best[1]][(xx, yy)] = bytes(p(xx, yy))
    res = []
    for gi, c in enumerate(merged):
        px = out[gi]
        if white:
            peaks = sorted(max(px[q][:3]) for q in c if q in px)
            peak = max(1, peaks[len(peaks) * 9 // 10])
            for q, v in px.items():
                m = min(255, round(max(v[:3]) * 255 / peak))
                px[q] = bytes([m, m, m, v[3]])
        res.append(Glyph(px, c, min(q[0] for q in c), max(q[0] for q in c), min(q[1] for q in c), max(q[1] for q in c)))
    # 字の下端の基準は、同じ絵の中の字の下端（「-」のような低くない字もそのままの高さに置く）
    base = max(g.cy1 for g in res)
    for g in res: g.base = base
    return res

def fit_height(g, h):
    # 字の芯の高さを h にする（数字の絵は字より 1 段高い）。縁の行は上下にずらすだけ
    old = g.cy1 - g.cy0 + 1
    if old == h: return g
    px = {}
    for (xx, yy), v in g.px.items():
        if yy < g.cy0: px[(xx, yy)] = v
        elif yy > g.cy1: px[(xx, yy + h - old)] = v
    for ty in range(h):
        sy = g.cy0 + min(old - 1, int((ty + 0.5) * old / h))
        for (xx, yy), v in g.px.items():
            if yy == sy: px[(xx, g.cy0 + ty)] = v
    core = {(q[0], q[1]) for q in g.core}
    n = Glyph(px, core, g.cx0, g.cx1, g.cy0, g.cy0 + h - 1)
    n.base = g.cy0 + h - 1
    return n

def compose(word, lib, w, fh, gap, bg, align="center"):
    # align が "right" なら右に寄せる（右端から 2 画素あける）
    # 空白は字ではなく、H の半分の幅だけ空ける
    space = (lib['H'].cx1 - lib['H'].cx0 + 1) // 2
    gs = [None if ch == ' ' else lib[ch] for ch in word]
    widths = [space if g is None else g.cx1 - g.cx0 + 1 for g in gs]
    g_ = gap
    while sum(widths) + g_ * (len(gs) - 1) > w - 4 and g_ > gap - 1: g_ -= 1
    total = sum(widths) + g_ * (len(gs) - 1)
    # 本の字の下端（ベースライン）に合わせる：基準は HARD の H
    base = lib['H'].base
    canvas = {}
    cx = 0
    for g in gs:
        if g is None:
            cx += space + g_
            continue
        dy = base - g.base
        for (xx, yy), v in g.px.items():
            t = (cx + xx - g.cx0, yy + dy)
            old = canvas.get(t)
            if old is None or (v[3], max(v[:3])) > (old[3], max(old[:3])): canvas[t] = v
        cx += g.cx1 - g.cx0 + 1 + g_
    # 幅に入らなければ横に縮める
    xs = [t[0] for t in canvas]
    lo, hi = min(xs), max(xs) + 1
    span = hi - lo
    scale = min(1.0, (w - 2) / span)
    frame = bytearray(bg * (w * fh))
    off = (w - 2 - span * scale) if align == "right" else (w - span * scale) / 2
    for tx in range(w):
        for ty in range(fh):
            # 縮めるときは、元の範囲の画素を重みつきで混ぜる
            a = (tx - off) / scale + lo
            bnd = (a, a + 1 / scale)
            acc = [0.0] * 4; tw = 0.0
            sx = int(bnd[0]) - 1
            while sx < bnd[1] + 1:
                ov = min(bnd[1], sx + 1) - max(bnd[0], sx)
                if ov > 0:
                    v = canvas.get((sx, ty), bg)
                    for k in range(4): acc[k] += v[k] * v[3] * ov if k < 3 else v[3] * ov
                    tw += ov
                sx += 1
            if tw == 0: continue
            al = acc[3] / tw
            if al <= 0: continue
            px = bytes([min(255, round(acc[k] / acc[3])) for k in range(3)] + [min(255, round(al))])
            i = (ty * w + tx) * 4
            frame[i:i + 4] = px
    return bytes(frame)

def library(spec, th, reach, white):
    lib = {}
    for src, names, *split in spec:
        # 字がくっついて数が合わないときは、芯とみなす明るさを上げ下げして数が合うところを使う
        for t in (th, th + 40, th - 20, th + 20, th + 80):
            gs = glyphs(*src, t, reach, white, split[0] if split else None)
            if len(gs) == len(names): break
        assert len(gs) == len(names), (src, names, len(gs))
        for g, ch in zip(gs, names):
            if ch != '_': lib.setdefault(ch, g)
    return lib



def add_outline(g, color):
    # 縁取りの無い字（数字の絵など）に、1 画素の縁を付ける。縁の色は color（RGBA の bytes）
    px = dict(g.px)
    for (x, y) in list(g.core):
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                q = (x + dx, y + dy)
                if q not in g.core and (q not in px or px[q][3] < color[3] or max(px[q][:3]) < 40):
                    px[q] = color
    n = Glyph(px, g.core, g.cx0, g.cx1, g.cy0, g.cy1)
    n.base = g.base
    return n


def add_image_line(lines, image_line):
    """CSV の行（bytes の list）に #IMAGE の行を足し、その絵の番号を返す。もう足してあればその番号。
    #IMAGE が #IF … #ENDIF の中にあるときは、分岐ごとに同じ数の #IMAGE があるものとして 1 つの分岐だけを数え、
    足す行は #ENDIF のあと（どの分岐でも同じ番号になる所）に置く"""
    def scan(stop=None):
        count, depth, branch_counted, last_end = 0, 0, True, None
        for i, l in enumerate(lines):
            if stop is not None and i == stop:
                return count, None
            u = l.upper()
            if u.startswith(b"#IF,") or u.startswith(b"#IF "):
                depth += 1
                branch_counted = depth == 1
            elif u.startswith(b"#ELSEIF") or u.startswith(b"#ELSE"):
                if depth == 1: branch_counted = False
            elif u.startswith(b"#ENDIF"):
                if depth == 1:
                    branch_counted = True
                    if last_end == "pending": last_end = i
                depth = max(0, depth - 1)
            elif l.startswith(b"#IMAGE,"):
                if depth == 0:
                    count += 1
                    last_end = i
                else:
                    if branch_counted: count += 1
                    last_end = "pending"
        return count, last_end
    if image_line in lines:
        idx, _ = scan(stop=lines.index(image_line))
        return idx
    count, at = scan()
    lines.insert(at + 1, image_line)
    return count


# --- スキンに無い字を、ある字から作る

def rebuilt(px, core, base):
    g = Glyph(px, core, min(q[0] for q in core), max(q[0] for q in core), min(q[1] for q in core), max(q[1] for q in core))
    g.base = base
    return g


def left_stem(h):
    # 左の縦棒の右端（上から 2 段目の芯の続き）
    y = h.cy0 + 1
    x = h.cx0
    while (x + 1, y) in h.core: x += 1
    return x


def make_i(h):
    # I：H の左の縦棒
    sr = left_stem(h)
    return rebuilt({q: v for q, v in h.px.items() if q[0] <= sr + 1}, {q for q in h.core if q[0] <= sr}, h.base)


def make_x(k):
    # X：K の斜めの部分（「<」）と、それを左右に返した「>」を、頂点で合わせる
    ks = left_stem(k)
    arm_px = {q: v for q, v in k.px.items() if q[0] > ks + 1}
    arm_core = {q for q in k.core if q[0] > ks + 1}
    v = min(q[0] for q in arm_core)
    xp, xc = dict(arm_px), set(arm_core)
    for (x, y), c in arm_px.items():
        m = (2 * v - x, y)
        if m not in xp or max(c[:3]) > max(xp[m][:3]): xp[m] = c
    for (x, y) in arm_core: xc.add((2 * v - x, y))
    return rebuilt(xp, xc, k.base)


def make_m(h, v):
    # M：H の左の縦棒・V・左右に返した縦棒を並べる
    sr = left_stem(h)
    sw = sr - h.cx0 + 1
    stem_px = {q: c for q, c in h.px.items() if q[0] <= sr + 1}
    stem_core = {q for q in h.core if q[0] <= sr}
    mp, mc = {}, set()
    def put(px, core, dx):
        for (x, y), c in px.items():
            q = (x + dx, y)
            if q not in mp or max(c[:3]) > max(mp[q][:3]): mp[q] = c
        for (x, y) in core: mc.add((x + dx, y))
    put(stem_px, stem_core, -h.cx0)
    put(v.px, v.core, sw - v.cx0)
    # 右の縦棒は左の縦棒を左右に返したもの（縦棒の中の位置 lx を sw − 1 − lx に）
    right = {(sw - 1 - (x - h.cx0), y): c for (x, y), c in stem_px.items()}
    right_core = {(sw - 1 - (x - h.cx0), y) for (x, y) in stem_core}
    put(right, right_core, sw + (v.cx1 - v.cx0 + 1))
    return rebuilt(mp, mc, v.base)
