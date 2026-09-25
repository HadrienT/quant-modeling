#!/usr/bin/env bash
# One-screen health board for the production site and everything it depends on
# (deploy/RUNBOOK.md "Opérations courantes"). Read-only: it never starts or
# stops anything.
#
#   scripts/status.sh                   # once
#   watch -c -n 10 scripts/status.sh    # live, refreshed every 10 s
#
# Exit code: 0 if everything is up, 1 otherwise (usable from cron / a timer).
set -uo pipefail

PROD_DIR="${QM_PROD_DIR:-$HOME/quant-modeling-prod}"
# Compose projects on this server, in boot order: market data, platform, site.
PROJECTS=(data-ingest quant-platform quant-modeling-prod)

if [[ -t 1 || -n "${FORCE_COLOR:-}" ]]; then
  G=$'\e[32m' R=$'\e[31m' Y=$'\e[33m' D=$'\e[2m' B=$'\e[1m' N=$'\e[0m'
else
  G='' R='' Y='' D='' B='' N=''
fi

failures=0
pad=''  # extra indent for rows nested under a heading
ok()   { printf '  %s%s✓%s %-40s %s%s%s\n' "$pad" "$G" "$N" "$1" "$D" "${2:-}" "$N"; }
bad()  { printf '  %s%s✗%s %-40s %s\n' "$pad" "$R" "$N" "$1" "${2:-}"; failures=$((failures + 1)); }
warn() { printf '  %s!%s %-40s %s\n' "$Y" "$N" "$1" "${2:-}"; }
title() { printf '\n%s%s%s\n' "$B" "$1" "$N"; }

env_value() { # env_value KEY DEFAULT — read KEY from the prod .env, trimmed
  local v=""
  [[ -f "$PROD_DIR/.env" ]] && v="$(grep -E "^$1=" "$PROD_DIR/.env" | tail -1 | cut -d= -f2- | tr -d '[:space:]')"
  printf '%s' "${v:-$2}"
}

probe() { # probe LABEL URL — HTTP 2xx within 8 s
  local code
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 8 "$2")"
  if [[ "$code" == 2* ]]; then ok "$1" "$2"; else bad "$1" "$2 → HTTP ${code/000/no answer}"; fi
}

printf '%sServer status%s  %s%s — up since %s%s\n' "$B" "$N" "$D" "$(date '+%F %T')" "$(uptime -s)" "$N"

title "systemd"
for unit in docker.service quant-modeling.service llama-bridge.socket; do
  state="$(systemctl is-active "$unit" 2>/dev/null)"
  if [[ "$state" == active ]]; then
    since="$(systemctl show "$unit" -p ActiveEnterTimestamp --value)"
    ok "$unit" "since ${since#* }"
  else
    bad "$unit" "$state — journalctl -b -u $unit"
  fi
done
# The ordering cycle that kept the site down at boot on 2026-09-25: a socket
# ordered both before sockets.target and after docker.service.
if systemctl show llama-bridge.socket -p Before --value 2>/dev/null | grep -qw sockets.target; then
  warn "llama-bridge.socket ordering" "boot cycle with docker — reinstall it from ~/AgenticEnv (RUNBOOK §5)"
fi

title "Docker stacks"
if ! docker info >/dev/null 2>&1; then
  bad "docker daemon" "unreachable"
else
  for project in "${PROJECTS[@]}"; do
    rows="$(docker ps -a --filter "label=com.docker.compose.project=$project" \
      --format '{{.Names}}|{{.State}}|{{.Status}}')"
    if [[ -z "$rows" ]]; then
      bad "$project" "no containers"
      continue
    fi
    printf '  %s%s%s\n' "$B" "$project" "$N"
    pad='  '
    while IFS='|' read -r name state status; do
      if [[ "$state" == running && "$status" != *unhealthy* && "$status" != *starting* ]]; then
        ok "$name" "$status"
      elif [[ "$state" == exited && "$status" == "Exited (0)"* ]]; then
        # One-shot init jobs (migrations, topic creation) finish and exit 0.
        printf '    %s· %-40s %s%s\n' "$D" "$name" "$status (one-shot)" "$N"
      else
        bad "$name" "$status"
      fi
    done <<<"$rows"
    pad=''
  done
fi

title "Endpoints"
probe "site (local)" "http://127.0.0.1:$(env_value QM_WEB_PORT 8091)/health"
probe "site (public, via tunnel)" "$(env_value QM_PUBLIC_URL https://tramonihadrien.com)/health"
probe "llama-server" "http://127.0.0.1:8000/health"
probe "llama-bridge (from containers)" "http://172.17.0.1:8001/health"

echo
if ((failures == 0)); then
  printf '%s%sAll up.%s\n' "$G" "$B" "$N"
else
  printf '%s%s%d problem(s).%s\n' "$R" "$B" "$failures" "$N"
fi
((failures == 0))
