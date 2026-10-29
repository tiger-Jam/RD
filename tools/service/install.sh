#!/usr/bin/env bash
# BMjoy の受け取り口（bmjoy_service.py）を Mac mini で常に動かす（LaunchAgent）。何度実行してもよい。
#   置き場：~/bmjoy-service（update/ に新しい版、logs/<PC の印>/ に届いたログ、releases/ に作った版）
#   待ち受け：127.0.0.1:4570（外からは tailscale funnel を通してだけ届く。funnel は別に設定する）
#     tailscale funnel --bg --https=8443 http://127.0.0.1:4570
#   配った BMjoy が使う住所：https://<この Mac の Tailscale の名前>:8443/bmjoy
set -euo pipefail
ROOT=$HOME/bmjoy-service
mkdir -p "$ROOT/update" "$ROOT/logs" "$ROOT/releases"
cp "$(dirname "$0")/bmjoy_service.py" "$ROOT/bmjoy_service.py"
PLIST=$HOME/Library/LaunchAgents/com.bmjoy.service.plist
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.bmjoy.service</string>
    <key>ProgramArguments</key>
    <array><string>/usr/bin/python3</string><string>-I</string><string>$ROOT/bmjoy_service.py</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>StandardOutPath</key><string>$ROOT/service.log</string>
    <key>StandardErrorPath</key><string>$ROOT/service.log</string>
</dict>
</plist>
PL
launchctl bootout "gui/$(id -u)/com.bmjoy.service" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
sleep 1
curl -s -o /dev/null -w "受け取り口：%{http_code}（404 なら動いている）\n" http://127.0.0.1:4570/
