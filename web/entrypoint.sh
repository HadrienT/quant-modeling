#!/bin/bash
set -e

# ── Render nginx config from template ───────────────────────────
# Only ${PORT} and ${API_URL} are substituted — every other $var is an nginx
# runtime variable and must survive (blueprint WP 14 pitfall).
envsubst '${PORT} ${API_URL}' \
  < /etc/nginx/templates/default.conf.template \
  > /etc/nginx/conf.d/default.conf

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
# --proxy-headers: uvicorn is behind nginx (127.0.0.1), itself behind cloudflared
# and Cloudflare. Trust the immediate peer's X-Forwarded-* so request.url.scheme
# and the logged client IP are the real ones, not nginx's loopback address.
uvicorn api.app.main:app --host 127.0.0.1 --port 8000 \
  --proxy-headers --forwarded-allow-ips=127.0.0.1 &
uvicorn_pid=$!

echo "Waiting for uvicorn on port 8000..."
for _ in $(seq 1 30); do
  curl -sf http://127.0.0.1:8000/health > /dev/null 2>&1 && { echo "uvicorn ready"; break; }
  kill -0 "$uvicorn_pid" 2>/dev/null || { echo "uvicorn exited during startup" >&2; exit 1; }
  sleep 1
done

# pid path + `user` are already set for a non-root runtime in the image's
# /etc/nginx/nginx.conf (see Dockerfile.prod).
nginx -g 'daemon off;' &
nginx_pid=$!

# Exit as soon as either process stops, so Docker restarts the whole container
# instead of leaving it half-alive (a dead API behind a live nginx, or vice versa).
wait -n "$uvicorn_pid" "$nginx_pid"
exit $?
