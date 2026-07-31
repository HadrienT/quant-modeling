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

Each section is structured as **Payoff → Numéraire/measure → Closed form → Market conventions →
Numerics plan**. The formulas are the ones we implement (or will implement) as the analytic
reference and control variate for the corresponding MC engine.

### 2.0 Common framework (applies to every section)

**Fundamental theorem.** For any numéraire $N_t>0$ and its associated measure $\mathbb{Q}^N$,
every tradable satisfies

$$V_t = N_t\,\mathbb{E}^{\mathbb{Q}^N}\!\left[\frac{V_T}{N_T}\,\middle|\,\mathcal F_t\right].$$

Choosing the right numéraire is what turns most of the closed forms below into one-liners.

| Numéraire | Measure | Martingale under it | Used for |
|---|---|---|---|
| Bank account $B_t=e^{\int_0^t r_s ds}$ | risk-neutral $\mathbb{Q}$ | discounted tradables $V_t/B_t$ | MC path generation (our engines) |
| Zero-coupon $P(t,T)$ | $T$-forward $\mathbb{Q}^T$ | forward $F(t,T)$, forward rate $L_i$ | any payoff paid at $T$, caplets |
| Share account $S_te^{qt}$ | share measure $\mathbb{Q}^S$ | inverse quotes $K/S_t$ | asset-or-nothing, Margrabe, FX symmetry |
| Annuity $A(t)=\sum_i\tau_iP(t,T_i)$ | swap measure $\mathbb{Q}^A$ | swap rate $S_{n,m}(t)$ | swaptions |
| Foreign bank account | foreign measure $\mathbb{Q}^f$ | $1/S^{d/f}_t$ | FX symmetry, quanto |

**Deterministic-rates simplification.** With deterministic $r,q$ (the world we live in for equity
exotics), $\mathbb{Q}$ and $\mathbb{Q}^T$ differ only by a deterministic factor, so every European
price is $P(0,T)\,\mathbb{E}^{T}[\text{payoff}]$ with $F_0=S_0e^{(r-q)T}$. All closed forms below are
written **Black-76 style on the forward** — what desks actually use, because it separates
discounting from the forward and generalises unchanged to FX, futures and rates.

**Discounting (post-2008 standard).** Multi-curve: discount at the **collateral (CSA) rate** —
SOFR / €STR / SONIA OIS — and take forwards from a separate source (listed futures, repo and
dividend curves for equity; forward points for FX). Our `DiscountCurve` *is* the CSA curve, and `q`
should be read as a "repo + dividend yield" continuous proxy, not as a physical dividend yield.

**Volatility.** The market quotes implied vols, never prices. Standard exotic pricing chain:
calibrate an arbitrage-free surface (SVI/SSVI on equity, SABR on rates, vanna–volga or SABR on FX)
→ **Dupire local vol**

$$\sigma_{\text{loc}}^2(K,T)=\frac{\partial_T C+(r-q)K\,\partial_K C+qC}{\tfrac12K^2\,\partial^2_{KK}C}$$

→ **LSV** (local-stochastic vol) for path-dependent, skew-sensitive books (barriers, autocalls),
because pure local vol misprices forward-smile dynamics.

### 2.1 European Vanilla (call/put)
The testbed — analytic truth available for everything.

**Payoff** (settled at $T$): $\Phi(S_T)=\big(\phi(S_T-K)\big)^+$, $\phi=+1$ call, $\phi=-1$ put.

**Numéraire**: $T$-forward. $F_t=F(t,T)$ is a $\mathbb{Q}^T$-martingale, $dF_t/F_t=\sigma\,dW^T_t$, so
$V_0=P(0,T)\,\mathbb{E}^{T}\big[(\phi(F_T-K))^+\big]$.

**Closed form (Black-76 / Black–Scholes)**

$$V_0=\phi\,P(0,T)\big[F_0N(\phi d_1)-KN(\phi d_2)\big],\qquad
d_{1,2}=\frac{\ln(F_0/K)\pm\tfrac12\sigma^2T}{\sigma\sqrt T},$$

with $F_0=S_0e^{(r-q)T}$, $P(0,T)=e^{-rT}$ — collapses to the textbook spot form.
**Parity** $C-P=P(0,T)(F_0-K)$ must hold to machine precision in tests.
**Greeks** (spot convention): $\Delta=\phi e^{-qT}N(\phi d_1)$, $\Gamma=e^{-qT}n(d_1)/(S_0\sigma\sqrt T)$,
$\mathcal V=S_0e^{-qT}n(d_1)\sqrt T$ (per 1.0 of vol; ÷100 for the quoted "per vol point").

