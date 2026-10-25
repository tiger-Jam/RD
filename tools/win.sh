#!/usr/bin/env bash
# Mac から Windows 機（ssh win）で BMjoy をビルド・起動する。
#
#   tools/win.sh sync             いまの作業ツリーを送ってビルドする
#   tools/win.sh run [引数...]     Windows のデスクトップで BMjoybody.exe を起動する（ログイン中の画面に出る）
#   tools/win.sh test             Windows でユニットテストを動かす
#   tools/win.sh log              最後に起動したときのログを見る
#   tools/win.sh shot [引数...]    起動して画面を1枚撮り、Mac に持ってくる（開発用）
#
# Windows 側のフォルダは C:\Users\<ユーザー>\BMjoy-beta20261022（変えるなら BMJ_WIN_DIR）
set -euo pipefail
HOST=${BMJ_WIN_HOST:-win}
DIR=${BMJ_WIN_DIR:-BMjoy-beta20261022}
cd "$(dirname "$0")/.."
WINDIR=$(ssh -o BatchMode=yes "$HOST" '$env:USERPROFILE' | tr -d '\r')"\\$DIR"
EXE="$WINDIR\\build\\RelWithDebInfo\\BMjoybody.exe"

ps() { ssh -o BatchMode=yes "$HOST" "[Console]::OutputEncoding=[Text.Encoding]::UTF8; $1"; }
# 引数が空のときは -ArgLine を付けない（powershell -File は空の文字列を「引数なし」とみなして失敗する）
argline() { if [ $# -gt 0 ]; then printf -- "-ArgLine '%s'" "$*"; fi; }

case "${1:-}" in
sync)
    tmp=$(mktemp -d)
    # git の管理下のファイル（コミットしていない変更も含む）だけを送る
    # macOS の tar は拡張属性を ._ のファイルとして入れてしまうので止める（Windows では C のソースに見えて、ビルドが壊れる）
    # 消したばかりでまだコミットしていないファイルは、一覧には残るので外す
    git ls-files -z --cached --others --exclude-standard |
        python3 -I -c "import os,sys; sys.stdout.buffer.write(b'\\0'.join(p for p in sys.stdin.buffer.read().split(b'\\0') if p and os.path.exists(p)))" |
        COPYFILE_DISABLE=1 tar --no-xattrs --null -T - -cf "$tmp/src.tar"
    ps "New-Item -ItemType Directory -Force '$WINDIR' | Out-Null"
    scp -q -o BatchMode=yes "$tmp/src.tar" "$HOST:$DIR/src.tar"
    rm -rf "$tmp"
    # 公式の MOD は丸ごと置き換える（消した MOD が Windows 側に残らないように）
    ps "cd '$WINDIR'; Remove-Item -Recurse -Force mods -ErrorAction SilentlyContinue; tar -xf src.tar; Remove-Item src.tar; cmd /c tools\\windows\\build.cmd"
    ;;
run)
    shift
    ps "New-Item -ItemType Directory -Force '$WINDIR\\run' | Out-Null; powershell -ExecutionPolicy Bypass -File '$WINDIR\\tools\\windows\\launch.ps1' -Exe '$EXE' -WorkDir '$WINDIR\\run' $(argline "$@")"
    ;;
shot)
    shift
    ps "New-Item -ItemType Directory -Force '$WINDIR\\run' | Out-Null; Remove-Item '$WINDIR\\run\\shot.bmp' -ErrorAction SilentlyContinue; powershell -ExecutionPolicy Bypass -File '$WINDIR\\tools\\windows\\launch.ps1' -Exe '$EXE' -WorkDir '$WINDIR\\run' $(argline "$@") -EnvList 'BMJ_SELECT_SHOT=$WINDIR\\run\\shot.bmp;BMJ_KEYCONFIG_SHOT=$WINDIR\\run\\shot.bmp' ; \$t=0; while (-not (Test-Path '$WINDIR\\run\\shot.bmp') -and \$t -lt 120) { Start-Sleep 1; \$t++ }; Start-Sleep 1"
    scp -q -o BatchMode=yes "$HOST:$DIR/run/shot.bmp" ./win_shot.bmp && echo "win_shot.bmp に保存した"
    ;;
test)
    ps "cd '$WINDIR'; .\\build\\RelWithDebInfo\\bmj_tests.exe | Select-Object -Last 1"
    ;;
log)
    scp -q -o BatchMode=yes "$HOST:$DIR/run/last_run.log" /dev/stdout
    ;;
*)
    sed -n '2,11p' "$0"
    exit 1
    ;;
esac
