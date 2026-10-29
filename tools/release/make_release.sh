#!/usr/bin/env bash
# 公開版を作る（Mac から動かす）。今のコミット（HEAD）から、出典のコメントを外したソースを作り、
# Windows 機（ssh win）で開発用の口を切ってビルドし、zip にして Mac へ持ってくる。
#
#   tools/release/make_release.sh <版> [--publish] [--notes "変わったこと"]
#
#   版は 年.月.日.番号（例 2026.10.30.1）
#   --publish を付けると、受け取り口（~/rd-service/update）に置き、配った RD が次の起動で入れ替えるようにする
#
# 出来たもの：~/rd-service/releases/<版>/（RD-<版>.zip・RD-<版>-update.zip・.pdb）
# Windows では C:\Users\<ユーザー>\RD-release\<版> で作業する（開発のフォルダとは分ける）。INFINITAS が動いていたら止まる
set -euo pipefail
VER=${1:?版を指定してください（例 2026.10.30.1）}
shift
PUBLISH=0
NOTES=""
while [ $# -gt 0 ]; do
    case "$1" in
    --publish) PUBLISH=1 ;;
    --notes) NOTES=$2; shift ;;
    *) echo "知らない引数: $1"; exit 2 ;;
    esac
    shift
done
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "版は 年.月.日.番号 の形で"; exit 2; }
HOST=${RD_WIN_HOST:-win}
DEV_DIR=${RD_WIN_DIR:-BMjoy-beta20261022}
SERVICE=${RD_SERVICE_ROOT:-$HOME/rd-service}
cd "$(dirname "$0")/../.."
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then echo "コミットしていない変更がある（公開版は HEAD から作る）"; exit 1; fi

ps() { ssh -o BatchMode=yes "$HOST" "[Console]::OutputEncoding=[Text.Encoding]::UTF8; $1"; }
if ps 'Get-Process bm2dx* -ErrorAction SilentlyContinue' | grep -q .; then echo "INFINITAS が動いているのでやめる"; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/src"
git archive HEAD | tar -xf - -C "$tmp/src"
# 公開版に入れないもの：点検の記録（OpenLR2 との読み比べ）
rm -rf "$tmp/src/docs/audit"
python3 -I tools/release/strip_cites.py "$tmp/src/src"
python3 -I tools/release/strip_cites.py "$tmp/src/tests"
(cd "$tmp/src" && COPYFILE_DISABLE=1 tar --no-xattrs -cf "$tmp/src.tar" .)

WINHOME=$(ps '$env:USERPROFILE' | tr -d '\r')
ROOT="$WINHOME\\RD-release"
ps "New-Item -ItemType Directory -Force '$ROOT\\$VER\\src' | Out-Null; Remove-Item -Recurse -Force '$ROOT\\$VER\\src\\*', '$ROOT\\$VER\\out' -ErrorAction SilentlyContinue; exit 0"
scp -q -o BatchMode=yes "$tmp/src.tar" "$HOST:RD-release/$VER/src.tar"
ps "tar -xf '$ROOT\\$VER\\src.tar' -C '$ROOT\\$VER\\src'; Remove-Item '$ROOT\\$VER\\src.tar'"
# 素材は開発のフォルダの RD の置き場から（名前を変える前なら LR2files）
ASSETS="$WINHOME\\$DEV_DIR\\build\\RelWithDebInfo\\RDfiles"
ps "if (-not (Test-Path '$ASSETS')) { exit 1 }" || ASSETS="$WINHOME\\$DEV_DIR\\build\\RelWithDebInfo\\LR2files"
ps "powershell -ExecutionPolicy Bypass -File '$ROOT\\$VER\\src\\tools\\windows\\release.ps1' -Ver $VER -Root '$ROOT' -Assets '$ASSETS'" | tee "$tmp/release.log"
grep -q RELEASE_OK "$tmp/release.log" || { echo "Windows で作れなかった"; exit 1; }

OUT="$SERVICE/releases/$VER"
mkdir -p "$OUT"
for f in "RD-$VER.zip" "RD-$VER-update.zip" RDbody.pdb RD.pdb; do
    scp -q -o BatchMode=yes "$HOST:RD-release/$VER/out/$f" "$OUT/$f"
done
SHA=$(shasum -a 256 "$OUT/RD-$VER-update.zip" | cut -d' ' -f1)
echo "作った：$OUT"
ls -lh "$OUT"

if [ "$PUBLISH" = 1 ]; then
    mkdir -p "$SERVICE/update"
    cp "$OUT/RD-$VER-update.zip" "$OUT/RD-$VER.zip" "$SERVICE/update/"
    python3 -I - "$SERVICE/update/latest.json" "$VER" "RD-$VER-update.zip" "$SHA" "$NOTES" <<'EOF'
import json, sys
path, ver, url, sha, notes = sys.argv[1:6]
with open(path + ".tmp", "w", encoding="utf-8") as f:
    json.dump({"version": ver, "url": url, "sha256": sha, "notes": notes, "full": f"RD-{ver}.zip"}, f, ensure_ascii=False)
import os
os.replace(path + ".tmp", path)
EOF
    echo "公開した：${SERVICE}/update/latest.json（${VER}）"
fi
