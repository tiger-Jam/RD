#!/usr/bin/env bash
# Cloudflare の受け口を作る・作り直す（何度実行してもよい）。先に一度 `npx wrangler login` でログインしておく。
#   1. R2 の置き場 rd-service を作る（あればそのまま）
#   2. 受け口（Workers の rd-service）を置く
#   3. 管理の合言葉を作って Workers に入れ、~/rd-service/cloudflare.json に住所と一緒に書く（Mac mini がログを取りに行くのに使う）
set -euo pipefail
cd "$(dirname "$0")"
W="env -u NODE_OPTIONS npx wrangler"
[ -d node_modules ] || env -u NODE_OPTIONS npm install --no-fund --no-audit
$W r2 bucket create rd-service 2>&1 | grep -v "already exists" || true
OUT=$($W deploy 2>&1) || { echo "$OUT"; exit 1; }
echo "$OUT" | tail -5
URL=$(echo "$OUT" | grep -Eo "https://[a-zA-Z0-9.-]+\.workers\.dev" | head -1)
[ -n "$URL" ] || { echo "受け口の住所が分からない"; exit 1; }
CFG=$HOME/rd-service/cloudflare.json
mkdir -p "$HOME/rd-service"
SECRET=$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))['admin_secret'])" "$CFG" 2>/dev/null || openssl rand -hex 24)
printf '%s' "$SECRET" | $W secret put ADMIN_SECRET >/dev/null
umask 077
python3 -I -c "import json,sys; json.dump({'url': sys.argv[1], 'admin_secret': sys.argv[2]}, open(sys.argv[3], 'w'))" "$URL" "$SECRET" "$CFG"
echo "受け口：${URL}（RD の既定の送り先・取りに行く先にする）"
