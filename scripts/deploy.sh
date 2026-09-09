#!/usr/bin/env bash
# Build and (re)start the production stack from this worktree (blueprint WP 14).
#
#   ~/quant-modeling-prod/scripts/deploy.sh
#
# Idempotent. Run it after every `git pull` on this branch. The running
# containers stop depending on the working tree once built, so other sessions
# switching branches elsewhere do not affect a live deployment — only the next
# deploy.
set -euo pipefail

cd "$(dirname "$0")/.."
COMPOSE=(docker compose -f docker-compose.prod.yml)

if [[ ! -f .env ]]; then
  echo "✗ .env is missing. Copy .env.placeholder and fill JWT_SECRET / PGPASSWORD." >&2
  exit 1
fi

# Fast-forward this branch if it tracks a remote (no-op for a local-only branch).
if git rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1; then
  echo "→ git pull --ff-only"
  git pull --ff-only
fi

# The shared docker network with the data-ingest stack.
docker network inspect dataplatform >/dev/null 2>&1 || docker network create dataplatform

COMMIT_SHA="$(git rev-parse --short HEAD)"
export COMMIT_SHA
echo "→ deploying $COMMIT_SHA"

# The tunnel only starts once its credentials exist (RUNBOOK §3). Until then,
# deploy just the app so `cloudflared` doesn't crash-loop in the logs.
services=()
if [[ ! -f cloudflared/quant-modeling.json ]]; then
  echo "⚠ cloudflared/quant-modeling.json missing — deploying 'app' only (see deploy/RUNBOOK.md §3)"
  services=(app)
fi

"${COMPOSE[@]}" build "${services[@]}"
"${COMPOSE[@]}" up -d --remove-orphans "${services[@]}"

# Health gate — poll the container's own healthcheck endpoint.
port="$(grep -E '^QM_WEB_PORT=' .env | cut -d= -f2)"; port="${port:-8091}"
echo -n "→ waiting for health on 127.0.0.1:${port} "
for _ in $(seq 1 60); do
  if curl -sf "http://127.0.0.1:${port}/health" >/dev/null 2>&1; then
    echo "ok"
    curl -s "http://127.0.0.1:${port}/health"; echo
    docker image prune -f >/dev/null
    echo "✓ deployed $COMMIT_SHA"
    exit 0
  fi
  echo -n "."
  sleep 2
done

echo " FAILED" >&2
"${COMPOSE[@]}" logs --tail 40 app >&2
exit 1
