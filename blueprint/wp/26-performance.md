# WP 26 — Performance: a fast generic engine, then multilevel Monte-Carlo with QMC

| | |
|---|---|
| **Depends on** | Scripting (WP 16: parser, visitors, bytecode), AAD (WP 17), GPU Monte-Carlo (WP 19: Philox, Sobol, Brownian bridge, logical blocks, bytecode on the device). All done |
| **Unlocks** | Removal of the hand-written Monte-Carlo engines (`src/engines/mc/{asian,barrier,lookback,basket,autocall,…}.cpp`); MLMC for every model with a discretisation bias (local vol, Heston, SLV, rough Bergomi); faster xVA scenarios (WP 23) |
| **Branches** | one per lot: `perf/p0-baseline`, `perf/p1-batched-interpreter`, … (§4–§10) |
| **Issues** | P0 #206, P1 #207 (P1c Sobol: #214), P2 #208, P3 #209, P4 #210, P5 #211, P6 #212 |
| **Starting point** | tag `v1-pedagogical` (`7d21573`, 2026-11-10): the code before this work package, kept as a reference, never maintained |
| **Main reading** | Giles, *Multilevel Monte Carlo path simulation*, Operations Research 56(3), 2008; Giles, *Multilevel Monte Carlo methods*, Acta Numerica 24, 2015; Giles & Szpruch, *Multilevel Monte Carlo methods for applications in finance*, 2012 ([arXiv:1212.1377](https://arxiv.org/abs/1212.1377), in `kbase`); Giles & Waterhouse, *Multilevel quasi-Monte Carlo path simulation*, Radon Series on Computational and Applied Mathematics 8, 2009 |
| **Implementation reading** | Savine & Andreasen, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021; Owen, *Scrambled net variance for integrals of smooth functions*, Annals of Statistics 25(4), 1997; Burley, *Practical Hash-based Owen Scrambling*, JCGT 9(4), 2020 (already in `utils/sobol.hpp`) |

Until now this repository was a teaching library. This work package turns it
into a fast one, without forking it: the generic engine becomes fast enough
to price every catalogue product, the hand-written Monte-Carlo engines are
retired one by one behind the registry, then multilevel Monte-Carlo, alone
and with randomised QMC, is built **once**, in the generic engine.

Every lot is judged on two numbers: the **measured** gain in time to reach a
given error, and the **proof** that the prices did not move. The method is the
`hpc-finance` skill (`.claude/skills/hpc-finance/`).

---

## Contents

1. [Decision: one code base, improved in place](#1-decision-one-code-base-improved-in-place)
2. [Where we start](#2-where-we-start)
3. [Rules for every lot](#3-rules-for-every-lot)
4. [P0 — Baseline and reference kernels](#4-p0--baseline-and-reference-kernels)
5. [P1 — Fast script execution](#5-p1--fast-script-execution)
6. [P2 — Model-aware primitives in the language](#6-p2--model-aware-primitives-in-the-language)
7. [P3 — Catalogue products as scripts](#7-p3--catalogue-products-as-scripts)
8. [P4 — Multilevel Monte-Carlo](#8-p4--multilevel-monte-carlo)
9. [P5 — Multilevel quasi-Monte Carlo](#9-p5--multilevel-quasi-monte-carlo)
10. [P6 — Multilevel on the GPU](#10-p6--multilevel-on-the-gpu)
11. [Results](#11-results)
12. [Risks and open decisions](#12-risks-and-open-decisions)

---

## 1. Decision: one code base, improved in place

Decided by the maintainer on 2026-11-10: **no V2 next to a V1**. The
pedagogical version is the tag `v1-pedagogical`; it can be checked out and
built at any time and is never maintained.

The registry already lets two implementations of the same (instrument, model,
engine) live side by side. Every migration follows the same pattern:

1. the new path (a script priced by the generic engine) is registered next to
   the old engine;
2. a cross-engine test requires both to agree within Monte-Carlo error
   (z-score below 4 over several seeds);
3. the benchmark of §11 shows the new path faster at equal error;
4. the registry's default route switches, and the old engine is **deleted** in
   the same pull request (blueprint rule: no dead duplicate).

## 2. Where we start

### 2.1 What each Monte-Carlo path does today

Inventory read from `src/engines/mc/` and `src/pricers/registry.cpp` on
2026-11-10.

| Product (model) | Generator | Variance reduction | Threads | GPU | Greeks |
|---|---|---|---|---|---|
| Vanilla (BS) | PCG32, Philox, Sobol | antithetic, stratification, importance sampling | logical blocks | yes (`src/gpu/vanilla_bs.cu`) | bump |
| Asian (BS, local vol) | PCG32, Sobol | antithetic; geometric-Asian control (BS) | 1 | no | bump |
| Barrier (BS, local vol) | PCG32 | Brownian-bridge crossing test, conditional MC | 1 | no | bump, 9 simulations a path |
| Lookback (BS, local vol) | PCG32 | antithetic, extrema drawn on the bridge | 1 | no | bump |
| Basket (BS) | PCG32 | antithetic | 1 | no | bump, common numbers |
| Autocall, mountain, best-of, worst-of, dispersion, variance and volatility swaps (BS); quanto vanilla; caplet, cap/floor, bond option (short rate) | PCG32 | none | 1 | no | bump |
| **Script, generic engine** (`SimulationMCEngine`) | PCG32, Philox, Sobol + bridge, Owen | antithetic, generic controls, stratification, importance sampling (GHS drift), spot control | logical blocks | yes, 2 V100 | AAD (CPU tape; GPU duals and per-path adjoint) |

All the depth is in the generic engine. The hand-written engines stayed at the
teaching stage.

The timeline architecture of Savine & Andreasen is already the generic
engine's: `ISimulatableProduct` (timeline, defline, payoffs),
`ISimulationModel` (merged timeline, `BrownianLayout`, `clone`),
`SimulationMCEngine`. Only `ScriptedProduct` and a demonstration Asian
(`instruments/equity/simulatable_asian.hpp`) implement the product side.

### 2.2 The cost of genericity, measured

Up-and-out call, BS, barrier 130 monitored daily for one year, 200 000 paths,
one CPU thread unless stated (2026-11-10, Python wheel):

| Path | Time | Std error |
|---|---|---|
| Hand-written (`price_barrier_bs_mc`), price **and** 4 bump greeks | 5.21 s | 0.0141 |
| Script, PCG32, CPU, price only | 2.40 s | 0.0142 |
| Script, Sobol + bridge, CPU | 4.36 s | 0.0041 |
| Script, Philox, 1 V100 | 0.048 s | 0.0118 |
| Script, Sobol + bridge, 1 V100 | 0.034 s | 0.0041 |

The hand-written engine runs 9 simulations a path (price and 8 bumped
revaluations, `barrier.cpp:196-204`): its price alone is **estimated** at
~0.6 s, so the generic engine is about 4× slower a path on the CPU at equal
technique. At equal **error** it wins by far (Sobol + bridge, GPU). P0
replaces the estimate by a measurement.

The 4× comes from execution, not from what the engine can do:

| Cost | Cause |
|---|---|
| Interpretation | each event's bytecode is decoded on every path (CPU), or dispatched through a large `switch` on the GPU: 12 to 15 cycles of `no_instruction` stall per issued instruction (WP 19 §9.1) |
| Materialised scenario | the model writes every sample of the defline, the product reads them back, through virtual calls (`generate_path`, `payoffs`); a hand-written loop keeps only its running state |
| Registers on the GPU | the interpreter's stack takes 128 registers a thread: 25 % occupancy (WP 19 §9.1, issue #130) |

What only hand-written code does today is product knowledge, not speed:
closed-form controls (geometric Asian), the bridge crossing probability
between two dates (it needs the model's volatility on the step), conditional
MC on a barrier. P2 brings them into the language.

## 3. Rules for every lot

- **Metric: time to reach a relative standard error** (1e-3 and 1e-4), not
  time a path: MLMC and QMC change the variance, not the cost of a path.
  Median of 5 runs, warm-up excluded, GPU memory used by other processes noted.
- **Reproducibility**: same seed, same bits, whatever the number of threads or
  GPUs (logical blocks). A change that reorders a floating-point sum says so
  and states its tolerance.
- **Oracle**: the old path stays until the new one matches it within error;
  the qmharness `qm_run` / `qm_compare` reports (golden prices, cross-engine,
  invariants, convergence slope, greeks, determinism) are run before and after.
- **Each lot fills its column of §11** and quotes its numbers in the pull
  request and the commit message.
- New tests are property tests where possible: complexity slopes in log-log,
  level variance decay, CPU = GPU, one card = two cards.

## 4. P0 — Baseline and reference kernels

Measure before changing anything.

- **`greeks = None` honoured by the hand-written engines**, so that their price
  alone can be timed (today the barrier engine always computes its bump
  greeks).
- **Benchmark matrix** `benchmarks/perf_matrix.cpp` → `qm_perf_bench`: every
  Monte-Carlo route of the registry (19 product × model pairs, §2.1), each in
  the configurations it supports: hand-written engine; script CPU (PCG32,
  Philox, Sobol + bridge, Owen), 1 and 16 threads; script GPU, 1 and 2 V100.
  Output: JSON in `benchmarks/results/<date>-<sha>.json` and the markdown
  table of §11. Product terms fixed in the file (the script library's
  examples where they exist).
- **Reference kernels** (`benchmarks/reference/`, never in the library):
  hand-written CPU loop and CUDA kernel for the daily barrier under local vol,
  the arithmetic Asian and the worst-of autocall. They are the ceiling the
  generic engine is measured against.
- **qmharness baseline**: one `full` run stored, the reference for every later
  `qm_compare`.

Acceptance: `qm_perf_bench` reproduces its own numbers within 10 % on two runs;
§11's "v1" column is filled; the barrier's price-only time replaces the
estimate of §2.2.

## 5. P1 — Fast script execution

Goal: **the generic engine within 1.5× of the reference kernels** of P0, on CPU
and on GPU, with the same bits as today's interpreter.

### P1a — Batched interpreter on the CPU

The bytecode interpreter runs a batch of W paths (W = 8 or 16: two to four
AVX2 registers of four doubles; the server's Xeon E5-2680 v4 has no AVX-512)
per decoded instruction: one dispatch for W paths instead of one, and the
arithmetic vectorised across paths. Variables,
the stack and the scenario become structure-of-arrays of width W. Conditions
become masks (both branches evaluated, results blended), which is also what
the GPU does within a warp.

Same bits as the scalar interpreter: same operations, same order, per path.
Masked evaluation must not evaluate a branch's side effects for a masked-out
path (a `pays` statement, a variable assignment). Transcendentals (`exp`,
`log`, `erfc`) stay scalar libm calls unless the maintainer accepts the
ulp-level difference of a vector library (SLEEF), which would break the
same-bits rule: see the skill's `libraries.md`.

### P1b — Scripts compiled for the GPU

Approved by the maintainer on 2026-11-10, as is compiling scripts for the CPU
if P1a falls short (decision point below: what remains open is only how a
compiler gets into the production image).

The script's tree is turned into CUDA source (one straight-line function per
event), compiled at run time with NVRTC and cached by a hash of the script
and the model kind. The interpreter's `switch`, stack and register pressure
disappear. The interpreter stays as the fallback (NVRTC unavailable,
compilation failure) and as the oracle in tests.

- Compile with `--fmad=false`, like the interpreter's separate operations,
  so the bits match; measure what FMA contraction would gain and decide with
  the maintainer before allowing it (it changes the bits, not the accuracy).
- Compilation cost (expected tens to hundreds of milliseconds) is paid once per
  script; the API reprices the same scripts many times.
- AAD on the GPU (forward duals, per-path adjoint) gets the same treatment in
  a second step: generated code for the tangent and adjoint sweeps.

### P1c — Sobol, the default sampler, as fast and as good as it can be

Decided by the maintainer on 2026-11-10: **Sobol (randomised QMC with the
Brownian bridge) becomes the default sampler everywhere**, and is improved
until it is the best it can be, in speed and in variance.

**Where the default is set today.** Scripts priced through the API already
default to Sobol (`api/app/schemas.py`, the scripting page's
`VarianceReductionFields.tsx`; PR #120). Everything else still defaults to
pseudo-random: `PricingSettings::mc_sampler` (C++), `price_script`'s
`sampler="pseudo"` (binding, `src/module.cpp`), the dated Asian request
(`schemas.py`) and its preview (`DatedAsianPreview.tsx`), and the hand-written
engines, which only know PCG32. The switch:

- every layer above defaults to `sobol`, in the same pull request, with
  `web/openapi.json` and `schema.gen.ts` regenerated;
- the catalogue products get Sobol when they become scripts (P3); until then
  the hand-written engines stay pseudo-random and say so in their
  diagnostics;
- pseudo-random stays where Sobol brings nothing measurable or cannot apply,
  and the engine says which sampler it used and why (as the model choice is
  announced): models whose draws are not Brownian increments (jumps: Merton,
  Kou, Bates) until measured, and the dimensions beyond the generator's
  21 201 (padded with pseudo-random draws).

**What the displayed error means.** With Sobol the standard error comes from
the dispersion of R = 16 independent replicates: 15 degrees of freedom, so the
error estimate itself is uncertain by about ±18 % (1/√(2·15)). Either R goes
to 32, or the interval uses the Student quantile; decided with the numbers of
P0, since a displayed figure must be defensible.

**Quality**, each measured with WP 19 §12's protocol (the 42 library scripts,
200 seeds, 95 % interval of the variance ratio) before it becomes a default:

- point counts: a Sobol net is balanced at N = 2^m points per replicate;
  check what the engine does with other path counts, round to a power of two
  if it does not;
- path construction: Brownian bridge (today) against PCA of the Brownian
  increments (`etc/ROADMAP.md` §1.1 lists it), and, for several factors, which
  factor's coarse coordinates take the first Sobol dimensions;
- randomisation: digital shift (today's decision, WP 19 §12) against a true
  Owen scrambling, random bits per node of the tree, which the hash of
  Burley only approximates;
- discontinuous payoffs lose most of QMC's edge: P2's smoothed indicators and
  bridge survival probabilities are what gives it back (barriers, digitals,
  autocalls).

**Speed.** Measured on 2026-11-10 (`qm_gpu_rng_bench`, V100, 2^20 points × 128 dims):
our random-access `sobol_point_bits` fills memory at 249 GB/s; the same bits
computed branchless, with a dimension's 32 directions in shared memory, at
456–500 GB/s; cuRAND's Sobol at ~820 GB/s, thanks to Gray-code stepping (one
XOR from a point to the next). cuRAND itself is not adopted (no Owen
scrambling, no inline per-path use, no CPU-only build: skill
`libraries.md`); its algorithm is.

- Branchless `sobol_point_bits` with the directions in shared memory in every
  kernel that draws Sobol points: same bits, no decision needed.
- Gray-code stepping where a thread walks consecutive points. The logical
  blocks give thread t the units t, t + 256, t + 512…: stepping needs
  consecutive units per thread, which changes the reduction order, so the CPU
  emulation changes with it (same bits CPU = GPU kept) and the prices move
  within their error. Measured in the fused kernels before deciding: the
  generator is only part of a path's cost.

### P1d — Streaming state

The script analyser already knows which samples each event reads. Where the
script allows it (no look-back to an earlier date's sample), the model hands
each event its samples as it simulates them, and the full scenario is never
stored: the daily barrier keeps `alive` and the spot, like a hand-written loop.

### Decision point

If P1a misses the 1.5× target on the CPU, the next step is compiling scripts
to C++ at run time (as P1b does for CUDA), approved in principle. It needs a
C++ compiler in the production image, which today has none
(`web/Dockerfile.prod`): how to ship it (full toolchain, or a JIT library
such as LLVM ORC) is decided with the measurement in hand.

Acceptance: §11's P1 column; every script of the library prices to the same
bits through the batched interpreter and the compiled GPU kernel as through
today's interpreter; the WP 19 §9.1 table measured again (occupancy,
`no_instruction`).

## 6. P2 — Model-aware primitives in the language

What hand-written engines know about their product, made available to every
script:

- **Continuous barrier monitoring with the bridge correction**: a primitive
  that returns the probability of not crossing a level between two
  consecutive simulation dates, given the model's volatility on that step
  (exact for BS, local-vol approximation at the step's start). With it, the
  survival indicator becomes a product of probabilities: the conditional MC
  of `barrier.cpp`, for any script.
- **Smoothed indicators**: the fuzzy evaluator (WP 16c) already exists on the
  tree; carry it into the bytecode and the compiled kernels.
- **Analytic controls**: a script declares a control payoff (another script)
  whose expectation a closed-form pricer of the registry knows under the
  model (the geometric Asian under BS, the vanilla for a barrier). The engine
  regresses it out like the generic controls of WP 19 §2.5.

Every language change updates `api/app/assistant/prompt.py`, whose examples
`pytest` checks against the parser (CLAUDE.md).

Acceptance: the barrier with the primitive matches the hand-written engine's
continuous-monitoring price (bridge on) within error; the arithmetic Asian
with its declared control reaches the hand-written engine's variance.

## 7. P3 — Catalogue products as scripts

Each Monte-Carlo route of the registry gets a **C++ script builder**: the
instrument's terms written as a script, priced by the generic engine. The
builder lives in the C++ pricers (not in Python), so that the wheel, the API
and the CLI use it. `api/app/scripted_payoffs.py` already does it in Python
for the dated Asian, lookback and barrier: it is the precedent, and moves to
C++.

Order, by value and by what the language already supports:

1. barrier and lookback (BS, local vol), Asian (BS, local vol);
2. worst-of and best-of, basket, mountain, autocall (BS);
3. variance and volatility swaps, dispersion;
4. quanto vanilla; caplet, cap/floor and bond option under the short-rate
   script models.

For each: cross-engine test (§1), §11 row filled, old engine deleted. Greeks
change method (bump with common numbers → AAD): the numbers shown on the site
move within their error; the pull request says so.

Acceptance: `src/engines/mc/` keeps only the generic engine, the model
kernels and what the GPU needs; every catalogue Monte-Carlo price comes from a
script.

## 8. P4 — Multilevel Monte-Carlo

### 8.1 The method

With P_l the payoff computed on a time grid of step h_l = h_0 / M^l (M = 2),
the telescoping sum

```latex
\mathbb{E}[P_L] = \mathbb{E}[P_0] + \sum_{l=1}^{L} \mathbb{E}[P_l - P_{l-1}]
```

is estimated level by level, each with its own number of paths N_l, the fine
and coarse payoffs of a level computed **on the same Brownian path** so that
their difference has a small variance V_l. For a target root-mean-square
error ε, the optimal numbers of paths are (Giles 2008; Giles & Szpruch 2012
§2.6)

```latex
N_l = \left\lceil 2\varepsilon^{-2} \sqrt{V_l / C_l} \sum_{k=0}^{L} \sqrt{V_k C_k} \right\rceil
```

with C_l the cost of one sample of level l. If the bias decays as h^α, the
level variance as h^β and the cost grows as h^-γ, the complexity is ε^-2 when
β > γ, ε^-2 (log ε)^2 when β = γ, and ε^-2-(γ-β)/α when β < γ (Giles 2008,
Theorem 3.1). Plain Monte-Carlo with a biased scheme costs ε^-3 for α = 1.

### 8.2 Where it helps, and where it does not

MLMC removes the cost of a **discretisation bias**. It brings nothing where
the simulation is exact between the product's dates.

| Case | Bias to remove | MLMC |
|---|---|---|
| BS, any product on its event dates | none (exact log-normal steps) | no gain: not measured as one |
| Local vol, Heston, SLV, rough Bergomi | time step of the scheme | **yes**: the main target |
| Continuously monitored barrier, lookback | monitoring between dates | yes, with the P2 bridge primitive for the variance decay |
| Daily-monitored barrier under BS | none | no gain |

### 8.3 Coupling in the timeline architecture

- Level l's simulation grid is the product timeline merged with a uniform grid
  of step h_l; the coarse grid of level l is level l−1's, so every coarse date
  is a fine date.
- The engine draws the fine Brownian increments; the coarse increments are
  sums of fine ones over each coarse step. `BrownianLayout` already describes
  which draws are Brownian increments and on which times; it is what lets the
  engine build the coarse draws for any model without knowing it.
- `ISimulationModel` gains a level-aware `init` and a coupled
  `generate_path(fine_gaussians, fine_path, coarse_path)`; models with
  non-Brownian draws (jumps) stay out of MLMC at first.
- The product is evaluated twice per sample (fine and coarse scenario); the
  level's sample is the difference.
- Draws are keyed by (seed, level, path): reproducible, and independent
  between levels, as the estimator requires.

### 8.4 Algorithm

Giles's adaptive algorithm (Giles 2015 §3; Giles & Szpruch 2012 §2.6):
start with L = 2, estimate V_l and C_l on a pilot of N = 10^3 to 10^4 paths a
level, set N_l by §8.1, add paths, test the bias on the last two levels
(max(|Ŷ_{L−1}| / M^α, |Ŷ_L|) < (M^α − 1) ε / √2), add a level if it fails.
α and β are estimated by regression on the levels, not assumed.

### 8.5 Scheme

Euler on the log-spot gives β = 1 for local vol: β = γ, complexity
ε^-2 (log ε)^2. **Milstein** gives β = 2 for a scalar diffusion (Giles &
Szpruch 2012), complexity ε^-2, and is what makes P5 worthwhile (§9). For
local vol the Milstein term needs ∂σ_loc/∂S, read from the Dupire grid's
interpolation. Heston and SLV start with the existing full-truncation Euler;
Milstein for them is a later decision.

### 8.6 Tests

- **Euler-GBM test bed**: GBM stepped by Euler on S (not log S), whose bias is
  O(h) against a known exact price; measure α ≈ 1, β ≈ 1 (Euler) and β ≈ 2
  (Milstein) in log-log.
- **Complexity**: cost against ε in log-log, slope ≈ −2 (Milstein) against
  ≈ −3 for plain MC at the same bias.
- **Local vol**: MLMC reprices the vanillas the Dupire grid was built from,
  within ε.
- MLMC = plain MC on the finest grid, within error.

## 9. P5 — Multilevel quasi-Monte Carlo

Each level's paths are Sobol points (Brownian bridge on the level's fine grid,
the coarse increments aggregated from the fine ones), randomised by R
independent scramblings (R = 16 to 32) so that each level's variance can be
estimated. N_l grows by doubling the level with the largest V_l / C_l
(Giles & Waterhouse 2009).

**Randomisation: the digital shift, unless a measurement says otherwise.**
Owen's nested scrambling has a better variance rate on smooth integrands in
theory (Owen 1997), but its hash-based version (Burley 2020, `owen_scramble`,
`mc_sobol_owen`) was measured against the digital shift on the 42 library
scripts over 200 seeds (WP 19 §12): no consistent gain, and the verdict
flipped with the number of points for the same product. P5 measures both on
the multilevel estimator with the same protocol, and a true Owen scrambling
(random bits per tree node) if §5's P1c gets to it.

What to expect, from Giles & Szpruch 2012 §8: QMC cuts the variance on the
coarse, low-dimensional levels; the finest levels have little low-dimensional
content and gain little. With β > γ (Milstein) the cost sits on the coarse
levels and Giles & Waterhouse measured about ε^-1.5 instead of ε^-2 on a
European call; with β ≤ γ (Euler on local vol) the gain is unlikely. P5 is
therefore measured on the Milstein local-vol case first.

The finest level's dimension (steps × factors) stays below the Sobol
generator's 21 201 dimensions (`kMaxDimension`); beyond, the remaining
dimensions are pseudo-random (a padding the engine states in its
diagnostics).

Acceptance: measured complexity slope of MLQMC below MLMC's on the Milstein
local-vol Asian and vanilla; same price within error.

## 10. P6 — Multilevel on the GPU

Levels map onto the segments of `run_logical_block_segments` (WP 19, lot G3):
each level is reduced by its own tree, all levels fill the device at once,
and the pilot and correction rounds of the adaptive algorithm are launches.
Bits identical on one or two cards, CPU = GPU within error.

Acceptance: §11's P6 column; `GpuTwoCardsTest`-style test for MLMC.

## 11. Results

Time to reach a relative standard error of 1e-3, filled lot by lot
(`qm_perf_bench`). "v1" is the tag `v1-pedagogical`.

| Product (model) | v1 hand-written | v1 script CPU 16 thr. | v1 script 1 V100 | P1 | P3 | P4 MLMC | P5 MLQMC | P6 GPU |
|---|---|---|---|---|---|---|---|---|
| (filled by P0) | | | | | | | | |

## 12. Risks and open decisions

- **Runtime compilation in production.** NVRTC must be in the runtime image
  (static `libnvrtc_static` exists since CUDA 11.5: check Debian's
  `nvidia-cuda-toolkit` 12.4 ships it). A CPU code generator would need a C++
  compiler in the image: decided at P1's decision point.
- **Bits and FMA.** Generated code must not contract across operations the
  interpreter keeps separate (`--fmad=false`, `-ffp-contract=off`).
- **Greeks.** Moving products to scripts moves their greeks from bump to AAD:
  the displayed values change within their error. Said in every P3 pull
  request.
- **Exercise.** Early exercise runs on the tree evaluator only today
  (`scripted_product.hpp`): Bermudan scripts stay on it until the bytecode
  handles `exercise()`.
- **Sobol dimension.** The finest MLQMC level is capped by 21 201 dimensions
  (§9).
- **GPU memory.** The LLM of the scripting assistant shares both cards
  (13 to 14 GB of 16 when loaded): the benchmarks note what was free, and the
  GPU tests that fail with `out of memory` while it is loaded are an
  environment limit, not a regression.
