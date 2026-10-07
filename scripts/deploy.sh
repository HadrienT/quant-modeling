#!/usr/bin/env bash
# Build and (re)start the production stack from this worktree (blueprint WP 14).
#
#   ~/quant-modeling-prod/scripts/deploy.sh
#
# Idempotent, and safe on boot (the systemd unit calls it). Records the deployed
# commit in .env so that a bare `docker compose up -d` brings up the SAME image.
set -euo pipefail

cd "$(dirname "$0")/.."
COMPOSE=(docker compose -f docker-compose.prod.yml)

if [[ ! -f .env ]]; then
  echo "✗ .env is missing. Copy .env.placeholder and fill it in." >&2
  exit 1
fi

# Fast-forward to the remote when reachable. Non-fatal: on boot the network may
# not be up yet, and a stale checkout still deploys a working (older) site.
# Not when scripts/auto-deploy.sh calls (DEPLOY_NO_PULL=1: it deploys exactly
# the commit whose CI it checked), and not onto a commit it had to roll back —
# the boot unit runs this script, and must not bring that commit back.
rolled_back="$(cat "$(git rev-parse --absolute-git-dir)/autodeploy-failed" 2>/dev/null || true)"
if [[ -n "${DEPLOY_NO_PULL:-}" ]]; then
  :
elif git rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1; then
  git fetch --quiet 2>/dev/null || true
  if [[ -n "$rolled_back" && "$(git rev-parse '@{u}')" == "$rolled_back" ]]; then
    echo "⚠ $(git rev-parse --short '@{u}') was rolled back by auto-deploy — staying on $(git rev-parse --short HEAD)"
  else
    git merge --ff-only --quiet '@{u}' 2>/dev/null \
      && echo "→ synced with $(git rev-parse --abbrev-ref '@{u}')" \
      || echo "⚠ git pull skipped (offline or diverged) — deploying the current checkout"
  fi
fi

docker network inspect dataplatform >/dev/null 2>&1 || docker network create dataplatform

COMMIT_SHA="$(git rev-parse --short HEAD)"
export COMMIT_SHA
echo "→ deploying $COMMIT_SHA"

# The tunnel is in the `tunnel` compose profile; include it once the token is set.
if grep -qE '^CLOUDFLARE_TUNNEL_TOKEN=.+' .env; then
  COMPOSE+=(--profile tunnel)
else
  echo "⚠ CLOUDFLARE_TUNNEL_TOKEN not set in .env — deploying 'app' only (see deploy/RUNBOOK.md §3)"
fi

"${COMPOSE[@]}" build
"${COMPOSE[@]}" up -d --remove-orphans

# Persist the tag so `docker compose up -d` (systemd unit, or a bare call) runs
# THIS image, not whatever ${COMMIT_SHA:-latest} last resolved to (e.g. :dev).
if grep -qE '^COMMIT_SHA=' .env; then
  sed -i "s/^COMMIT_SHA=.*/COMMIT_SHA=${COMMIT_SHA}/" .env
else
  printf '\nCOMMIT_SHA=%s\n' "${COMMIT_SHA}" >> .env
fi

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
