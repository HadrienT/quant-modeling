# quant-modeling — notes for Claude Code

Derivatives pricing library in **C++20**, exposed all the way to a usable product:
`include/quantModeling` + `src` → **pybind11** bindings → **FastAPI** API →
**React/Vite** front end. ~43,000 LOC of C++ (`include/` + `src/`), ~70 GoogleTest
files, GitHub Actions CI.

The C++ split is that of a desk library and must be preserved: **payoff**
(`instruments/`), **model** (`models/`), **numerical method** (`engines/`),
**orchestration** (`pricers/` with registry + adapters). No engine may branch on a
product type; no instrument may read market data.

## The chain, layer by layer

| Layer | Where | What it contains |
|---|---|---|
| C++ core | `include/quantModeling/`, `src/` | `core` (types, timegrid, results), `market`, `instruments`, `models`, `engines` (analytic / tree / pde / mc / xva: single-currency exposure engine, on CPU or on both V100s, and two-currency or rate-plus-equity on CPU), `pricers`, `risk` (xVA, WP 23: exposure metrics, netting and profiles per netting set, collateral, CVA / DVA / FVA / MVA / KVA integrals, regulatory capital in `risk/regulatory/`), `utils` (Sobol, Brownian bridge, control variates, greeks) |
| Bindings | `bindings/python/` | pybind11 → `quantmodeling` wheel |
| API | `api/app/` | FastAPI: `routers/` (pricing, market, credit, rates, xva, portfolio, backtest, auth, local-vol, assistant, admin), `credit.py` + `fundamentals.py` (spreads, hazard, Merton, 10-K / 10-Q statements), `xva.py` (exposure, CVA / DVA of a netting set on market data, WP 23), `portfolio_counterparties.py` + `portfolio_rates.py` + `portfolio_xva.py` + `xva_equity.py` (counterparties and netting sets of a portfolio, daily marking of its swaps, swaptions, forwards and currency swaps, xVA of one of its netting sets, equity options included; WP 23 §14.16), `xva_fx.py` + `fx_option_market.py` (second currency of the netting set: EUR curve and Hull-White, EUR/USD vol from traded straddles, historical correlations), `swaption_market.py` (ATM normal vols from DTCC swaption trades, to fit Hull-White to the market), `local_vol/` (quote cleaning, IV surface, Dupire), `assistant/` (scripting LLM chat, see below), `audit/` (audit events to the `quant-platform` Kafka), `backtest/` (point-in-time reads, calls to the `quantbacktest` wheel, `backtest.run` event and its replay: no computation, see WP 24), `valuation.py` + `replay.py` (recording and replay of valuations), `telemetry.py` (OpenTelemetry), JWT auth, cache |
| Front end | `web/src/` | React 18 + Vite + TypeScript |
| CLI | `main.cpp` | demo binary |

## Neighboring repositories

This repository is not alone on the server; four sibling repositories, each with its own
`CLAUDE.md`, hold what is not pricing:

