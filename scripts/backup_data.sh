#!/usr/bin/env bash
# Blueprint WP 14 §5 — dump the self-hosted data (accounts + portfolios) that
# replaced GCS. Once out of the cloud nobody backs this up for you.
#
#   scripts/backup_data.sh /mnt/offsite/quant
#
# Installed as a systemd timer by deploy/RUNBOOK.md §4. Restore with
# scripts/restore_data.sh (and test it — a backup never restored is not a backup).
set -euo pipefail

DEST="${1:?usage: backup_data.sh <destination-dir>}"
VOLUME="${QM_DATA_VOLUME:-quant-modeling-prod_qm_data}"
STAMP="$(date +%F)"
KEEP_DAYS="${QM_BACKUP_KEEP_DAYS:-14}"

mkdir -p "$DEST"

if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
  echo "✗ volume '$VOLUME' not found — is the prod stack up?" >&2
  exit 1
fi

docker run --rm -v "$VOLUME:/data:ro" -v "$DEST:/backup" alpine \
  tar czf "/backup/qm_data-${STAMP}.tgz" -C /data .

find "$DEST" -name 'qm_data-*.tgz' -mtime "+${KEEP_DAYS}" -delete
echo "backed up $VOLUME → $DEST/qm_data-${STAMP}.tgz ($(du -h "$DEST/qm_data-${STAMP}.tgz" | cut -f1))"
