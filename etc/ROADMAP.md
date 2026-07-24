# quant-modeling — Upgrade Roadmap

Goal: evolve the library from a "learning" codebase into a high-performance pricing library featuring
**QMC (Sobol)**, **variance reduction** (antithetic, control variates, stratified sampling, importance
sampling), **AAD greeks**, and **CUDA Monte Carlo** on 2× V100 — plus modern C++ (templates,
memory alignment, devirtualized hot loops).

---

## 0. Current State (audit summary)

| Area | Today | Gap |
|---|---|---|
| RNG | PCG32 + Box-Muller, antithetic wrapper | No Sobol/QMC, no Brownian bridge, Box-Muller not vectorizable (branchy, stateful) |
| Variance reduction | Antithetic only | No control variates, no stratification, no importance sampling, no moment matching |
| Greeks | Pathwise delta, LRM vega/rho, FD gamma/theta with CRN | No AAD; FD requires many reprices; pathwise delta noisy for discontinuous payoffs |
| Hot loops | `vol.value(S,t)` virtual call per step per path; some per-request allocations | Devirtualize via templates/CRTP; pre-allocate; SoA layouts; alignment |
| Build | `-g` only, no `-O3`, no CUDA | Add Release presets, `-O3 -march=native`, CUDA toolchain |
| Parallelism | Single-threaded MC | CPU threading (per-stream PCG already supports it), then CUDA |

---

## 1. Cross-Cutting Infrastructure (build once, reuse everywhere)

These are the shared components every instrument section below refers to.

### 1.1 QMC: Sobol sequences
- **Sobol generator** (Joe–Kuo direction numbers, up to ~21201 dimensions) with **scrambling**
  (Owen or random digit shift) so we get error bars back (randomized QMC, e.g. 32 independent shifts).
- **Inverse-normal transform** (Acklam/Beasley-Springer-Moro) instead of Box-Muller —
  required for QMC (preserves low-discrepancy structure) and branch-free, hence SIMD/CUDA friendly.
- **Dimension budget**: dimension = `n_steps × n_assets` (+1 per digital/barrier decision if smoothing needs it).
- **Effective-dimension reduction**:
  - **Brownian bridge** path construction: first Sobol coordinates drive the terminal value,
    subsequent ones refine — concentrates variance in the leading (well-distributed) dimensions.
  - **PCA construction** as an alternative for multi-asset (spectral decomposition of covariance).
- API: an `IPathGenerator`-style *compile-time* concept:
  ```cpp
  template <class Gen>
  concept GaussianSource = requires(Gen g, std::span<double> out) { g.fill(out); };
  // PseudoRandomSource<Pcg32>, SobolSource<BrownianBridge>, AntitheticSource<...>
  ```
  Engines become templates over the source → zero virtual calls in path generation.

### 1.2 Variance-reduction toolkit
- **Antithetic variates** — keep, but restructure as a path-level transform (not RNG-level state) so
  it composes with Sobol (antithetic is redundant with scrambled Sobol; must be selectable per run).
- **Control variates (CV)** — generic accumulator:
  `Y_cv = Y − β (X − E[X])`, with `β` estimated on a pilot batch (or analytically = Cov/Var),
  one or multiple controls (multi-CV via small OLS solve with Eigen).
- **Stratified sampling** — stratify the *terminal* Gaussian (or the first Brownian-bridge coordinate):
  split N paths into M equiprobable strata of Z_T, sample uniformly within strata
  (proportional allocation first; optimal Neyman allocation as a refinement).
- **Importance sampling (IS)** — Girsanov drift shift: simulate under drift `μ + θσ`, weight each
  path by the Radon–Nikodym likelihood ratio `exp(−θ Z − θ²T/2)` (per-step version for path-dependent).
  θ chosen by:
  - closed form for vanilla-like payoffs (shift so the payoff region is centered), or
  - a cheap pilot optimization (minimize second moment over θ — 1-D Brent search).
