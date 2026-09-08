#!/usr/bin/env bash
# Blueprint WP 14 §5 — dump the self-hosted data (accounts + portfolios) that
# replaced GCS. Once out of the cloud nobody backs this up for you.
#
#   crontab:  0 3 * * *  /path/to/scripts/backup_data.sh /mnt/offsite/quant
#
# Restore (tested at least once — a backup never restored is not a backup):
#   docker run --rm -v qm_data:/data -v "$PWD":/b alpine \
#     sh -c 'cd /data && tar xzf /b/qm_data-YYYY-MM-DD.tgz'
set -euo pipefail

DEST="${1:?usage: backup_data.sh <destination-dir>}"
STAMP="$(date +%F)"
mkdir -p "$DEST"

docker run --rm -v qm_data:/data -v "$DEST":/backup alpine \
  tar czf "/backup/qm_data-${STAMP}.tgz" -C /data .

# keep 14 days
find "$DEST" -name 'qm_data-*.tgz' -mtime +14 -delete
echo "backed up qm_data → $DEST/qm_data-${STAMP}.tgz"
