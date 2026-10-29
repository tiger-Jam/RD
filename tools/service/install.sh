#!/usr/bin/env bash
# Cloudflare の受け口（tools/service/cloudflare）に届いたログを、Mac mini が 10 分ごとに取ってくるようにする（LaunchAgent）。
# 何度実行してもよい。Mac mini は外から受けない（取りに行くだけ）。
#   置き場：~/rd-service（logs/<PC の印>/ に取ったログ、releases/ に作った版）
#   受け口の住所と管理の合言葉：~/rd-service/cloudflare.json（tools/service/cloudflare/setup.sh が書く）
set -euo pipefail
ROOT=$HOME/rd-service
mkdir -p "$ROOT/logs" "$ROOT/releases"
cp "$(dirname "$0")/rd_pull.py" "$ROOT/rd_pull.py"
# 前の形（Mac mini で受けていた受け口）は止めて消す
launchctl bootout "gui/$(id -u)/com.rd.service" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/com.rd.service.plist" "$ROOT/rd_service.py"
PLIST=$HOME/Library/LaunchAgents/com.rd.pull.plist
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.rd.pull</string>
    <key>ProgramArguments</key>
    <array><string>/usr/bin/python3</string><string>-I</string><string>$ROOT/rd_pull.py</string></array>
    <key>StartInterval</key><integer>600</integer>
    <key>RunAtLoad</key><true/>
    <key>StandardOutPath</key><string>$ROOT/pull.log</string>
    <key>StandardErrorPath</key><string>$ROOT/pull.log</string>
</dict>
</plist>
PL
launchctl bootout "gui/$(id -u)/com.rd.pull" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "ログを取りに行く設定を入れた（10 分ごと。記録は $ROOT/pull.log）"
