#!/usr/bin/env python3
# 公開版のソースから、出典（OpenLR2 の関数名・ファイルと行番号）を指すコメントを外す。開発版（リポジトリ）はそのまま。
#   strip_cites.py <ソースのフォルダ>      そのフォルダの .cpp・.h を書き換え、外した数を出す
#   strip_cites.py --diff <ソースのフォルダ>  書き換えずに、変わる行を出す（点検用）
# 外すもの：
#   ・「LR2: 識別子」（関数名・ファイル名・ファイル:行）。後ろに「。説明」が続けば説明は残す
#   ・ファイル:行だけの括弧（skindraw:575 など）
#   ・OpenLR2 を名指しする括弧と文
# 挙動の説明（「LR2 と同じ」・「LR2: 入力受付と同時に…」など）は残す
import re
import sys
from pathlib import Path

# OpenLR2 のソースのファイル名（拡張子なしで書くことがある）
FILES = (r"(?:LR2_)?(?:bmsload|Play|songmanage|Main|statplay|skindraw|Scene\d\d_\w+|gameloop|skinload|Songselect|mng|fileutil|"
         r"skinobject|skinmanage|net|fu|load|En_fileutil|DxUseCLib|DxSystem|bmsplay|Result)")
FILE_REF = re.compile(FILES + r"(?:\.cpp|\.h)?(?::\d+(?:-\d+)?)?(?![A-Za-z])")
FILE_LINE = re.compile(FILES + r"(?:\.cpp|\.h)?:\d+(?:-\d+)?")
# 「LR2: 」のあとの出典（英字で始まる識別子・ファイル名、それを + や → や / や ・ でつないだもの、ファイル:行）
IDENT = r"[A-Za-z_][\w.]*(?:\(\))?(?::\d+(?:-\d+)?)?(?:\*)?"
CITE = re.compile(r"LR2:\s*" + IDENT + r"(?:\s*(?:\+|→|/|・|と|の)\s*" + IDENT + r")*")
# 決まりで外しきれないものは、そのまま置き換える
EXACT = {
    "// LR2: ReadSRC の DerivationGraph と同じ切り出し": "// SRC の縦横の分け方に合わせた切り出し",
    "（mng:362-371。空なら読むたびにランダム）": "（空なら読むたびにランダム）",
    "（LR2: gameloop:183。": "（",
    "。LR2: Scene13_Courseresult.cpp）": "）",
    "// LR2: totalnotes - note_current + BAD + POOR": "// 残りのノーツ + BAD + POOR",
    "// LR2: rank = 2": "// 既定は NORMAL",
    "。LR2: bmsload:3810": "",
}
# 外したあとに残っていたら知らせる
SUSPECT = re.compile(r"OpenLR2|LR2:\s*[A-Za-z_]|[A-Za-z_]+\.(?:cpp|h):\d|\b(?:bmsload|skindraw|songmanage|statplay|gameloop|skinload|fileutil|"
                     r"skinobject|skinmanage|mng|fu|load):\d")
OPEN = "（("
CLOSE = "）)"