**Exact simulation** (no discretisation, one Gaussian per path):
$S_T=F_0\exp\!\big(-\tfrac12\sigma^2T+\sigma\sqrt T\,Z\big)$.

**Market conventions**: index options are European, cash-settled; single-stock listed options are
American with **discrete cash dividends** (desk standard: escrowed-dividend / Bos–Vandermark spot
adjustment, not a continuous $q$). OTC quotes are in implied vol against a forward-ATM strike.

Numerics plan:
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

**Payoff**: cash-or-nothing $Q\,\mathbf 1\{\phi(S_T-K)>0\}$; asset-or-nothing $S_T\,\mathbf 1\{\phi(S_T-K)>0\}$.
Identity: $\text{AoN}-K\cdot\text{CoN}_{Q=1}=\text{vanilla}$.

**Numéraire — the textbook change-of-numéraire pair.** Both digitals price the *same event* under
two different numéraires:

$$\text{CoN}=Q\,P(0,T)\,\mathbb{Q}^T\big(\phi(F_T-K)>0\big)=Q\,P(0,T)\,N(\phi d_2),$$

$$\text{AoN}=S_0e^{-qT}\,\mathbb{Q}^{S}\big(\phi(S_T-K)>0\big)=S_0e^{-qT}\,N(\phi d_1).$$

$d_2$ is the exercise probability under the $T$-forward measure, $d_1$ the same probability under
the share measure — that is the whole content of the "$d_1$ vs $d_2$" mystery.

**Smile correction (what the Street actually charges).** A digital is the limit of a call spread, so
with a strike-dependent implied vol $\sigma(K)$:

$$V_{\text{CoN}}=-Q\,\frac{\partial C}{\partial K}=Q\,P(0,T)N(d_2)\;-\;Q\,\mathcal V_{BS}\,\frac{\partial\sigma}{\partial K}.$$

On equity indices the skew term is 10–30 % of the price — pricing a digital at a flat vol is a
classic desk error. Risk management: replicate with a **finite-width call spread** (typically
$1\%\cdot K$, or the over-hedge width implied by bid/offer); a sold digital is priced with the wider,
conservative spread. One-touch / no-touch (FX) are the continuously-monitored cousins → §2.4.

Numerics plan:
- **CMC / smoothing**: replace `1{S_T > K}` by its conditional expectation given the path up to T−dt:
  `Φ(d)` under BS — removes the discontinuity entirely → unbiased, huge variance reduction, AAD-compatible.
- **IS**: essential for OTM digitals (shift into the payoff region).
- **LRM vs pathwise**: pathwise delta doesn't exist (a.e. zero) — keep LRM, add AAD-on-smoothed and
  compare bias/variance of the three estimators in a test.
- **QMC**: works well only after smoothing (discontinuity destroys QMC rates) — document this.

### 2.3 Asian Option (arithmetic & geometric average)  ← flagship example

**Payoff**: fixings $t_1<\dots<t_n$, arithmetic average $A=\frac1n\sum_iS_{t_i}$, geometric average
$G=\big(\prod_iS_{t_i}\big)^{1/n}$. Fixed strike: $(\phi(A-K))^+$. Floating strike: $(\phi(S_T-\kappa A))^+$.
Settlement is a few days after the last fixing → discount at $P(0,T_{\text{settle}})$, not $P(0,t_n)$.

**Numéraire**: $T$-forward on the settlement date.

**Closed form — Kemna–Vorst (geometric average, exact).** $\ln G$ is Gaussian with

$$m=\ln S_0+\Big(r-q-\frac{\sigma^2}{2}\Big)\bar t,\quad \bar t=\frac1n\sum_i t_i,\qquad
v^2=\frac{\sigma^2}{n^2}\sum_{i,j}\min(t_i,t_j),$$

$$V_G=\phi\,P(0,T)\big[F_GN(\phi d_1)-KN(\phi d_2)\big],\quad F_G=e^{m+v^2/2},\quad
d_{1,2}=\frac{\ln(F_G/K)\pm\tfrac12v^2}{v}.$$

For uniform fixings $t_i=iT/n$: $\bar t=\frac{(n+1)T}{2n}$ and $v^2=\sigma^2\frac{(n+1)(2n+1)T}{6n^2}$,
whose continuous limit is $v^2\to\sigma^2T/3$ — the famous **vol over $\sqrt3$** rule of thumb.

