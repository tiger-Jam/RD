# 難易度表ごとに、持っている譜面と持っていない譜面を Excel の表（.xlsx）にする。
#   RDbody.exe --list-charts "*" --tables-file <集める表の一覧> > charts.tsv
#   py -3 chart-sheet.py charts.tsv <ミラーの索引 mirror-index.tsv> <書き出す .xlsx>
# 最初のシートは表ごとの数（譜面・持っている・持っていない）、そのあとに表ごとのシート
# （レベル・曲名・作者・持っているか・URL1〜3・md5）。URL は持っていない譜面だけ。その譜面が載っているどの表の差分の URL、
# 差分のミラー、本体の URL の順に、重ならないように 3 つまで。Excel を開いたままだと書けないので、そのときは別の名前で書く
import html
import os
import re
import sys
import urllib.parse
import zipfile

if len(sys.argv) != 4:
    sys.exit("使い方: chart-sheet.py <charts.tsv> <mirror-index.tsv> <書き出す .xlsx>")
src, mirror_path, out = sys.argv[1:]

MIRROR_ROOT = "E:\\BMSFolder\\_mirror\\hexlataia\\"
MIRROR_URL = "https://bms.hexlataia.xyz/mirror/"
mirror = {}
if os.path.exists(mirror_path):
    for line in open(mirror_path, encoding="utf-8"):
        md5, _, path = line.rstrip("\r\n").partition("\t")
        if path.startswith(MIRROR_ROOT):
            rel = path[len(MIRROR_ROOT):].replace("\\", "/")
            mirror.setdefault(md5, []).append(MIRROR_URL + urllib.parse.quote(rel))

tables = {}  # 表の URL → {name, rows}（出てきた順。選曲画面に出す表が先）
diffs, mains = {}, {}
for line in open(src, encoding="utf-8", errors="replace"):
    f = line.rstrip("\r\n").split("\t")
    if len(f) < 9 or len(f[0]) != 32:
        continue
    md5, level, title, artist, name, main, diff, have, url = f[:9]
    t = tables.setdefault(url, {"name": name, "rows": []})
    t["rows"].append((level, title, artist, have == "1", md5))
    if diff.startswith("http"):
        diffs.setdefault(md5, []).append(diff)
    if main.startswith("http"):
        mains.setdefault(md5, []).append(main)

def candidates(md5):
    out_urls = []
    for u in diffs.get(md5, []) + mirror.get(md5, []) + mains.get(md5, []):
        if u not in out_urls:
            out_urls.append(u)
    return out_urls[:3]

BAD_XML = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff]")
def esc(s):
    return html.escape(BAD_XML.sub("", s), quote=True)

def col(i):
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s

# 文字は共有の表（sharedStrings）に 1 度だけ入れる（同じ譜面が多くの表に載るので、ファイルが小さくなる）
shared, shared_list = {}, []
def sid(v):
    i = shared.get(v)
    if i is None:
        i = shared[v] = len(shared_list)
        shared_list.append(v)
    return i

def sheet_xml(header, rows, widths, links, jumps=None):
    # rows は文字か数のリスト。links はセル → URL（シートの rels に入れる）、jumps はセル → 飛び先のシートの名前
    parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
             '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
             'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
             '<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/>'
             '</sheetView></sheetViews><cols>']
    for i, w in enumerate(widths):
        parts.append(f'<col min="{i + 1}" max="{i + 1}" width="{w}" customWidth="1"/>')
    parts.append("</cols><sheetData>")
    for r, row in enumerate([header] + rows, start=1):
        cells = []
        for c, v in enumerate(row):
            if v is None or v == "":
                continue
            ref = f"{col(c)}{r}"
            style = ' s="1"' if r == 1 else (' s="2"' if (ref in links or (jumps and ref in jumps)) else "")
            if isinstance(v, (int, float)):
                cells.append(f'<c r="{ref}"{style}><v>{v}</v></c>')
            else:
                cells.append(f'<c r="{ref}"{style} t="s"><v>{sid(str(v))}</v></c>')
        parts.append(f'<row r="{r}">{"".join(cells)}</row>')
    parts.append("</sheetData>")
    last = f"{col(len(header) - 1)}{len(rows) + 1}"
    parts.append(f'<autoFilter ref="A1:{last}"/>')
    if links or jumps:
        parts.append("<hyperlinks>")
        for n, ref in enumerate(links, start=1):
            parts.append(f'<hyperlink ref="{ref}" r:id="rId{n}"/>')
        for ref, name in (jumps or {}).items():
            loc = "'" + name.replace("'", "''") + "'!A1"
            parts.append(f'<hyperlink ref="{ref}" location="{esc(loc)}" display="{esc(name)}"/>')
        parts.append("</hyperlinks>")
    parts.append("</worksheet>")
    rels = None
    if links:
        rels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
        for n, (ref, url) in enumerate(links.items(), start=1):
            rels.append(f'<Relationship Id="rId{n}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" '
                        f'Target="{esc(url)}" TargetMode="External"/>')
        rels.append("</Relationships>")
        rels = "".join(rels)
    return "".join(parts), rels

