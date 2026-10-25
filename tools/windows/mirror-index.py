# 落としておいた差分のミラー（zip）の中の譜面を md5 にして、bms-fetch.ps1 が引く索引を作る。
#   python3 mirror-index.py <ミラーを落とした場所> <Windows での置き場> > mirror-index.tsv
# 1 行に「譜面の md5 <TAB> zip の Windows のパス」。zip の中の zip も 1 段だけ見る
import hashlib, io, os, sys, zipfile

CHARTS = ('.bms', '.bme', '.bml', '.pms', '.bmson')

def charts(z, depth=0):
    for info in z.infolist():
        name = info.filename.lower()
        if name.endswith(CHARTS):
            yield hashlib.md5(z.read(info)).hexdigest()
        elif name.endswith('.zip') and depth == 0:
            try:
                with zipfile.ZipFile(io.BytesIO(z.read(info))) as inner:
                    yield from charts(inner, 1)
            except (zipfile.BadZipFile, OSError, NotImplementedError, RuntimeError):
                pass

root, win_root = sys.argv[1], sys.argv[2].rstrip('\\')
bad = 0
for dirpath, _, files in os.walk(root):
    for f in sorted(files):
        if not f.lower().endswith('.zip'):
            continue
        path = os.path.join(dirpath, f)
        rel = os.path.relpath(path, root).replace('/', '\\')
        try:
            with zipfile.ZipFile(path) as z:
                for md5 in sorted(set(charts(z))):
                    print(f'{md5}\t{win_root}\\{rel}')
        except (zipfile.BadZipFile, OSError, NotImplementedError, RuntimeError) as e:
            bad += 1
            print(f'読めない: {rel}: {e}', file=sys.stderr)
print(f'読めない zip: {bad}', file=sys.stderr)