**Arithmetic approximations used for quoting**: Turnbull–Wakeman / Lévy **moment matching** (fit a
lognormal to the first two moments of $A$, with
$\mathbb{E}[A^2]=\frac{S_0^2}{n^2}\sum_{i,j}e^{(r-q)(t_i+t_j)+\sigma^2\min(t_i,t_j)}$), Curran's
conditioning on the geometric mean, and Ju's Taylor expansion. Errors are typically < 1 bp for
commodity parameters, which is why these are still the desk quoting tools — MC is for the smile.

**Market conventions**: commodity swaps *are* Asians — arithmetic average of the daily settlement
prices over a calendar month (the standard oil/gas/power contract); FX Asians average the WMR fix;
structured notes use "Asian tails" (averaging the final 5–20 business days) to kill gap risk. The
fixing schedule follows a holiday calendar. **Seasoned trades**: with $n_f$ fixings already observed,
the option becomes an option on the residual average with an adjusted strike

$$K'=\frac{nK-\sum_{\text{fixed}}S_{t_i}}{n-n_f},$$

so the engine must accept a running average as state (and the vol effectively shrinks over time).

Numerics plan:
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

**Payoff**: with the first passage time $\tau=\inf\{t:S_t\le B\}$ (down) or $\{t:S_t\ge B\}$ (up),
knock-out $=(\phi(S_T-K))^+\mathbf 1\{\tau>T\}$ plus a rebate $R$ — **paid at hit or at maturity, a
contractual detail that changes the price**; knock-in $=(\phi(S_T-K))^+\mathbf 1\{\tau\le T\}$.
**Parity**: KI + KO = vanilla when the rebate is zero (exact, and one of our tests).

**Numéraire**: $T$-forward for the terminal leg; a rebate paid at $\tau$ discounts at $P(0,\tau)$,
which is why the closed form needs the first-passage density and not just the terminal law.

**Closed form — image / reflection method (Merton 1973).** For a down-and-out call with $B\le K$:

$$C_{DO}(S)=C(S)-\left(\frac{S}{B}\right)^{1-\frac{2(r-q)}{\sigma^2}}C\!\left(\frac{B^2}{S}\right),$$

i.e. the vanilla minus its image reflected through the barrier. The general case (all eight
in/out × up/down × call/put combinations, $B$ above or below $K$, with rebates) is the
**Reiner–Rubinstein (1991)** decomposition into six building blocks $A,B,C,D,E,F$ built from

$$\mu=\frac{r-q-\sigma^2/2}{\sigma^2},\quad \lambda=\sqrt{\mu^2+\frac{2r}{\sigma^2}},\quad
x_1=\frac{\ln(S/B)}{\sigma\sqrt T}+(1+\mu)\sigma\sqrt T,\quad
y_1=\frac{\ln(B/S)}{\sigma\sqrt T}+(1+\mu)\sigma\sqrt T,$$

where $E$ is the rebate-at-maturity term and $F$ (the one carrying $\lambda$) the rebate-at-hit term.
This is the standard analytic engine to add — reference *and* control variate.

**Discrete monitoring — Broadie–Glasserman–Kou (1997)**: a daily-monitored barrier is worth less
(KO) than a continuous one; the continuity correction

$$B\;\longrightarrow\;B\,e^{\pm\beta_1\sigma\sqrt{\Delta t}},\qquad
\beta_1=-\frac{\zeta(1/2)}{\sqrt{2\pi}}\approx0.5826$$

($+$ for up-barriers, $-$ for down-barriers) makes the continuous formula first-order exact in
$\sqrt{\Delta t}$. It is the standard cheap fix when the contract is daily but the formula is continuous.

**Market conventions**: FX is the barrier market — one-touch / no-touch / double-no-touch quoted as a
% of notional, **continuously monitored** (any interbank print, 24 h ex-weekends, with a
barrier-monitoring desk and an official determination process). Equity and structured barriers are
usually **daily close** or purely **European** (observed only at maturity — now the norm on
autocalls, precisely to remove gap risk). Smile handling: **vanna–volga** with survival-probability
weighting is the FX quoting standard; **LSV** is the book standard, because local vol alone
misprices barriers by several vol points. Risk management: **barrier shift / over-hedge** — price
the barrier 1–3 % beyond the contractual level in the direction that penalises the seller, to fund
the delta discontinuity and the hedging slippage at the trigger.

Numerics plan:
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

