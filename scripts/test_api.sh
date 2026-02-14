#!/usr/bin/env bash
set -euo pipefail

BASE_URL="${1:-http://localhost:8000}"

curl -s "$BASE_URL/health" | cat

echo "\n--- Vanilla analytic (call) ---"
curl -s "$BASE_URL/price/vanilla" \
  -H "Content-Type: application/json" \
  -d '{
    "spot": 100,
    "strike": 100,
    "maturity": 1.0,
    "rate": 0.05,
    "dividend": 0.02,
    "vol": 0.20,
    "is_call": true,
    "engine": "analytic"
  }' | cat

echo "\n--- Vanilla MC (put) ---"
curl -s "$BASE_URL/price/vanilla" \
  -H "Content-Type: application/json" \
  -d '{
    "spot": 100,
    "strike": 100,
    "maturity": 1.0,
    "rate": 0.05,
    "dividend": 0.02,
    "vol": 0.20,
    "is_call": false,
    "engine": "mc",
    "n_paths": 200000,
    "seed": 42
  }' | cat

echo "\n--- Asian arithmetic analytic (call) ---"
curl -s "$BASE_URL/price/asian" \
  -H "Content-Type: application/json" \
  -d '{
    "spot": 100,
    "strike": 100,
    "maturity": 1.0,
    "rate": 0.05,
    "dividend": 0.02,
    "vol": 0.20,
    "is_call": true,
    "average_type": "arithmetic",
    "engine": "analytic"
  }' | cat

echo "\n--- Asian geometric MC (call) ---"
curl -s "$BASE_URL/price/asian" \
  -H "Content-Type: application/json" \
  -d '{
    "spot": 100,
    "strike": 100,
    "maturity": 1.0,
    "rate": 0.05,
    "dividend": 0.02,
    "vol": 0.20,
    "is_call": true,
    "average_type": "geometric",
    "engine": "mc",
    "n_paths": 200000,
    "seed": 42
  }' | cat

echo
