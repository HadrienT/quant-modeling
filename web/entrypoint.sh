#!/bin/bash
set -e

# ── Render nginx config from template ───────────────────────────
# Only ${PORT} and ${API_URL} are substituted — every other $var is an nginx
# runtime variable and must survive (blueprint WP 14 pitfall).
envsubst '${PORT} ${API_URL}' \
  < /etc/nginx/templates/default.conf.template \
  > /etc/nginx/conf.d/default.conf
rm -f /etc/nginx/sites-enabled/default

# ── Runtime app configuration (blueprint WP 14 §1) ──────────────
# One image, many environments: the app reads /config.json before its first
# request. VITE_* is NOT used and the API key never appears here.
cat > /usr/share/nginx/html/config.json <<EOF
{
  "apiBase": "${APP_API_BASE:-}",
  "commitSha": "${COMMIT_SHA:-dev}"
}
EOF

# ── Start uvicorn (API) then nginx ─────────────────────────────
uvicorn api.app.main:app --host 127.0.0.1 --port 8000 &

echo "Waiting for uvicorn on port 8000..."
for i in $(seq 1 30); do
  curl -sf http://127.0.0.1:8000/health > /dev/null 2>&1 && { echo "uvicorn ready"; break; }
  sleep 1
done

# Non-root worker: keep the pid and temp paths writable.
exec nginx -g 'daemon off; pid /tmp/nginx/nginx.pid;'