**Payoff** with $M=\max_{t\le T}S_t$ and $m=\min_{t\le T}S_t$:
floating strike call $=S_T-m$, floating put $=M-S_T$;
fixed strike call $=(M-K)^+$, fixed put $=(K-m)^+$.
Contracts specify the monitoring convention (almost always **daily close**) and may be *seasoned*,
i.e. start from a running extremum $M_0\ne S_0$ — the engine must accept it as an input.

**Numéraire**: $T$-forward; the payoff is a functional of the whole path, so no measure trick helps
beyond the joint law of $(S_T,M)$, which is known in closed form via the reflection principle.

**Closed form — Goldman–Sosin–Gatto (1979)**, continuous monitoring, floating-strike call:

$$C_{fl}=S_0e^{-qT}N(a_1)-m\,e^{-rT}N(a_2)
+\frac{\sigma^2S_0e^{-rT}}{2(r-q)}\left[\left(\frac{S_0}{m}\right)^{-\frac{2(r-q)}{\sigma^2}}
N\!\Big(\!-a_1+\frac{2(r-q)\sqrt T}{\sigma}\Big)-e^{(r-q)T}N(-a_1)\right]$$

with $a_1=\frac{\ln(S_0/m)+(r-q+\sigma^2/2)T}{\sigma\sqrt T}$, $a_2=a_1-\sigma\sqrt T$
(Conze–Viswanathan for the fixed-strike variants). Add as the analytic reference / CV.

**Bridge extremum law** (what we implemented in Phase 3): conditional on $S_i=a$, $S_{i+1}=b$,

$$\mathbb{P}\big(M_{[t_i,t_{i+1}]}\le x\big)=1-\exp\!\left(-\frac{2\ln(x/a)\ln(x/b)}{\sigma^2\Delta t}\right)
\;\Longrightarrow\;
M=\exp\!\left(\frac{\ln a+\ln b+\sqrt{(\ln b-\ln a)^2-2\sigma^2\Delta t\ln U}}{2}\right).$$

**Practitioner caveat (important)**: since contracts are discretely monitored, "removing the
discretisation bias" is only correct when the contract *is* continuous — otherwise it is a
mispricing of a few % in favour of the buyer. The monitoring convention must be an explicit contract
field, not an engine setting. Lookbacks are rarely listed: OTC, mostly FX and structured notes,
and always expensive (the buyer gets the ex-post optimal fixing), so they usually appear in capped
or partial form (lookback over the first month only, "best-of-N-fixings" strike).

Numerics plan:
- **Discretization bias**: running max of discrete path underestimates the true max —
  apply the BB correction: sample the *bridge maximum* between steps
  (inverse-CDF of the Brownian-bridge max: `M = (a + b + sqrt((b−a)² − 2 dt σ² ln U))/2`).
- **CV**: analytic continuous lookback (Goldman–Sosin–Gatto closed form — add as analytic engine)
  and/or vanilla control.
- **QMC + BB** construction, stratify terminal.
- **AAD**: max is a.e. differentiable → pathwise works; subgradient at ties is measure-zero.
- **CUDA**: running max in registers; bridge-max sampling needs one extra uniform per step.

### 2.6 Basket Option (multi-asset)

**Payoff**: $\big(\phi(\sum_iw_iS_i(T)-K)\big)^+$. Industry convention: **performance weights** —
the basket is rebased to 100, $w_i=\tilde w_i\cdot100/S_i(0)$ with $\sum\tilde w_i=1$, and the strike
is quoted as a percentage of the initial level.

**Dynamics / numéraire**: $dS_i/S_i=(r-q_i)dt+\sigma_i dW_i$, $d\langle W_i,W_j\rangle=\rho_{ij}dt$;
$T$-forward measure, forwards $F_i=S_i(0)e^{(r-q_i)T}$. Path construction by Cholesky
$\Sigma=LL^\top$ or by PCA $\Sigma=V\Lambda V^\top$ (the latter for QMC dimension reduction).

**Closed forms**: the *geometric* basket is exactly lognormal —

$$m=\sum_iw_i\Big(\ln S_i(0)+(r-q_i-\tfrac12\sigma_i^2)T\Big),\qquad
v^2=T\sum_{i,j}w_iw_j\rho_{ij}\sigma_i\sigma_j$$

→ Black-76 with $F_G=e^{m+v^2/2}$, exactly as in §2.3, and it is the natural control variate.
For the arithmetic basket the quoting tools are **Lévy / Ju moment matching** and the
**Beisser / Deelstra–Diallo** conditioning bounds.

