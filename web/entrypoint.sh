#!/bin/bash
set -e

# ── Render nginx config from template ────────────────────────────
# The nginx Docker image normally does this via its own entrypoint,
# but since we use python:3.12-slim as base we do it manually.
envsubst '${PORT} ${API_URL}' \
  < /etc/nginx/templates/default.conf.template \
  > /etc/nginx/conf.d/default.conf

# Remove the default nginx site if it exists
rm -f /etc/nginx/sites-enabled/default

# ── Start uvicorn (API) in the background ────────────────────────
uvicorn api.app.main:app --host 127.0.0.1 --port 8000 &

# ── Wait for uvicorn to be ready before starting nginx ───────────
echo "Waiting for uvicorn on port 8000..."
for i in $(seq 1 30); do
  if curl -sf http://127.0.0.1:8000/health > /dev/null 2>&1; then
    echo "uvicorn is ready"
    break
  fi
  sleep 1
done

# ── Start nginx in the foreground ────────────────────────────────
exec nginx -g 'daemon off;'
