# Quant Modeling

A derivatives-pricing library written the way a desk would build one — C++20
core, exposed all the way to a usable product — built solo, end to end:
pricing engine, API, and front end.

[![CI](https://github.com/HadrienT/quant-modeling/actions/workflows/ci.yml/badge.svg)](https://github.com/HadrienT/quant-modeling/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![React 19](https://img.shields.io/badge/React-19-61DAFB?logo=react&logoColor=black)
![Python](https://img.shields.io/badge/Python-FastAPI-009688?logo=fastapi&logoColor=white)

**[tramonihadrien.com](https://tramonihadrien.com)** — live demo, self-hosted
(no cloud).

---

## What this is

`include/quantModeling` + `src` is a C++20 pricing library, split the way a
desk library is split: **instruments** (payoffs) know nothing about market
data; **models** know nothing about how they're solved; **engines**
(analytic, tree, PDE, Monte Carlo) know nothing about which product they're
pricing; a **pricer registry** wires the three together per product. No
engine branches on a product type; no instrument reads market data.

That library is then carried all the way to something a non-engineer can use:

```
C++20 core  →  pybind11 bindings  →  FastAPI service  →  React front end
```

Every layer is real, tested, and in the same repo: ~43k lines of C++ behind
~70 GoogleTest suites (plus an AddressSanitizer/UBSan build), a typed Python
API with its own pytest suite, and a React app whose types are generated from
the API's OpenAPI schema — with CI that fails the build on drift.

## Screenshots

**Pricing workbench** — pick a product, an engine, a model; get the price,
the full Greek profile, and a payoff chart, side by side.

![Pricing workbench](.github/readme/pricing-workbench.png)

**Product catalog** — every instrument documented with its payoff formula,
model assumptions, and which numerical methods price it (and why they should
agree).

![Product catalog](.github/readme/product-catalog.png)

**Scripting assistant** — describe a payoff in plain language; a local LLM
drafts it in the project's own payoff DSL and checks the draft against the
real parser before showing it to you. No made-up syntax reaches the screen.

![Scripting assistant](.github/readme/scripting-assistant.png)

## Highlights

- **~20 instruments** across vanilla, exotics (Asian, barrier, digital,
  lookback, basket, rainbow), structured (autocall, mountain/Himalaya),
  volatility (variance/volatility/dispersion swaps), rates, FX and commodity —
  deliberately not growing further; the roadmap is depth, not a 21st payoff.
- **A dozen-plus models**: Black-Scholes, Heston, Bates, SABR (+ SABR-PDE),
  Dupire local vol (calibrated from an SVI surface via Levenberg-Marquardt),
  rough Bergomi, Merton and Kou jump-diffusion, Hull-White / Vasicek / CIR
  short-rate, Garman-Kohlhagen FX.
- **Four engine families** sharing one interface: closed-form analytic,
  binomial/trinomial trees, Crank-Nicolson PDE, and Monte Carlo (Heston QE,
  Heston COS, a rough Bergomi hybrid scheme) with antithetic variates,
  control variates, Sobol sequences and a Brownian bridge.
- **Calibrated on real data**: SVI per maturity and Dupire local vol from the
  stored option chains; Heston fitted to that surface (COS pricing,
  Levenberg-Marquardt, exact implied-vol error reported); stochastic-local vol
  whose leverage (particle method) makes Heston reprice every vanilla.
- **A payoff scripting language** (lexer → parser → AST → fuzzy-logic
  evaluator, Andreasen–Savine style): one generic Monte-Carlo engine prices
  *any* scripted payoff instead of a dedicated code path per product. The
  model is read off the script (local vol for payoffs on each date's spot,
  SLV for path-dependent ones, correlated Black-Scholes for several
  underlyings), announced, and every dynamics can be compared on one seed. A
  frozen library of 42 structures from Bouzoubaa & Osseiran is priced from a
  term sheet, each with a computed long/short risk profile.
- **Adjoint algorithmic differentiation**: an operator-overloading tape
  (`Number`, checkpointing, a parallel-AAD path) computing full Greek vectors
  in roughly the cost of one extra valuation, instead of bumping every input
  — carried through the calibration (Dupire on the tape, the SVI fits by the
  implicit function theorem) to **the vega of any scripted product to each
  quoted option of the chain**.
- **Property-based tests over exact-number tests**: call-put parity,
  `in + out = vanilla`, monotonicity bounds, measured log-log convergence
  order — not just "this number equals that number."

## Monte-Carlo on two GPUs

Payoff scripts compile to a stack-machine bytecode that runs unchanged on the
CPU and in a CUDA kernel on two Tesla V100s, with prices and model risks
(forward-mode duals, a hand-written per-path adjoint for local vol). Every
path is a pure function of (seed, path index) and every reduction follows the
path indices, so **one GPU, two GPUs and the CPU give the same number** — bit
for bit between GPU counts, to 1e-12 against the CPU. Wall time to a 1e-4
relative standard error (1e-3 on the delta for the superbucket), the CPU
columns running the kernels' own per-path code
(`build-cuda/qm_gpu_table_bench`; `*` = timed on a fraction and scaled):

| Pseudo-random | Paths | CPU 1 thread | CPU 8 threads | 1 V100 | 2 V100 |
|---|---|---|---|---|---|
| Vanilla Black-Scholes | 1.25e8 | 8.9 s | 1.3 s | 28 ms | 17 ms |
| Up-and-out, daily, local vol | 1.55e8 | 169 min * | 27 min * | 25.2 s | 12.7 s |
| Worst-of autocall, 3 assets | 3.8e6 | 10.3 s | 1.5 s | 41 ms | 34 ms |
| Superbucket (363 local-vol risks) | 8.5e5 | 51.5 s * | 7.5 s | 237 ms | 237 ms |

| Sobol RQMC + Brownian bridge | Paths | CPU 1 thread | CPU 8 threads | 1 V100 | 2 V100 |
|---|---|---|---|---|---|
| Vanilla Black-Scholes | 1.3e5 | 17 ms | 3 ms | 5 ms | 6 ms |
| Up-and-out, daily, local vol | 3.4e7 | 35 min * | 291 s * | 4.5 s | 2.3 s |
| Worst-of autocall, 3 assets | 1.0e6 | 2.8 s | 478 ms | 10 ms | 9 ms |
| Superbucket (363 local-vol risks) | 1.6e4 | 695 ms | 119 ms | 13 ms | 14 ms |

A card joins a run only with enough work (128 logical blocks) to pay for its
start-up; small runs stay on one. Design, measurements and decisions:
[`blueprint/wp/19-gpu.md`](blueprint/wp/19-gpu.md).

## Stack

| Layer | Tech |
|---|---|
| Pricing core | C++20, CMake, vcpkg, Eigen, GoogleTest, Google Benchmark, ASan/UBSan |
| Bindings | pybind11 → a `quantmodeling` wheel |
| API | FastAPI, Pydantic, PostgreSQL, JWT + Google OAuth (PKCE) |
| Front end | React 19, TypeScript, Vite, TanStack Query/Router/Table, Zustand, Tailwind, react-three-fiber (WebGL vol surfaces), CodeMirror, Storybook, Vitest, Playwright |
| Ops | Docker Compose, GitHub Actions, self-hosted behind a Cloudflare Tunnel (no cloud provider) |

## Quality bar

- **C++**: every new engine or instrument ships with a test — ideally a
  property test, not a single numeric equality — and passes an
  AddressSanitizer + UBSan build, not just the release one.
- **Contract-checked API**: the OpenAPI schema and the generated TypeScript
  client are committed; CI regenerates both and fails on any diff.
- **Front-end discipline enforced by lint, not convention**: features can't
  import each other, `shared/` can't import a feature, and no feature file
  exceeds 230 lines — checked by a dedicated test, not a code-review habit.
- **An internal engineering log**: [`CLAUDE.md`](CLAUDE.md) documents the
  actual conventions this repo runs on (build commands, coding conventions,
  deployment model) — written for an AI pair-programming agent, but it's the
  same document a human contributor would need.

## Running it

The C++ core is fully self-contained and builds standalone:

```bash
git clone https://github.com/HadrienT/quant-modeling.git
cd quant-modeling
cmake --preset default && cmake --build build
ctest --test-dir build --output-on-failure
```

The API and front end are real too, but market data (spot, option chains,
FRED curves) comes from a separate `data-ingest` Postgres this public repo
doesn't publish — `docker compose up --build` needs `PGPASSWORD` pointed at
one to fully come up. The live demo above is the fastest way to see the whole
stack running end to end; the full command reference (wheel build, sanitizer
preset, Python tests, OpenAPI regeneration) is in [`CLAUDE.md`](CLAUDE.md).

## License

[MIT](LICENSE)