**Correlation hygiene (genuinely industry standard)**: correlation matrices assembled from pairwise
estimates or from broker marks are rarely PSD. Before any factorisation, project them with
**Higham's nearest-correlation-matrix** algorithm (alternating projections) or apply eigenvalue
flooring. Our spectral-sqrt fallback handles the PSD-but-singular case ($\rho=1$); the NCM
projection belongs in the model layer.

**Risk**: a basket call is **long correlation** (higher $\rho$ → higher basket variance), so the
seller is short correlation; correlation is marked with a bid/offer of several points and a
"correlation skew" (the $\rho$ implied by index vs constituent quotes is not the one implied by
worst-of quotes). Beyond vega, the real greeks are cross-gamma $\partial^2V/\partial S_i\partial S_j$
and cega $\partial V/\partial\rho_{ij}$ — $n(n-1)/2$ bumps, which is exactly the AAD argument.

Numerics plan:
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

**Payoffs**: best-of call $\big(\max_iX_i(T)-K\big)^+$ and worst-of put $\big(K-\min_iX_i(T)\big)^+$
on *performances* $X_i=S_i(T)/S_i(0)$; exchange option $(S_1-S_2)^+$; spread option $(S_1-S_2-K)^+$.

**Margrabe (1978) — the change-of-numéraire showcase.** Take $S_2$ (dividend-reinvested) as
numéraire: $X=S_1/S_2$ is a $\mathbb{Q}^{S_2}$-martingale, so the exchange option is a *driftless*
Black formula and **no interest rate appears**:

$$V=S_1e^{-q_1T}N(d_1)-S_2e^{-q_2T}N(d_2),\quad
d_{1,2}=\frac{\ln\frac{S_1e^{-q_1T}}{S_2e^{-q_2T}}\pm\tfrac12\hat\sigma^2T}{\hat\sigma\sqrt T},\quad
\hat\sigma^2=\sigma_1^2+\sigma_2^2-2\rho\sigma_1\sigma_2.$$

Ideal analytic reference for the multi-asset engine (rates enter only through the forwards).

**Stulz (1982)** gives the two-asset max/min options in terms of the bivariate normal $M(a,b;\rho)$,
with the parity $\text{call}_{\max}+\text{call}_{\min}=\text{call}_1+\text{call}_2$ — another exact test.

**Spread options (energy standard)** — Kirk's approximation, the market tool for crack and spark
spreads:

$$V\approx P(0,T)\big[F_1N(d_1)-(F_2+K)N(d_2)\big],\quad
\hat\sigma^2=\sigma_1^2-2\rho\sigma_1\sigma_2\frac{F_2}{F_2+K}+\sigma_2^2\Big(\frac{F_2}{F_2+K}\Big)^2,$$

with Bjerksund–Stensland as the improved variant and a 2-D numerical integration
(Carmona–Durrleman) as the exact reference.

**Market**: worst-of structures dominate the retail structured-products flow (§2.8). A worst-of is
short the downside skew of *every* underlying and short correlation in the opposite sense to a
basket; because no single $\rho$ can fit both index/constituent and worst-of quotes, desks overlay
**local correlation** or copula adjustments.

Numerics plan:
- Same toolkit as basket (PCA-QMC, IS, AAD, CUDA).
- **CV**: individual vanillas (analytic) per asset, multi-CV OLS; for 2 assets, Stulz closed form
  (min/max of two lognormals) — add as analytic engine + exact reference.
- **AAD caution**: `min/max` kinks → a.e. fine for first-order; document Γ noise.

### 2.8 Autocallable Notes
The realistic structured product — everything combines here.

**Contract (the standard retail Phoenix note)**: notional $N$, underlying = worst-of 2–4 indices,
performances $X^{(i)}_t=S^{(i)}_t/S^{(i)}_0$, $W_t=\min_iX^{(i)}_t$; observation dates
$T_1<\dots<T_n$ (quarterly or annual, 5–10 y final maturity); **autocall barrier** $AC\approx100\%$
(sometimes stepping down 2 %/yr); **coupon barrier** $CB\approx70\%$ with coupon $c$ and **memory**;
**knock-in barrier** $KI\approx60\%$, nowadays **European** (observed only at $T_n$).

**Cashflows**:

$$k^*=\min\{k:W_{T_k}\ge AC\};\qquad
\text{at }T_{k^*}\text{: pay }N\big(1+k^*c\big)\text{ and terminate},$$

$$\text{at each }T_k<T_{k^*}\text{: pay }N\,c\cdot(\text{unpaid count})\ \mathbf 1\{W_{T_k}\ge CB\}
\quad(\text{memory}),$$