- **Moment matching** (cheap, optional): rescale sampled normals to exact mean 0 / var 1 per batch.
- **Conditional Monte Carlo** — analytic conditional expectation of the last step (used for barriers
  and digitals; see per-instrument sections).

### 1.3 AAD (adjoint algorithmic differentiation)
- Tape-based operator-overloading `Number` type (Savine-style, matching the book you'll study):
  - `Tape` with block memory pool (aligned arena, no per-node allocation),
  - `Number` records ops (adjoint = local derivative), reverse pass gives **all** sensitivities
    (delta, vega per point of vol surface, rho, dividend sensitivity) in **O(1) extra cost per path**.
  - Checkpointing per path: tape is rewound each path → constant memory.
- Engine templating: MC engines templated on scalar type `T ∈ {double, Number}` — the same code
  produces price (double) and greeks (Number). This is the main motivation for template refactor.
- **Smoothing for discontinuous payoffs** (digitals, barriers, autocall triggers): AAD needs
  a.e.-differentiable payoffs → replace indicators with tight call-spreads / sigmoid with width `ε ~ σS√dt`,
  or use conditional expectations (preferred, unbiased).
- Deliverables: `utils/aad/{tape.hpp, number.hpp}` + `T`-templated payoffs/models.

### 1.4 CUDA backend (2× V100)
- **Design**: one kernel per (instrument family × model), one **path per thread**, blocks of 256,
  grid sized to saturate 80 SMs; multi-GPU = split paths across devices, reduce on host.
- **RNG on device**: cuRAND **Philox4x32-10** (counter-based → no state transfer, perfectly
  reproducible per path index) — matches CPU results when we add a CPU Philox for cross-checking.
  Sobol: cuRAND `CURAND_DIRECTION_VECTORS_32` scrambled Sobol.
- **Memory**: SoA everywhere; market data (vol grid, curve nodes) in `__constant__` / texture memory
  (bilinear interpolation of the local-vol grid maps perfectly to 2-D texture fetches on V100).
- **Precision**: V100 has 1:2 FP64 ratio → keep FP64 for accumulation; experiment FP32 paths +
  FP64 Kahan/pairwise reduction (document the bias study in a notebook).
- **Reductions**: warp-shuffle + block reduction into per-block partials, final reduce on host
  (also accumulate sum of squares for std errors).
- Build: `enable_language(CUDA)`, `CMAKE_CUDA_ARCHITECTURES 70`, separate `quantModeling_cuda`
  target so CPU-only builds still work; runtime engine selection `EngineKind::MonteCarloGPU`.
- Priority order for kernels (best speedup/effort): **basket/rainbow/dispersion → autocall → local-vol
  paths → Asian/barrier/lookback → short-rate**.

### 1.5 Modern C++ refactor
- **Devirtualize hot loops**: template engines on `Model`, `Payoff`, `GaussianSource`
  (CRTP or plain templates + `if constexpr`); keep the virtual visitor layer only at the API boundary
  (one virtual call per *request*, zero per path/step).
- **Alignment & layout**:
  - `alignas(64)` accumulator structs (avoid false sharing when multithreaded),
  - SoA path buffers (`std::vector` with aligned allocator, or `std::assume_aligned`),
  - keep the existing eval-major layout from `short_rate.cpp` and generalize it.
- **Allocation discipline**: per-engine `Workspace` struct allocated once per request, reused across
  paths (already partly done in local_vol.cpp — make it the norm).
- **CPU parallel MC**: `std::jthread`/OpenMP over path chunks, per-thread PCG stream
  (RngFactory already supports stream ids), per-thread accumulators merged at the end.
- **Build hygiene**: CMake presets Release (`-O3 -march=native -DNDEBUG`), RelWithDebInfo,
  optional `-ffp-contract=fast`; keep `-Wall -Wextra -Wconversion`.
- Benchmarks: add `benchmarks/` with google-benchmark; every technique below gets a
  before/after entry (paths/sec and variance × time = efficiency).

### 1.6 Validation framework
- Every variance-reduction technique validated by: (a) unbiasedness vs analytic/high-N reference,
  (b) **efficiency ratio** = (Var₀ × t₀)/(Var₁ × t₁) reported in tests,
  (c) QMC convergence plots (error vs N in log-log, expect ~N⁻¹ vs N⁻½) in a notebook.
- GPU vs CPU cross-check with identical Philox streams (bitwise-comparable path values).

---

## 2. Per-Derivative Plan

Legend: **CV** control variate, **IS** importance sampling, **BB** Brownian bridge,
**CMC** conditional Monte Carlo, **LSMC** Longstaff–Schwartz.

### 2.1 European Vanilla (call/put)
The testbed — analytic truth available for everything.
- **QMC**: Sobol + inverse-normal (1-D → trivial, near machine-accuracy at low N). Reference case for the RQMC error-bar machinery.
- **IS**: Girsanov drift shift for deep OTM options — closed-form optimal shift θ ≈ solves
  `S₀ exp((r−q−σ²/2)T + σ√T θ) = K`. Demonstrate variance reduction of 10–100× for far OTM.
- **Stratified sampling**: stratify Z_T — near-perfect for monotone payoff (this is where stratification shines).
- **AAD**: full greeks (Δ, Γ via tape-of-tape or pathwise-LRM hybrid, vega, rho, dividend rho) —
  validate against closed-form BS greeks to 1e-10.
- **CUDA**: baseline kernel; use it to calibrate the reduction/multi-GPU plumbing.
- **C++**: becomes the templated-engine reference implementation (Model × Payoff × Source).

### 2.2 Digital (cash-or-nothing / asset-or-nothing)
Discontinuous payoff — the stress test for everything.
- **CMC / smoothing**: replace `1{S_T > K}` by its conditional expectation given the path up to T−dt:
  `Φ(d)` under BS — removes the discontinuity entirely → unbiased, huge variance reduction, AAD-compatible.
- **IS**: essential for OTM digitals (shift into the payoff region).
- **LRM vs pathwise**: pathwise delta doesn't exist (a.e. zero) — keep LRM, add AAD-on-smoothed and
  compare bias/variance of the three estimators in a test.
- **QMC**: works well only after smoothing (discontinuity destroys QMC rates) — document this.

### 2.3 Asian Option (arithmetic & geometric average)  ← flagship example
- **CV — geometric Asian** (the classic): geometric-average Asian has the closed-form
  Kemna–Vorst price. Use the *same paths* to compute the geometric payoff, then
  `price = MC_arith − β (MC_geom − KemnaVorst)`. Correlation ≈ 0.99+ → variance ÷ 50–1000.
  Add the analytic Kemna–Vorst engine (currently missing) as a standalone engine too.
- **Second CV option**: terminal `S_T` (E[S_T] known) as an extra control — multi-CV OLS.
- **QMC + BB**: dimension = n_fixings; Brownian bridge makes the average dominated by the first
  coordinates → Sobol convergence ~N⁻¹. This is *the* textbook QMC use case.
- **Stratification**: stratify the terminal point (first BB coordinate).
- **IS**: for OTM average strikes — drift shift applied to the whole path, per-step LR weights.
- **AAD**: pathwise Δ/vega through the average (smooth payoff → AAD directly applicable);
  vega bucketed per fixing date once local vol is in the picture.
- **CUDA**: fixings loop in registers, path per thread; geometric CV computed in the same kernel (free).
- **Target test**: arithmetic Asian, 30 fixings — show std error vs plain MC for
  {antithetic, CV, CV+QMC+BB, CV+QMC+BB+IS} in one table.

### 2.4 Barrier Options (knock-in/out)
- **BB / continuity correction**: discrete monitoring of a continuous barrier biases the price.
  Two fixes:
  1. **Brownian-bridge hitting probability**: between steps, knock out with probability
     `exp(−2 ln(B/S_i) ln(B/S_{i+1}) / (σ² dt))` → removes discretization bias, smooths the payoff
     (AAD- and QMC-friendly!). **Priority implementation.**
  2. Broadie–Glasserman–Kou barrier shift `B → B exp(±0.5826 σ√dt)` (cheap alternative, keep for comparison).
- **CMC**: for knock-out + terminal vanilla payoff: survival-probability-weighted conditional value.
- **CV**: vanilla option (analytic BS) as control — high correlation for far barriers;
  analytic barrier price (already have `analytic/black_scholes` barrier? add Merton/Reiner–Rubinstein
  closed forms as reference + control).
- **IS**: for far-barrier knock-ins (rare event!) — drift shift toward the barrier, LR weights.
  This is the canonical rare-event IS example.
- **AAD**: only through the BB-smoothed version (survival probabilities are smooth in S, B, σ).
- **CUDA**: per-step barrier check = branch-free `min/max` + survival-prob multiply — GPU-perfect.

### 2.5 Lookback (floating & fixed strike)
- **Discretization bias**: running max of discrete path underestimates the true max —
  apply the BB correction: sample the *bridge maximum* between steps
  (inverse-CDF of the Brownian-bridge max: `M = (a + b + sqrt((b−a)² − 2 dt σ² ln U))/2`).
- **CV**: analytic continuous lookback (Goldman–Sosin–Gatto closed form — add as analytic engine)
  and/or vanilla control.
- **QMC + BB** construction, stratify terminal.
- **AAD**: max is a.e. differentiable → pathwise works; subgradient at ties is measure-zero.
- **CUDA**: running max in registers; bridge-max sampling needs one extra uniform per step.

### 2.6 Basket Option (multi-asset)
- **CV — geometric basket**: geometric average of lognormals is lognormal → closed form.
  Same trick as the Asian: massive variance reduction for arithmetic baskets.
- **QMC + PCA**: dimension = n_assets × n_steps; use PCA of the covariance matrix (Eigen
  `SelfAdjointEigenSolver`) so leading Sobol dims carry the top eigen-modes.
- **Stratification**: stratify along the first principal component.
- **IS**: shift along the "payoff direction" (weighted-sum gradient) for OTM baskets.
- **AAD**: per-asset deltas + full correlation/vol sensitivities in one reverse pass —
  the case where AAD crushes bump-and-reprice (2n+n(n−1)/2 bumps → 1 sweep).
- **CUDA**: flagship GPU kernel — n_assets in registers/shared mem, Cholesky factor in
  `__constant__`; batch over strikes in the same kernel.
- **C++**: replace per-path Eigen temporaries with a pre-factorized `L` applied via hand-rolled
  aligned loop (n small); alignas(64) asset blocks.

### 2.7 Rainbow (best-of / worst-of)
- Same toolkit as basket (PCA-QMC, IS, AAD, CUDA).
- **CV**: individual vanillas (analytic) per asset, multi-CV OLS; for 2 assets, Stulz closed form
  (min/max of two lognormals) — add as analytic engine + exact reference.
- **AAD caution**: `min/max` kinks → a.e. fine for first-order; document Γ noise.

### 2.8 Autocallable Notes
The realistic structured product — everything combines here.
- **Smoothing mandatory**: autocall triggers = digital cliffs at each observation date. Apply CMC
  one step before each observation (conditional trigger probability `Φ(·)`) → smooth for AAD + QMC.
- **QMC + BB** across observation dates.
- **CV**: bond floor + strip of vanillas replication (approximate but highly correlated).
- **IS**: shift to balance early-redemption vs maturity scenarios.
- **CUDA**: per-thread state machine with precomputed trigger steps (pattern already exists in
  `short_rate.cpp`, reuse it).
- **AAD**: full vega map / delta / trigger-level sensitivities — the "why AAD exists" demo.

### 2.9 Variance & Dispersion Swaps
- Analytic replication (log-contract) already exists → use as **CV** for the MC engine.
- **QMC**: variance is a smooth functional of the path — QMC very effective.
- **AAD**: vega map w.r.t. local-vol grid nodes (adjoint through the σ(S,t) bilinear interpolation).
- **CUDA**: dispersion (n assets × n steps) is compute-heavy → priority kernel with basket.

### 2.10 American Vanilla (and Bermudan extension)
Currently binomial/trinomial only.
- **LSMC (Longstaff–Schwartz)**: new MC engine — regression of continuation value on polynomial
  (or Laguerre) basis; enables American under **local vol** and multi-asset Bermudan (impossible for trees).
  - Variance reduction: use the European price as CV; use the same paths for lower bound,
    add Andersen–Broadie duality upper bound later (stretch goal).
- **QMC**: works with LSMC (sort-based stratification of regression).
- **AAD**: differentiate through fixed regression coefficients (standard trick — coefficients frozen
  at optimum, envelope theorem) → greeks for American MC.
- **CUDA**: path generation on GPU; regression solve on host (or cuSOLVER batched QR).
- Keep trees as fast 1-D reference; fix `std::pow` per node → incremental `u/d` update.

### 2.11 Bonds, Caplets, Cap/Floors (Vasicek, CIR, Hull–White)
- **Exact simulation** where available: Vasicek/HW transition is Gaussian (exact step, no Euler bias);
  CIR via non-central χ² sampling (or Andersen QE) — removes step-size bias entirely.
- **CV**: analytic ZCB prices (affine closed forms) as controls for caplets/options.
- **QMC + BB** on the short-rate driver.
- **AAD**: sensitivities to curve nodes + model params (a, σ) in one sweep.
- Keep the eval-major buffer design — it is already the right layout; generalize it into the
  shared `Workspace` abstraction and reuse on GPU.

### 2.12 FX & Commodity Options
- Garman–Kohlhagen ≡ BS with two carry rates → inherit *everything* from vanilla (2.1) once
  engines are templated on the model (this is the payoff of the template refactor: zero new MC code).
- Commodity convenience yield same story.

---

## 3. Suggested Implementation Order

| Phase | Content | Depends on |
|---|---|---|
| **1. Foundations** | Release build flags + benchmarks; inverse-normal transform; template/devirtualize the vanilla MC engine (Model × Payoff × Source); Workspace allocation pattern | — |
| **2. QMC** | Sobol (+ scrambling), Brownian bridge, PCA construction; wire into vanilla + Asian | 1 |
| **3. Variance reduction** | CV framework (geometric Asian, geometric basket, vanilla-for-barrier), stratified sampling, IS (vanilla OTM, knock-in rare event), BB barrier survival probabilities, bridge-max for lookback, CMC for digitals/autocall | 1–2 |
| **4. CPU parallel MC** | jthread/OpenMP path chunking, per-stream RNG, aligned per-thread accumulators | 1 |
| **5. AAD** | Tape + Number, T-templated engines, smoothed payoffs, validate vs analytic greeks; roll out vanilla → Asian → basket → autocall | 1 (do after reading the book; design engines AAD-ready from phase 1) |
| **6. CUDA** | Toolchain + Philox parity, vanilla kernel, then basket/rainbow/dispersion, autocall, local-vol texture kernel; multi-GPU split | 1–3 |
| **7. LSMC** | American/Bermudan MC, CV with European, GPU path-gen | 2–4 |
| **8. Extras** | Andersen–Broadie upper bound, QE scheme for CIR/Heston(?), mixed-precision GPU study | 6–7 |

Each phase = benchmark entry + unit test proving unbiasedness + efficiency ratio.

---

## 4. New Files (sketch)

```
include/quantModeling/utils/
    sobol.hpp              # direction numbers, scrambling
    inverse_normal.hpp     # branch-free Φ⁻¹
    brownian_bridge.hpp    # BB + PCA path construction
    philox.hpp             # counter-based RNG (CPU mirror of cuRAND)
    aad/tape.hpp, aad/number.hpp
utils/variance_reduction/
    control_variate.hpp    # generic multi-CV accumulator (pilot β)
    stratified.hpp         # strata allocation + within-stratum sampling
    importance.hpp         # drift shift + LR weights
engines/mc/kernels/        # templated path kernels shared CPU/GPU (host/device functions)
cuda/                      # .cu kernels, reductions, multi-GPU driver
benchmarks/                # google-benchmark harness
```