| Repository | Role for quant-modeling |
|---|---|
| `~/data-ingest` | **All market data ingestion** (Airflow → Postgres). quant-modeling only reads this database (`PGHOST=data-ingest-postgres` through the Docker network `dataplatform`). Missing data is fixed there, not here |
| `~/quant-platform` ([GitHub](https://github.com/HadrienT/quant-platform)) | **Destination of logs, audit events and telemetry**: Kafka, append-only audit Postgres, OpenTelemetry Collector → Prometheus / Loki / Tempo, Grafana. Producer ↔ platform contract: `~/quant-platform/docs/contract.md`. On this side, the producer is `api/app/audit/` (`emit()`, `record_fallback()`), design in `blueprint/wp/18-observability.md` |
| `~/AgenticEnv` | `llama-server` for the scripting assistant (see below) |
| `~/quant-backtest` ([GitHub](https://github.com/HadrienT/quant-backtest)) | **Backtest engine** in C++, shipped as the `quantbacktest` wheel. It consumes the `quantModeling` library at a pinned commit; only the database reads, the router, the audit and the `/backtest` page remain here (`api/app/backtest/`, `web/src/features/backtest/`; strategy tab live, hedging and risk model to do, `blueprint/wp/24-backtest.md`). Boundary: `~/quant-backtest/docs/contract.md`. No backtest computation in this repository |

Connection to `quant-platform` (WP 18, done): in **production**, audit events go to its
Kafka (`QM_AUDIT_SINK=kafka`, `kafka:9092`) and metrics / traces to its OTel Collector
(`otel-collector:4318`), through the `dataplatform` network; if the platform is absent,
events wait in `LOG_DIR/spool` and leave on their own, and the site never depends on it.
In **dev**, the transport stays the spool: `dataplatform` leads to the **production**
platform, whose audit database is append-only, so never send a test event there (use a
disposable broker for experiments). Each pricing emits a `pricing.valuation`
(`api/app/valuation.py`) that `POST /api/admin/replay/{id}` can replay. Any new log or
business event goes through `api/app/audit/` (`emit()`, `record_fallback()`), any new
metric through `api/app/telemetry.py`; never a ticker, a user or an IP as a label.
Application logs go to stdout and to `LOG_DIR/api.jsonl` (`api/app/logging_utils.py`), with
the `trace_id`. Operations: `deploy/RUNBOOK.md` §9.

## Market data

Market data comes from **the Postgres database fed by `~/data-ingest`** (prices, option
chains, dividend yields, FRED curves, ICE BofA credit spreads, XBRL facts from S&P 500
10-Ks / 10-Qs, traded swaps and swaptions published by DTCC (SOFR, €STR, Euribor), traded
FX options published by DTCC, historical default rates by rating published by ESMA). **Any
new data path goes through the database, with no fallback to Yahoo Finance**: data that is
absent or too old there is an explicit error (script pricing paths go through
`api/app/market_snapshot.py`, which does not import `yfinance`). The live fallbacks
**already in place** in the older endpoints (`vol_surface.py`,
`routers/local_vol_pricing.py`, `routers/simulation.py`) are **kept on purpose**, so as not
to break the API while `data-ingest` is not reliable: do not extend them, do not remove
them without a decision from the maintainer. The `notebooks/` directory is a personal
sandbox, out of scope. No more BigQuery: the project is entirely off cloud.

## Scripting assistant

**Scripting assistant** (`api/app/assistant/`, route `POST /api/assistant/scripting/chat`,
chat of the `/scripting` page): it talks to the `llama-server` of `~/AgenticEnv`, whose URL
is `QM_LLM_BASE_URL` (default `http://127.0.0.1:8000/v1` outside Docker;
`http://172.17.0.1:8001/v1` in the compose files, through AgenticEnv's `llama-bridge`
socket). Production is public and the route consumes the GPU: a visitor who is not logged
in gets **10 questions per day** (`assistant/quota.py`, in-memory counter per IP hash, plus
a global cap; `QM_ASSISTANT_ANON_DAILY` and `QM_ASSISTANT_ANON_DAILY_TOTAL`), after which
the route answers 429 with the code `anonymous_quota_exhausted` and the page suggests
logging in. A logged-in user (JWT) is not counted, and one of the two slots is reserved for
them. Any script written by the model is checked by the real parser before being offered.
`GET /api/assistant/status` (public, cached 15 s) says whether the server responds and with
which model: the page shows it, because the `llama-server` is shared and can be stopped or
loaded with another model. The prompt describes the language: **when the language changes
(new function, new syntax), update `assistant/prompt.py`**. `pytest` validates its examples
against the parser and fails if they no longer pass.

## Commands

| | |
|---|---|
| `cmake --preset default && cmake --build build` | C++ build (Ninja, Release, tests ON) |
| `ctest --test-dir build --output-on-failure -j16` | C++ tests, or `scripts/make.sh`, which chains all three. Each `TEST()` runs in its own process (`gtest_discover_tests`), so `-j` parallelizes with no shared state; 16 measured at ~1.7 GB above what already runs on the machine (`NPROC=n scripts/make.sh` to change) |
| `cmake --preset asan` then `ctest --test-dir build-asan -j8` | sanitizers (address + UB), or `scripts/make_asan.sh`. ASan roughly triples memory per process; 8 measured at ~8.4 GB above the baseline, a more significant share of the margin than with the default preset (`NPROC=n scripts/make_asan.sh` to change) |
| `cmake --preset release` | `-march=native` + google-benchmark benchmarks |
| `build/qm_xva_aad_bench [paths] [threads]` | cost of CVA sensitivities by adjoint differentiation against one valuation (WP 23, work package X8) |
| `cmake --preset cuda && cmake --build build-cuda` | CUDA backend (WP 19, both V100s) in `build-cuda/`; nvcc uses `g++-13` as host (CUDA 12.4 rejects gcc 14). `ctest --test-dir build-cuda -L gpu` for the GPU tests only (CI has no GPU and does not build them); `build-cuda/qm_gpu_bench` for the time-to-error benchmark, `qm_gpu_vr_bench` for generic variance reduction, `qm_gpu_risk_bench` for GPU risks against the CPU tape (work package G3), `qm_gpu_exposure_bench` for the exposure and CVA of a netting set (WP 23, work package X7). On the Python side: `QM_ENABLE_CUDA=1 scripts/run_api.sh` (or `scripts/dev_install.sh`, `scripts/build_wheel.sh`) for a module with GPU; the production image does so by default (`QM_CUDA=ON`) |
| `scripts/build_wheel.sh` | pybind11 wheel in `dist/` |
| `scripts/install_quantbacktest.sh` | builds and installs the `quantbacktest` wheel (backtest engine, repository `~/quant-backtest`) at the commit pinned in `api/quantbacktest.pin`; no effect if that commit is already installed. Also read by both Docker images and by CI: **bumping the engine version means changing this one file**. `NPROC=n` bounds the compilations (16 by default) |
| `scripts/run_api.sh` | venv + wheel + pinned `quantbacktest` wheel + `uvicorn --reload` |
| `pytest` | Python tests of the API (`api/tests/`: scripting assistant with the real parser and a fake LLM, market snapshot, audit trail, Kafka producer on a fake broker, valuation replay), from the root (`pytest.ini`). Run by CI (`pytest` job, with the real wheel) |
| `uv venv .venv-ml && uv pip install --python .venv-ml/bin/python -r ml/requirements.txt`, then `.venv-ml/bin/python -m pytest ml/tests` | training side of the ML project (WP 25): CPU PyTorch, in a separate environment, never in the API's or the production image. The C++ only reads the weight files (`ml::Mlp`). CI job `pytest-ml`. `cd ml && ../.venv-ml/bin/python -m qmml.make_fixture ../tests/data` regenerates the set of cases shared with `tests/testMlp.cpp` |
| `docker compose up --build` | full stack (API + front end with HMR) |
| `cd web && npm run dev` | front end only |
| `cd web && npx tsc --noEmit` | front-end typecheck |
| `cd web && npm run lint` | eslint |
| `cd web && npm run build` | Vite production build |
| `python scripts/gen_openapi.py web/openapi.json && cd web && npm run api:types:local` | regenerates `web/openapi.json` and `schema.gen.ts` after an API route/schema change (ADR-005). `gen_openapi.py` stubs the pybind11 module, so no running API is needed. Both files are committed; the CI job `api-contract` fails if the diff is not empty |

The `docker compose` host ports are configurable, `QM_API_PORT` (default 8010) and
`QM_WEB_PORT` (default 5180), because several projects run on the same self-hosted server.
Copy `.env.placeholder` to `.env`.

## Tracking work: GitHub Issues, no handoff markdown

The project's "JIRA" is the repository's **GitHub Issues**. `gh` is authenticated (account
`HadrienT`).

- **At the start of a session**: `gh issue list --state open`.
- **Issue handled** → `gh issue close <n> --comment "done in <sha>"`.
- **Never** a markdown handoff file between sessions. A task that outlives the session is
  an issue.
- Large pieces of **design** live in `blueprint/wp/*.md`; the issue points to them, it does
  not replace them.
- `blueprint/README.md` holds the dependency graph between work packages: consult it before
  starting a work package, to check that its prerequisites are done.

## Reference documents

| File | Role |
|---|---|
| `blueprint/` | Specification of the work packages: the front-end rewrite (WP 00–14, merged into `main`), then the C++ core projects (WP 16 scripting, WP 17 AAD). Dependencies, acceptance criteria. |
| `etc/roadmap.md` | Six-month quant strategy: calibrated stochastic vol, AAD, GPU Monte Carlo, xVA capstone. A strategy document, not a spec. |
| `etc/todo.md` | "Desk grade" checklist: what is missing to resemble a production library. |
| `etc/structure.md` | Target layout of the pricing module. |
| `etc/bytecode-aad-gpu-ordre-2.md` | Detailed explanation of what goes beyond Savine's two books: script bytecode, AAD on GPU (duals, per-path adjoint without a tape), second-order AAD. Keep it up to date when this code changes. |

**Roadmap rule not to be bypassed: stop adding products.** The catalogue is already wider
than necessary; the marginal value of a 25th payoff is zero. Depth (calibration, adjoint
greeks, GPU) beats breadth. One exception, decided by the maintainer on 2026-10-03: the
currency swap of work package X10 (WP 23 §14.15).

## Conventions

- **Language: English by default.** Everything you produce is in English: code, identifiers, comments, commit messages, PRs, issues, documentation and interface text. The only exception is the conversation: reply in the language the maintainer writes in, so in French when they write in French. `blueprint/` and `etc/` are historically in French: new content there is in English too.
- **Commits: go through a branch, never directly on `main`.** End messages with
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
- **C++**: `clang-format --style=file`, **version 18** (the one used by CI, job
  `format-check`; pre-commit is pinned to the same). A badly formatted file fails CI; the
  pre-commit hook (below) prevents it. To reformat everything by hand:
  `pre-commit run --all-files`. **Python**: `black`.
- **The pre-commit hook is installed on the server** (`.git/hooks/pre-commit`, shared by
  both worktrees): on each `git commit`, clang-format 18 and black reformat the staged files.
  If it changed anything, the commit **stops** ("files were modified by this hook"): run
  `git add` on the same files again, then `git commit`, that is all. Never bypass it with
  `--no-verify`. On another clone: `uv tool install pre-commit && pre-commit install -t pre-commit`.
- The pre-push stage of `.pre-commit-config.yaml` (`cmake + ctest`) **is not installed**:
  nothing stops a push with red tests except CI. Run `scripts/make.sh` before pushing C++.
- Any new engine or instrument comes with its test in `tests/`, preferably a *property* test
  (call-put parity, `in + out = vanilla`, monotonicity bounds, convergence order measured in
  log-log) rather than a simple numeric equality.
- The rewritten front end is in `main`; the old code is gone (`web/src/` = `app/`,
  `features/`, `shared/`). Rules checked by ESLint and `web/tests/discipline.test.ts`: a
  feature does not import another feature, `shared/` imports neither `features/` nor `app/`,
  a file in `features/` does not exceed 230 lines (extract a component rather than raising
  the cap).

## Deployment

The project is **self-hosted**, not on Google Cloud: GCP's cost was the reason for the move.
The target: `docker-compose.prod.yml` on the personal server, exposed over HTTPS by a
**Cloudflare tunnel** (no open port).

Two directories, one git repository (`git worktree`): `~/quant-modeling` is **development**
(branches, commits, PRs; this is where Claude sessions work); `~/quant-modeling-prod` is
**the live site, always on `main`**, and is only used for `./scripts/deploy.sh` (which does
the `git pull` of `main`, rebuilds the image and restarts). Normal flow: branch → PR → merge
into `main` → deploy from the production directory. There is no deployment branch any more
(`web/14-deploy` and `web/rewrite` were deleted). Never edit or commit from
`~/quant-modeling-prod`. Full procedure in `deploy/RUNBOOK.md`, design in
`blueprint/wp/14-deploy-selfhost.md`. `web/Dockerfile.prod` is the foundation (multi-stage:
C++ wheel → Vite build → nginx + uvicorn).

Beware of the classic Vite trap: `VITE_*` variables are **inlined at build time**, not read
at runtime. Changing `VITE_API_BASE` requires rebuilding the image, or going through the
runtime injection mechanism described in the deployment work package.