$$\text{at }T_n\text{ if never called: }N\Big(1-\big(1-W_{T_n}\big)\mathbf 1\{W_{T_n}<KI\}\Big).$$

**Numéraire**: each cashflow under its own $T_k$-forward measure,
$V=\sum_kP(0,T_k)\,\mathbb{E}^{T_k}[\text{CF}_k]$; in practice one MC under $\mathbb{Q}$ with
deterministic discounting.

**Static decomposition (quoting tool and CV)**: *zero-coupon bond + strip of up-and-out digitals
(the coupons) − down-and-in put on the worst-of*. The bond floor is discounted at the **issuer's
funding spread**, which is a first-order component of the economics — it is why banks issue these
notes at all.

**Model standard**: one underlying → PDE under local vol; worst-of → MC under **LSV** with a
correlation matrix, plus explicit dividend and repo curves (a 10 y note is extremely sensitive to
forward assumptions).

**Risk profile (why smoothing is mandatory)**: the issuer is structurally **short vol, short skew,
short correlation**, long dividend risk, and carries **autocall gap risk** — the delta jumps
discontinuously as spot crosses $AC$ near an observation date. A raw MC delta on the unsmoothed
trigger is pure noise and unusable for hedging; CMC one step before each observation is the fix.

Numerics plan:
- **Smoothing mandatory**: autocall triggers = digital cliffs at each observation date. Apply CMC
  one step before each observation (conditional trigger probability `Φ(·)`) → smooth for AAD + QMC.
- **QMC + BB** across observation dates.
- **CV**: bond floor + strip of vanillas replication (approximate but highly correlated).
- **IS**: shift to balance early-redemption vs maturity scenarios.
- **CUDA**: per-thread state machine with precomputed trigger steps (pattern already exists in
  `short_rate.cpp`, reuse it).
- **AAD**: full vega map / delta / trigger-level sensitivities — the "why AAD exists" demo.

### 2.9 Variance & Dispersion Swaps

**Payoff**: $N_{\text{var}}\big(\sigma^2_{\text{realised}}-K_{\text{var}}\big)$ with the *contractual*
realised variance

$$\sigma^2_{\text{realised}}=\frac{A}{n}\sum_{i=1}^{n}\ln^2\!\left(\frac{S_i}{S_{i-1}}\right)\times100^2,
\qquad A=252 .$$

Note the two conventions that are contractual rather than statistical: **no mean subtraction** and
Act/252 annualisation, on official closing prices. Quoting is in **vol points**, with
$N_{\text{vega}}=2K_{\text{vol}}N_{\text{var}}$ so that
$\text{P\&L}\approx N_{\text{vega}}\frac{\sigma_r^2-K_{\text{vol}}^2}{2K_{\text{vol}}}$.

**Numéraire**: $T$-forward (single payment at maturity).

**Fair strike by static replication — the log contract.** Because
$\mathbb{E}\big[\int_0^T\sigma_t^2dt\big]=-2\,\mathbb{E}\big[\ln(F_T/F_0)\big]$ and a log payoff is
replicated by a $1/K^2$-weighted strip of OTM options:

$$K_{\text{var}}=\frac{2}{T}\left[\int_0^{F_0}\frac{P(K)}{K^2}\,dK+\int_{F_0}^{\infty}\frac{C(K)}{K^2}\,dK\right]\Big/P(0,T).$$

This is *the* industry pricing and hedging argument (and the definition of **VIX** at 30 days). The
real modelling choices are the strike-grid discretisation and the tail truncation; jumps break the
replication (the well-known crash-sensitivity of var swaps).

**Conventions**: single-name var swaps are **capped**, usually at $2.5\times K_{\text{vol}}$ (so they
are var swap − var call and cannot be replicated statically); index var swaps are uncapped.
Variants: **gamma swap** (weights $S_i/S_0$, no cap needed) and **corridor variance**
(weights $\mathbf 1\{S_i\in[A,B]\}$).

**Dispersion**: sell index variance, buy constituent variance —
$\sum_iw_i^2\sigma_i^2-\sigma_{\text{idx}}^2$; the traded quantity is the implied correlation

$$\rho_{\text{imp}}=\frac{\sigma^2_{\text{idx}}-\sum_iw_i^2\sigma_i^2}{2\sum_{i<j}w_iw_j\sigma_i\sigma_j}
\;\approx\;\frac{\sigma^2_{\text{idx}}}{\big(\sum_iw_i\sigma_i\big)^2}\ \ (\text{the "clean" proxy}).$$

