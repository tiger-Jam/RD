# 曲データベース（songs.db）から、同じ曲のフォルダの重複を整理する計画（plan.json）を作る。Windows の dedup.ps1 が読む。
#   python3 -I dedup-plan.py songs.db sizes.tsv plan.json
# 同じ譜面（md5）を持つフォルダどうしを 1 組にし、譜面がいちばん多いフォルダを残す（同じなら torrent → Music の A〜Z → パックの順）。
# 残す方の親・子、中に別の曲のフォルダがあるものは動かさない。どれも全部はそろわない組は merge にする（dedup.ps1 が足りないファイルを写す）
# sizes.tsv（フォルダ、バイト数、ファイル数のタブ区切り）は同じ数のときの順を決めるだけ。無ければ空のファイルでよい
import bisect, collections, json, sqlite3, sys

db_path, sizes_path, out_path = sys.argv[1:4]
roots = ['E:\\BMSFolder\\Music', 'E:\\BMSFolder\\Custom', 'E:\\BMSFolder\\torrent', 'E:\\BMSFolder\\Download']

size = {}
for l in open(sizes_path, encoding='utf-8-sig'):
    f, n, _ = l.rstrip('\n').split('\t')
    size[f] = int(n)
db = sqlite3.connect(db_path)
fm = collections.defaultdict(set)
mf = collections.defaultdict(set)
fp = collections.defaultdict(list)
for p, f, m in db.execute('select path, folder, md5 from chart'):
    fm[f].add(m)
    mf[m].add(f)
    fp[f].append((p, m))
allfolders = sorted(fm)

par = {f: f for f in fm}
def find(x):
    while par[x] != x:
        par[x] = par[par[x]]
        x = par[x]
    return x
for fs in mf.values():
    fs = list(fs)
    for g in fs[1:]:
        par[find(g)] = find(fs[0])
comp = collections.defaultdict(list)
for f in fm:
    comp[find(f)].append(f)

def kind(f):
    r = f.replace('E:\\BMSFolder\\', '').split('\\')
    if r[0] == 'torrent': return 0
    return 1 if r[0] == 'Music' and not r[1].startswith('+') else 2

def under(f):  # f の下にある、譜面のあるフォルダ
    i = bisect.bisect_right(allfolders, f + '\\')
    out = []
    while i < len(allfolders) and allfolders[i].startswith(f + '\\'):
        out.append(allfolders[i])
        i += 1
    return out

plan = []
for g in comp.values():
    if len(g) < 2: continue
    u = set().union(*[fm[f] for f in g])
    full = [f for f in g if fm[f] == u]
    keep = min(full or g, key=lambda f: (-len(fm[f]), kind(f), -size.get(f, 0)))
    gs = set(g)
    others = []
    for f in g:
        if f == keep or keep.startswith(f + '\\') or f.startswith(keep + '\\'): continue
        if f in roots or not any(f.startswith(r + '\\') for r in roots): continue
        if any(x not in gs for x in under(f)): continue
        if any(o.startswith(f + '\\') or f.startswith(o + '\\') for o in others): continue
        others.append(f)
    if not others: continue
    # ばらけている組で、残す方に同じ md5 がある譜面のファイル（写さない）
    skip = [p for f in others for p, m in fp[f] if m in fm[keep]] if not full else []
    plan.append({'keep': keep, 'others': others, 'merge': not full, 'skipcharts': skip})

json.dump(plan, open(out_path, 'w', encoding='utf-8'), ensure_ascii=False)
print(f'{len(plan)} 組／移す {sum(len(p["others"]) for p in plan)} フォルダ')