# シートの名前（31 文字まで。使えない字を除き、重なれば番号を付ける）
used = set()
def sheet_name(name, n):
    s = re.sub(r"[\[\]:*?/\\]", "", name).strip().strip("'") or f"表{n}"
    s = s[:31]
    base, k = s, 2
    while s.lower() in used:
        suffix = f" ({k})"
        s = base[:31 - len(suffix)] + suffix
        k += 1
    used.add(s.lower())
    return s

sheets = []  # (名前, xml, rels)
summary = []
used.add("一覧")
for n, (url, t) in enumerate(tables.items(), start=1):
    rows, links = [], {}
    for r, (level, title, artist, have, md5) in enumerate(t["rows"], start=2):
        urls = [] if have else candidates(md5)  # 持っている譜面には URL を出さない（ファイルが重くなる）
        rows.append([level, title, artist, "○" if have else "", *(urls + [""] * (3 - len(urls))), md5])
        for i, u in enumerate(urls):
            if len(u) < 2000:
                links[f"{col(4 + i)}{r}"] = u
    have_n = sum(1 for x in t["rows"] if x[3])
    name = sheet_name(t["name"], n)
    xml, rels = sheet_xml(["レベル", "曲名", "作者", "持っている", "URL1", "URL2", "URL3", "md5"], rows,
                          [9, 48, 32, 10, 40, 40, 40, 34], links)
    sheets.append((name, xml, rels))
    total = len(t["rows"])
    summary.append([t["name"], total, have_n, total - have_n, round(have_n * 100.0 / total, 1) if total else 0, name, url])

sxml, _ = sheet_xml(["表", "譜面", "持っている", "持っていない", "持っている（%）", "シート", "表の URL"], summary,
                    [36, 9, 11, 13, 15, 30, 60], {}, {f"F{r}": row[5] for r, row in enumerate(summary, start=2)})
sheets.insert(0, ("一覧", sxml, None))

STYLES = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
          '<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">'
          '<fonts count="3"><font><sz val="11"/><name val="Meiryo UI"/></font>'
          '<font><b/><sz val="11"/><name val="Meiryo UI"/></font>'
          '<font><u/><sz val="11"/><color rgb="FF0563C1"/><name val="Meiryo UI"/></font></fonts>'
          '<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>'
          '<fill><patternFill patternType="solid"><fgColor rgb="FFDDEBF7"/></patternFill></fill></fills>'
          '<borders count="1"><border/></borders><cellStyleXfs count="1"><xf/></cellStyleXfs>'
          '<cellXfs count="3"><xf fontId="0" xfId="0"/><xf fontId="1" fillId="2" xfId="0" applyFont="1" applyFill="1"/>'
          '<xf fontId="2" xfId="0" applyFont="1"/></cellXfs>'
          '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>')

def write(path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        ct = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
              '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
              '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
              '<Default Extension="xml" ContentType="application/xml"/>'
              '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
              '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'
              '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>']
        wb = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
              '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
              'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>']
        wbrels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                  '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
        defined = []
        for i, (name, xml, rels) in enumerate(sheets, start=1):
            ct.append(f'<Override PartName="/xl/worksheets/sheet{i}.xml" '
                      'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>')
            wb.append(f'<sheet name="{esc(name)}" sheetId="{i}" r:id="rId{i}"/>')
            wbrels.append(f'<Relationship Id="rId{i}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" '
                          f'Target="worksheets/sheet{i}.xml"/>')
            z.writestr(f"xl/worksheets/sheet{i}.xml", xml)
            if rels:
                z.writestr(f"xl/worksheets/_rels/sheet{i}.xml.rels", rels)
            # 絞り込みの範囲（Excel が autoFilter と一緒に要る名前）
            m = re.search(r'<autoFilter ref="A1:([A-Z]+)(\d+)"', xml)
            if m:
                defined.append(f'<definedName name="_xlnm._FilterDatabase" localSheetId="{i - 1}" hidden="1">'
                               f"'{esc(name).replace(chr(39), chr(39) * 2)}'!$A$1:${m.group(1)}${m.group(2)}</definedName>")
        wb.append("</sheets>")
        if defined:
            wb.append("<definedNames>" + "".join(defined) + "</definedNames>")
        wb.append("</workbook>")
        n = len(sheets)
        wbrels.append(f'<Relationship Id="rId{n + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" '
                      'Target="styles.xml"/>'
                      f'<Relationship Id="rId{n + 2}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" '
                      'Target="sharedStrings.xml"/></Relationships>')
        ct.append("</Types>")
        z.writestr("[Content_Types].xml", "".join(ct))
        z.writestr("_rels/.rels", '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                   '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
                   'Target="xl/workbook.xml"/></Relationships>')
        z.writestr("xl/workbook.xml", "".join(wb))
        z.writestr("xl/_rels/workbook.xml.rels", "".join(wbrels))
        z.writestr("xl/styles.xml", STYLES)
        z.writestr("xl/sharedStrings.xml", '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
                   f'<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" uniqueCount="{len(shared_list)}">'
                   + "".join(f'<si><t xml:space="preserve">{esc(v)}</t></si>' for v in shared_list) + "</sst>")

tmp = out + ".tmp"
write(tmp)
try:
    os.replace(tmp, out)
    print(f"書き出した: {out}（表 {len(tables)} 個）")
except OSError:
    alt = os.path.splitext(out)[0] + "（新）.xlsx"
    os.replace(tmp, alt)
    print(f"{out} を開いているので {alt} に書き出した（表 {len(tables)} 個）")