Numerics plan:
- Analytic replication (log-contract) already exists → use as **CV** for the MC engine.
- **QMC**: variance is a smooth functional of the path — QMC very effective.
- **AAD**: vega map w.r.t. local-vol grid nodes (adjoint through the σ(S,t) bilinear interpolation).
- **CUDA**: dispersion (n assets × n steps) is compute-heavy → priority kernel with basket.

### 2.10 American Vanilla (and Bermudan extension)
Currently binomial/trinomial only.

**Value = optimal stopping** over stopping times $\mathcal T_{[0,T]}$ (Bermudan: over the exercise
dates only):

$$V_0=\sup_{\tau\in\mathcal T_{[0,T]}}\mathbb{E}^{\mathbb{Q}}\big[e^{-r\tau}\,\Phi(S_\tau)\big],$$

equivalently the variational inequality (free-boundary PDE) with exercise boundary $S^*(t)$:

$$\max\Big(\partial_tV+\tfrac12\sigma^2S^2\partial^2_{SS}V+(r-q)S\partial_SV-rV,\ \ \Phi(S)-V\Big)=0 .$$

**Numéraire**: bank account (the Snell envelope $V_t/B_t$ is a supermartingale, a martingale before
the optimal stopping time). No closed form; the classic approximations are **Barone-Adesi–Whaley**
and **Bjerksund–Stensland** (still used for fast marks on listed books), and the American call on a
non-dividend payer equals the European (exercise only just before an ex-dividend date).

**LSMC (Longstaff–Schwartz 2001)**: backward induction where the continuation value is regressed on
**in-the-money paths only** (the LS convention — essential for stability):

$$\hat C_k(x)=\sum_{l=1}^{L}\beta_l\psi_l(x),\qquad
\beta=\arg\min_\beta\sum_{p\,\in\,\text{ITM}}\Big(\sum_{j>k}e^{-r(t_j-t_k)}\text{CF}^p_j-\hat C_k(S^p_{t_k})\Big)^2,$$

basis $\psi$ = monomials of degree ≤ 3 or weighted Laguerre polynomials; solve with QR/SVD
(Eigen `ColPivHouseholderQR`), never the normal equations. Exercise if $\Phi(S_{t_k})>\hat C_k$.
The resulting policy is suboptimal, so the estimate is **low-biased**; the standard discipline is to
(a) re-price the *stopping rule* on independent paths to remove the foresight bias, and (b) add the
**Andersen–Broadie** dual upper bound to bracket the true value.

**Industry**: listed equity and FX options are American (trees / PDE with discrete dividends);
LSMC is the standard for Bermudan swaptions, multi-asset callables and swing options, and the very
same regression machinery drives XVA exposure profiles.

Numerics plan:
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

**Affine building block**: $P(t,T)=A(t,T)e^{-B(t,T)r_t}$.
- **Vasicek / Hull–White**: $dr=a(\theta(t)-r)dt+\sigma dW$, $B(t,T)=\frac{1-e^{-a(T-t)}}{a}$;
  HW's $\theta(t)$ is fitted so that the model reproduces the initial curve **exactly** — the reason
  it, not Vasicek, is used in production. Exact Gaussian transition → no Euler bias.
