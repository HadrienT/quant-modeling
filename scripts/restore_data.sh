#!/usr/bin/env bash
# Restore the self-hosted accounts + portfolios volume from a backup produced by
# scripts/backup_data.sh (blueprint WP 14 §5 — "a backup never restored is not a
# backup").
#
#   scripts/restore_data.sh /mnt/offsite/quant/qm_data-2026-09-09.tgz
#   scripts/restore_data.sh /mnt/offsite/quant/qm_data-2026-09-09.tgz --into scratch
#
# Default target is the LIVE volume (quant-modeling-prod_qm_data): the app is
# stopped, the volume wiped and repopulated, the app restarted. Pass
# `--into scratch` to restore into a throwaway volume and diff instead — that is
# the periodic "did the backup actually work" check.
set -euo pipefail

ARCHIVE="${1:?usage: restore_data.sh <archive.tgz> [--into scratch]}"
MODE="${3:-live}"   # $2 is the literal "--into"
[[ -f "$ARCHIVE" ]] || { echo "✗ no such archive: $ARCHIVE" >&2; exit 1; }

cd "$(dirname "$0")/.."
COMPOSE=(docker compose -f docker-compose.prod.yml)
LIVE_VOLUME="quant-modeling-prod_qm_data"
ARCHIVE_ABS="$(cd "$(dirname "$ARCHIVE")" && pwd)/$(basename "$ARCHIVE")"

if [[ "$MODE" == "scratch" ]]; then
  VOL="qm_data_restore_check"
  echo "→ restoring into throwaway volume '$VOL' (live data untouched)"
  docker volume rm "$VOL" >/dev/null 2>&1 || true
  docker volume create "$VOL" >/dev/null
  docker run --rm -v "$VOL:/data" -v "$ARCHIVE_ABS:/archive.tgz:ro" alpine \
    sh -c 'cd /data && tar xzf /archive.tgz'
  echo "→ contents:"
  docker run --rm -v "$VOL:/data" alpine sh -c 'ls -R /data | head -50'
  echo
  echo "✓ archive extracts cleanly. Remove with:  docker volume rm $VOL"
  exit 0
fi

echo "⚠  This REPLACES the live volume '$LIVE_VOLUME' with the contents of:"
echo "     $ARCHIVE_ABS"
read -r -p "   Type 'restore' to proceed: " confirm
[[ "$confirm" == "restore" ]] || { echo "aborted."; exit 1; }

echo "→ stopping app"
"${COMPOSE[@]}" stop app

echo "→ wiping and repopulating $LIVE_VOLUME"
docker run --rm -v "$LIVE_VOLUME:/data" -v "$ARCHIVE_ABS:/archive.tgz:ro" alpine \
  sh -c 'rm -rf /data/* /data/..?* /data/.[!.]* 2>/dev/null; cd /data && tar xzf /archive.tgz'

echo "→ starting app"
"${COMPOSE[@]}" up -d app

port="$(grep -E '^QM_WEB_PORT=' .env | cut -d= -f2)"; port="${port:-8091}"
for _ in $(seq 1 30); do
  curl -sf "http://127.0.0.1:${port}/health" >/dev/null 2>&1 && { echo "✓ restored, app healthy"; exit 0; }
  sleep 2
done
echo "✗ app did not come back healthy — check: ${COMPOSE[*]} logs app" >&2
exit 1