def split_comment(line):
    """行を（コードの部分, コメントの部分）に分ける。文字列の中の // は見ない"""
    i, n = 0, len(line)
    quote = None
    while i < n:
        c = line[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            # R"(...)" の生文字列は 1 行で閉じるものだけ扱う
            quote = c
        elif line.startswith("//", i):
            return line[:i], line[i:]
        i += 1
    return line, ""


def strip_parens(text):
    """出典だけを指す括弧を外す。括弧の中が「出典。説明」なら説明を残す"""
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c in OPEN:
            depth, j = 1, i + 1
            while j < len(text) and depth:
                if text[j] in OPEN:
                    depth += 1
                elif text[j] in CLOSE:
                    depth -= 1
                j += 1
            inner = text[i + 1:j - 1] if depth == 0 else None
            if inner is not None:
                new = clean_inner(inner)
                if new is None:
                    i = j
                    continue
                out.append(c + new + text[j - 1])
                i = j
                continue
        out.append(c)
        i += 1
    return "".join(out)


def clean_inner(inner):
    """括弧の中身を整える。全部が出典なら None"""
    s = inner.strip()
    if "OpenLR2" in s:
        return None
    m = CITE.match(s)
    if m:
        rest = s[m.end():]
        if rest.startswith("。"):
            rest = clean_inner(rest[1:])
            return rest if rest else None
        if rest.strip() == "":
            return None
        return None  # 「LR2: Play.cpp:710 の judge_x」のように出典と続いているものは、括弧ごと外す
    if FILE_LINE.fullmatch(s) or re.fullmatch(r"(?:" + FILE_LINE.pattern + r"[、,/ ]*)+", s):
        return None
    # 括弧の中の後ろに付いた「、bmsload:123」「。fu:829-866」
    s2 = re.sub(r"[、。,]\s*(?:LR2:\s*)?" + FILE_LINE.pattern + r"(?:\s*[/、]\s*" + FILE_LINE.pattern + r")*\s*$", "", inner)
    s2 = re.sub(r"\s*(?:LR2:\s*)?" + FILE_LINE.pattern + r"\s*/\s*", "", s2)
    return strip_parens(s2)


def clean_comment(body):
    """// のあとの文を整える。全部が出典なら None"""
    text = strip_parens(body)
    # 先頭の「LR2: 識別子」
    m = re.match(r"\s*" + CITE.pattern, text)
    if m:
        rest = text[m.end():]
        if rest.startswith("。"):
            rest = rest[1:]
        elif rest.strip() and not rest.lstrip().startswith(("（", "(")):
            rest = text  # 「LR2: 入力受付と同時に」のような説明は残す（CITE は英字で始まるので、ここに来るのは英字の説明）
        text = (" " + rest.lstrip()) if rest.strip() else ""
    # OpenLR2 を名指しする文
    parts = re.split(r"(?<=。)", text)
    parts = [p for p in parts if "OpenLR2" not in p]
    text = "".join(parts)
    text = re.sub(r"\s+$", "", text)
    if text and not text.startswith(" "):
        text = " " + text
    # 出典を外して括弧だけが残ったコメントは、括弧も外す
    m = re.fullmatch(r"\s*[（(](.*)[）)]", text)
    if m and "（" not in m.group(1) and "(" not in m.group(1):
        text = " " + m.group(1)
    if not text.strip() or text.strip() in ("。", "、"):
        return None
    return text


def process(path, show_diff):
    src = path.read_text(encoding="utf-8")
    for a, b in EXACT.items():
        src = src.replace(a, b)
    lines = src.split("\n")
    out, changed = [], 0
    for line in lines:
        code, comment = split_comment(line)
        if not comment:
            out.append(line)
            continue
        body = comment[2:]
        new = clean_comment(body)
        if new is not None and new == body.rstrip():
            out.append(line)
            continue
        changed += 1
        if new is None:
            new_line = code.rstrip()
            if not new_line.strip():
                if show_diff:
                    print(f"{path}: - {line.strip()}")
                continue  # コメントだけの行は消す
        else:
            new_line = code + "//" + new
        if show_diff:
            print(f"{path}: - {line.strip()}\n{' ' * len(str(path))}  + {new_line.strip()}")
        out.append(new_line)
    result = "\n".join(out)
    if result != path.read_text(encoding="utf-8") and not show_diff:
        path.write_text(result, encoding="utf-8")
    for i, line in enumerate(out, 1):
        _, comment = split_comment(line)
        if comment and SUSPECT.search(comment):
            print(f"残っている：{path}:{i}: {line.strip()}", file=sys.stderr)
    return changed


def main():
    args = sys.argv[1:]
    show = args and args[0] == "--diff"
    if show:
        args = args[1:]
    root = Path(args[0])
    total = 0
    for p in sorted(root.rglob("*")):
        if p.suffix in (".cpp", ".h") and "third_party" not in p.parts:
            total += process(p, show)
    print(f"出典のコメントを {total} 行直した", file=sys.stderr)


if __name__ == "__main__":
    main()