- **CIR**: $dr=a(\theta-r)dt+\sigma\sqrt r\,dW$ with the Feller condition $2a\theta>\sigma^2$; exact
  transition is non-central $\chi^2$ (or Andersen's QE scheme).

**Caplet**: on the rate $L(T_{i-1},T_i)$ with accrual $\tau_i$, paid at $T_i$. Under the
$T_i$-**forward measure** the forward rate $F_i$ is a martingale, hence the market formula

$$\text{Caplet}=N\,\tau_i\,P(0,T_i)\,\text{Black}\big(F_i,K,\sigma_i\sqrt{T_{i-1}}\big).$$

**Post-2008 / negative rates**: the market standard is now the **normal (Bachelier)** model, or
shifted-lognormal, for rates volatility:

$$\text{Caplet}=N\tau_iP(0,T_i)\Big[(F-K)N(d)+\sigma_N\sqrt{T}\,n(d)\Big],\qquad d=\frac{F-K}{\sigma_N\sqrt T},$$

with $\sigma_N$ quoted in bp/year, and **SABR** ($dF=\alpha F^\beta dW$, $d\alpha=\nu\alpha dZ$,
$d\langle W,Z\rangle=\rho\,dt$) with Hagan's expansion as the smile parameterisation.

**Swaption — the annuity numéraire**: with $A(t)=\sum_i\tau_iP(t,T_i)$ the swap rate
$S(t)=\frac{P(t,T_0)-P(t,T_n)}{A(t)}$ is a $\mathbb{Q}^A$-martingale, so

$$V=N\,A(0)\,\text{Black or Bachelier}\big(S_0,K,\sigma\sqrt T\big).$$

Cash-settled versus physically-settled annuities are genuinely different conventions (cash-settled
needs a convexity adjustment) — worth a flag on the instrument.

**Hull–White ZCB option (analytic reference / CV)**:

$$\text{ZBC}=P(0,S)N(h)-K\,P(0,T)N(h-\sigma_p),\quad
\sigma_p=\sigma\sqrt{\frac{1-e^{-2aT}}{2a}}\,B(T,S),\quad
h=\frac{1}{\sigma_p}\ln\frac{P(0,S)}{P(0,T)K}+\frac{\sigma_p}{2},$$

and caps/floors follow from the caplet ↔ ZCB-put identity.

**Multi-curve reality**: forecast on the RFR/term curve, discount on the CSA curve; a caplet on a
*compounded* RFR (SOFR-in-arrears) additionally needs the compounding convention plus the
lookback/lockout/payment-delay parameters — the live industry detail post-LIBOR.

Numerics plan:
- **Exact simulation** where available: Vasicek/HW transition is Gaussian (exact step, no Euler bias);
  CIR via non-central χ² sampling (or Andersen QE) — removes step-size bias entirely.
- **CV**: analytic ZCB prices (affine closed forms) as controls for caplets/options.
- **QMC + BB** on the short-rate driver.
- **AAD**: sensitivities to curve nodes + model params (a, σ) in one sweep.
- Keep the eval-major buffer design — it is already the right layout; generalize it into the
  shared `Workspace` abstraction and reuse on GPU.

### 2.12 FX & Commodity Options

**Garman–Kohlhagen**: a foreign currency is an asset paying the continuous "dividend" $r_f$, so

$$V=\phi\big[S_0e^{-r_fT}N(\phi d_1)-Ke^{-r_dT}N(\phi d_2)\big]
=\phi\,P_d(0,T)\big[F_0N(\phi d_1)-KN(\phi d_2)\big],\quad F_0=S_0e^{(r_d-r_f)T}.$$

In practice $F_0$ comes from the **quoted forward points**, not from the two deposit rates (the
cross-currency basis makes them disagree) — the engine should take the forward as an input.

**Foreign–domestic symmetry** (change to the foreign bank account as numéraire): a domestic call on
$S$ with strike $K$ is $S_0K$ times a foreign put on $1/S$ with strike $1/K$ and the same vol —
an exact, free unit test of the engine.

**Quoting conventions (what makes an FX engine credible)**: vols are quoted **by delta**, not by
strike. ATM is the **delta-neutral straddle** vol; the smile is given by the 25Δ (and 10Δ)
**risk reversal** and **butterfly**

$$RR_{25}=\sigma_{25C}-\sigma_{25P},\qquad
BF_{25}=\frac{\sigma_{25C}+\sigma_{25P}}{2}-\sigma_{ATM},$$

with the delta convention itself depending on the pair: spot vs forward delta, and
**premium-adjusted** delta when the premium is paid in the foreign currency (EURUSD premium in EUR).
Expiry/delivery follow the T+2 spot lag and dual holiday calendars. The market strangle vs smile
strangle distinction matters for calibration; **vanna–volga** or SABR builds the surface from the
3–5 quoted points.

**Quanto & composite**: a quanto (foreign asset, domestic payout, fixed FX) shifts the asset drift by
$-\rho\,\sigma_S\sigma_{FX}$; a composite (payout converted at the floating rate) prices with
$\hat\sigma^2=\sigma_S^2+\sigma_{FX}^2+2\rho\sigma_S\sigma_{FX}$. Both are one-line changes in a
templated engine and deserve dedicated tests.

**Commodities**: everything is written on **futures** → Black-76 with $F$ read from the listed curve;
there is no carry to model, the convenience yield is implicit in $F$. Volatility is term-structured
and **seasonal** (gas, power), and the flow products are Asians on the monthly average (§2.3), spread
options — crack, spark, location — (§2.7, Kirk), and swing/take-or-pay contracts (multiple exercises
with volume constraints → LSMC, §2.10).

Numerics plan:
- Garman–Kohlhagen ≡ BS with two carry rates → inherit *everything* from vanilla (2.1) once
  engines are templated on the model (this is the payoff of the template refactor: zero new MC code).
- Commodity convenience yield same story: the templated engine prices futures options unchanged.

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
