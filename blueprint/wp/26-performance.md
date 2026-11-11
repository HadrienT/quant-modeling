# WP 26 — Performance: a fast generic engine, then multilevel Monte-Carlo with QMC

| | |
|---|---|
| **Depends on** | Scripting (WP 16: parser, visitors, bytecode), AAD (WP 17), GPU Monte-Carlo (WP 19: Philox, Sobol, Brownian bridge, logical blocks, bytecode on the device). All done |
| **Unlocks** | Removal of the hand-written Monte-Carlo engines (`src/engines/mc/{asian,barrier,lookback,basket,autocall,…}.cpp`); MLMC for every model with a discretisation bias (local vol, Heston, SLV, rough Bergomi); faster xVA scenarios (WP 23) |
| **Branches** | one per lot: `perf/p0-baseline`, `perf/p1-batched-interpreter`, … (§4–§10) |
| **Issues** | tracking #217; P0 #206, P1 #207 (P1c Sobol: #214), P2 #208, P3 #209, P4 #210, P5 #211, P6 #212 |
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
8. [P4 — Multilevel Monte-Carlo](#8-p4--multilevel-monte-carlo) (8.7: approximate random variables)
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
| Vanilla (BS) | PCG32, Philox, Sobol | antithetic, stratification, importance sampling | 1 (logical blocks, CPU emulation of the GPU tree) | yes (`src/gpu/vanilla_bs.cu`) | pathwise |
| Asian (BS, local vol) | PCG32, Sobol | antithetic; geometric-Asian control (BS) | 1 | no | bump |
| Barrier (BS, local vol) | PCG32 | Brownian-bridge crossing test, conditional MC | 1 | no | bump, 9 simulations a path (P0: `compute_greeks = false` prices only) |
| Lookback (BS, local vol) | PCG32 | antithetic, extrema drawn on the bridge | 1 | no | bump |
| Basket (BS) | PCG32 | antithetic | 1 | no | bump, common numbers |
| Autocall, mountain, best-of, worst-of, dispersion, variance and volatility swaps (BS); quanto vanilla; caplet, cap/floor, bond option (short rate) | PCG32 | none | 1 | no | bump |
| **Script, generic engine** (`SimulationMCEngine`) | PCG32, Philox, Sobol + bridge, Owen | antithetic, generic controls, stratification, importance sampling (GHS drift), spot control | **1** for pricing (threads only for AAD: `simulation_engine_parallel_aad.hpp`) | yes, 2 V100 | AAD (CPU tape; GPU duals and per-path adjoint) |

All the depth is in the generic engine. The hand-written engines stayed at the
teaching stage. **No Monte-Carlo pricing uses more than one CPU thread**, on
any path: the server's 56 hardware threads only serve the AAD, the xVA and
the rough Bergomi surface pricer today.

The timeline architecture of Savine & Andreasen is already the generic
engine's: `ISimulatableProduct` (timeline, defline, payoffs),
`ISimulationModel` (merged timeline, `BrownianLayout`, `clone`),
`SimulationMCEngine`. Only `ScriptedProduct` and a demonstration Asian
(`instruments/equity/simulatable_asian.hpp`) implement the product side.

### 2.2 The cost of genericity, measured (P0)

From the v1 matrix (§11, `benchmarks/results/2026-11-11-f2a4a48.json`): time
to reach a relative standard error of 1e-3, one CPU thread.

**At equal technique (pseudo-random, antithetic), a path costs about the
same** in the script engine and in the hand-written ones, not several times
more as first estimated:

| Product | Hand-written | Script, PCG32 | Ratio |
|---|---|---|---|
| Up-and-out, 52 weekly dates (BS), price only | 5.64 s | 8.25 s | 1.46 |
| Up-and-out, 52 weekly dates (local vol), price only | 9.31 s | 12.2 s | 1.31 |
| Vanilla call (BS) | 0.085 s | 0.176 s | 2.1 |
| Worst-of call, 3 assets (BS) | 0.83 s | 1.42 s | 1.7 |
| Lookback, basket, Asian (hand-written timed with their Greeks) | | | 0.6 to 1.0 |

The first estimate (~4×, 2026-11-10) divided the barrier's time with Greeks
by its 9 simulations a path; the draws are shared, and the price alone costs
a quarter of the time with Greeks (5.64 s against 23.8 s), not a ninth.

**At equal error the script engine wins by one to three orders of
magnitude**, through Sobol + bridge (one CPU thread: 3.8× on the barriers,
28 to 130× on the Asians, lookbacks, basket and worst-of; upper bounds where
the first 16 384 points already reach the target) and the GPU (one V100 with
Sobol against one CPU thread with PCG32: 180× on the vanilla, 600 to 750× on
the path-dependent products).

**Against hand-written reference paths on the GPU** (`qm_gpu_reference_bench`,
the same products, Philox, antithetic): the script engine takes 2.0× (barrier
BS), 3.4× (barrier local vol), 4.5× (Asian BS) and 4.8× (Asian local vol) the
reference's time. This is the interpreter's cost that P1b removes.

**On the CPU the gaussians dominate a path.** The reference paths draw
Philox + Φ⁻¹, as the GPU does: on one thread they are *slower* than the
hand-written barrier (6.99 s against 5.64 s), which draws PCG32 +
Box-Muller. Sobol needs Φ⁻¹, so its cost is what P1 must make cheap on the
CPU.

**What P0 also found.** The hand-written local-vol barrier priced every model
at the flat vol σ_loc(S₀, 0), ignoring the Dupire surface (fixed in `f2a4a48`,
with a test that fails on the old engine). The script engine and the
reference paths agreed with each other (4.13, 4.14) and not with it (3.63):
the cross-engine comparison is the oracle P3 relies on. The qmharness base
holds 9 cases, all on the Black-Scholes vanilla: it could not have seen it.

Where the script engine's execution costs, what P1 attacks:

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

**Done (2026-11-11).** Measured first, by ablation (no profiler on the
server without root): on one thread, a unit of the daily Asian under BS (two
paths, ~256 dates) cost 31.0 µs -- 7.1 for its draws (Philox + Φ⁻¹), 13.3
for the model's steps, 10.6 for interpreting its events (21 ns an event).
The hand-written reference spends 19.6 µs in all: the draws and the model
already cost what it costs, the gap was the interpreter.

`scripting::run_event_batch` runs an event on kBatch = 8 paths per decoded
instruction: the stack and the variables lane-minor, each lane through the
scalar interpreter's operations (`std::max`, the scalar libm, `if` rather
than `+ 0` for masked lanes), so the same bits. A hard `if` is a mask over
the compiler's layout (JumpIfFalse a; then; Jump b; else; b:), each branch
skipped when no lane is in it. `Program::batchable()` admits hard ifs nested
at most 32 deep; fuzzy scripts, the tree evaluator and AAD stay path by path.
`ISimulatableProduct::batch_width / payoffs_batch` carry it to the engine;
`ScriptedProduct` implements them. In the engine, a logical block's samples
are computed first, a unit and its mirror side by side in a batch, and then
reduced in the serial order (`reduce_logical_blocks_filled`); Sobol batches
its points in sequence. Philox (plain, antithetic, stratified, importance
sampling) and Sobol, one thread or a pool; not with spot controls yet.
`PricingSettings::mc_batch = false` keeps the path-by-path engine as the
oracle: `tests/testScriptBatch.cpp` compares the two bit for bit on the
whole library (a mask removed from `Assign` makes 36 cases fail), CI runs
it.

Measured (`build/qm_script_cpu_bench`, one thread, Philox + antithetic,
100 000 units, against the reference paths at the same units):

| Product | Path by path | Batched | Reference | Batched / reference |
|---|---|---|---|---|
| Up-and-out, 52 weekly dates (BS) | 568 ms | 500 ms | 373 ms | **1.34** |
| Up-and-out (local vol) | 909 ms | 834 ms | 779 ms | **1.07** |
| Arithmetic Asian, ~256 dates (BS) | 3 096 ms | 2 420 ms | 1 953 ms | **1.24** |
| Arithmetic Asian (local vol) | 4 666 ms | 3 939 ms | 3 563 ms | **1.11** |

The interpretation share of the BS Asian goes from 10.6 to ~3.8 µs a unit.
**P1's goal is met on the CPU too**; the decision point below is settled
without compiling scripts to C++ (no compiler in the production image).

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

**Step 1 done (prices, 2026-11-11).** What was built, and what the
measurements changed in the design above:

- *Generator* (`scripting/cuda_codegen.*`, tested on the CPU by
  `tests/testScriptCudaCodegen.cpp`): from the bytecode, not the tree. The
  stack depth is static at every instruction, so each stack slot becomes a
  named local and each jump a `goto`; the source has no include and no user
  text (constants as hexadecimal literals). Events with the same statements
  share one `case`: a daily schedule is three bodies, not 252.
- *Linking, not compiling the kernel.* NVRTC has no standard library, and the
  kernel's headers use `<vector>`, `<string>`, `std::function`. So NVRTC
  compiles only the events, to LTO-IR; the kernel (`src/gpu/script_jit_kernel.cu`,
  the interpreter's kernel with `CompiledEvents` in place of the interpreter)
  is compiled by nvcc to LTO-IR at build time and embedded; **nvJitLink**
  links both with link-time optimisation, so the events are inlined.
  NVRTC and nvJitLink are linked statically (Debian's `nvidia-cuda-dev`,
  already in the image's build stage); the driver's functions come from
  `cudaGetDriverEntryPoint`, so the wheel does not link `libcuda.so`.
- *Specialisation.* The first version gained only 1.0–1.3×: `ncu` showed
  `no_instruction` at 9 of 16 cycles per instruction, the instruction cache
  thrashing on a 30 000-instruction kernel carrying every model and sampler.
  The generated source now also defines the request (model kind, assets,
  Sobol, stratification, importance sampling, antithetic) as `qm_spec_*`
  functions the kernel reads in place of its arguments; link-time
  optimisation drops the rest. Registers went from 128 to 60–70.
- *FMA.* With contraction on (nvcc's default, which the interpreter used),
  the specialised Heston kernel moved in the last bit: whether a multiply and
  an add fuse depends on how the optimiser sees the code. The script kernels
  are now built with `--fmad=false` on both sides (`script.cu`, the LTO-IR,
  NVRTC, nvJitLink `-fma=0`): the compiled script gives the interpreter's bits
  on every library script, crisp and fuzzy, on Black-Scholes, local vol,
  Heston and SLV, Philox and Sobol (`tests/gpu/testGpuScriptJit.cpp`). It
  costs 5–7 % on both paths and moves the GPU script results by an ulp
  against v1 (closer to the CPU, which does not fuse either).
- *Scope.* Prices without spot controls, which is the default; with spot
  controls, and for the duals and the adjoint, the interpreter runs and the
  diagnostics say so. `QM_SCRIPT_JIT=0` turns compilation off.
- *Cost.* 30–450 ms to compile a library script (NVRTC 75 %, link 25 %), then
  cached for the life of the process by its source.

Measured (`build-cuda/qm_script_jit_bench`, one V100, Philox + antithetic,
the reference's path counts; medians of five):

| Product | Interpreter | Compiled | Gain | Reference | Compiled / reference |
|---|---|---|---|---|---|
| Up-and-out, 52 weekly dates (BS) | 33.6 ms | 13.2 ms | 2.55× | 9.3 ms | 1.42 |
| Up-and-out, 52 weekly dates (local vol) | 39.1 ms | 16.7 ms | 2.34× | 11.6 ms | 1.44 |
| Arithmetic Asian, 252 dates (BS) | 65.3 ms | 35.1 ms | 1.86× | 22.3 ms | 1.57 |
| Arithmetic Asian, 252 dates (local vol) | 86.7 ms | 58.6 ms | 1.48× | 25.5 ms | 2.30 |

(The reference kernels are built with FMA on.) What remains is not the
script: at these path counts a launch has few logical blocks (Asian local
vol: 116 blocks of 256 threads for 80 SMs, 18 % of warps active), because a
thread runs its 16 units one after the other to keep the reduction tree.
Running the units in parallel and reducing them in the same order would keep
the bits; it is the next lever, for the interpreter as much as for compiled
scripts.

**Leads noted after P1b** (checklist in issue #207):

- *P1g, parallel units, same reduction*: the launch shape above. **Done**,
  see below.
- *P1h, explicit FMA* (**done**, see below): the script kernels stay `--fmad=false`, and the hot
  spots fuse where the source says so, through a `mul_add(a, b, c)` helper (a
  device `fma` for `double`; `a * b + c` on the host and for `Dual` and
  `aad::Number`) and a `horner(...)` built on it. For example
  `S *= exp(mul_add(vol_sqrt_dt, z, drift))` in the local-vol step, and
  Φ⁻¹'s polynomials as `horner(q, c1, ..., c6) / horner(q, d1, ..., d4, 1.0)`.
  About 20 lines added and 6–8 changed; the interpreter and the compiled
  scripts fuse at the same places, so the bit test stands. Profile first.
- *P1i, expensive functions* (first part done, AS 241 adopted, see below), after P1g and P1h, one at a time: Φ⁻¹ (Acklam
  plus a Halley step with an `erfc` and an `exp` on every draw: probably the
  largest single cost), the `exp` of each log-Euler step, the numeraire and
  discount `exp`s recomputed per event and per path though path-independent
  (a per-event table on the device keeps the bits), the `log`/`pow`/`sqrt`
  scripts write, and on the CPU the SLEEF decision. Changing Φ⁻¹'s
  algorithm changes the bits: the maintainer decides. P1h adds a lever: the
  script kernels are bound by the instruction cache, so a large body called
  from several places (an `erfc`, a model step) may gain more by being called
  than by being made cheaper.

### P1g — Parallel units, same reduction (added after P1b; done)

A logical block is 4 096 units on 256 threads; each thread ran its 16 units
in turn and added them as it went, which fixes the bits (ADR-G5) but leaves
a V100 under-occupied at the path counts that matter: about a million paths
is 116 to 280 blocks for 80 SMs.

Now two kernels (`src/gpu/logical_blocks.cuh`): `logical_units_kernel`, one
thread per unit, stores each unit's sample (a payoff, a control sample, a
dual); `logical_reduce_kernel` adds the stored samples in exactly the serial
kernel's order -- thread t, units t, t + 256, ... -- then the same warp tree
and warp order (`logical_block_reduce`, shared by both). Same additions,
same bits, sixteen times the threads in flight. It serves every script
request but the per-path adjoint (whose threads each accumulate a gradient
row over their 16 paths): plain, Sobol, stratified, spot controls, duals,
and the compiled scripts. The samples buffer is 4 096 samples per logical
block, the Sobol scratch 4 096 rows instead of 256; the launch size comes
from free memory as before. `ScriptGpuRequest::parallel_units = false` keeps
the serial kernel as the oracle: `tests/gpu/testGpuParallelUnits.cpp`
compares the two bit for bit (the GPU-vs-CPU tests compare to 1e-9 and could
not see a change of summation order; a deliberate swap of two samples in the
reduction makes it fail).

Measured (`qm_script_jit_bench`, as in P1b):

| Product | Interpreter, P1b → P1g | Compiled, P1b → P1g | Compiled / reference |
|---|---|---|---|
| Up-and-out, 52 weekly dates (BS) | 33.6 → 30.9 ms | 13.2 → 11.8 ms | 1.27 |
| Up-and-out (local vol) | 39.1 → 29.5 ms | 16.7 → 15.1 ms | 1.30 |
| Arithmetic Asian, 252 dates (BS) | 65.3 → 55.1 ms | 35.1 → 26.5 ms | 1.19 |
| Arithmetic Asian (local vol) | 86.7 → 57.6 ms | 58.6 → 32.1 ms | 1.26 |

**P1's GPU goal (within 1.5× of the reference) is met on all four.** At
eight times the paths the time grows linearly (barrier BS: 89 ms for 18 M
paths): the gain is at the path counts where the launch was short of
threads, and nothing is lost above.

### P1h — Explicit FMA, and Φ⁻¹ called rather than inlined (added after P1b; done)

The script kernels stay `--fmad=false` (P1b). `utils/fused.hpp` gives
`mul_add(a, b, c)` (an FMA on the device, `a * b + c` on the host) and
`horner(...)` built on it; Φ⁻¹'s polynomials and its Halley step use them.
On the host they are the expressions they replace: 54 CPU prices (three
models, PCG32, Philox, Sobol) are identical to 17 digits.

Measured at kernel level (`ncu`, clocks locked, one V100), changing one thing
at a time:

| Kernel | Base | Φ⁻¹ fused | Φ⁻¹ called, not fused | **Φ⁻¹ fused and called** | `--fmad=true` (ceiling of P1b) |
|---|---|---|---|---|---|
| Compiled, up-and-out BS | 12.09 ms | 11.39 | 11.00 | **10.42** | ~11.1 ¹ |
| Compiled, Asian local vol | 34.12 | 31.92 | 31.51 | **29.96** | ~30.3 ¹ |
| Interpreter, up-and-out BS | 29.29 | 29.26 | 25.62 | **27.00** | |
| Interpreter, Asian local vol | 61.30 | 62.86 | 57.16 | **60.21** | |

¹ from the end-to-end bench, not `ncu`.

What the measurements said:

- **The instruction cache, again.** Φ⁻¹, with its `erfc` and `exp`, was
  inlined at every draw site. Calling it instead (`QM_DEVICE_NOINLINE`, one
  copy of the code) gains 8–12 % on every script kernel by itself, more than
  automatic FMA did.
- **FMA on top** gains a further 5 % on the compiled scripts and costs 5 % on
  the interpreter. Fused and called beats the base on every kernel, and the
  compiled scripts serve prices by default: kept. Not fusing (called only)
  would keep v1's GPU bits and favour the interpreter (duals, controls,
  fallback); the measurements are here if that trade is ever preferred.
- **The model steps' multiply-adds** (BS, multi-asset, local vol, Heston)
  gained under 1 % on the compiled scripts and cost 3 % on the interpreter:
  not kept.
- Duals and the local-vol adjoint (`qm_gpu_risk_bench` kernels): within
  noise (−4 %, +6 %, −3 % on the duals; ±1 % on the adjoint).

End to end (`qm_script_jit_bench`, P1g → P1h): compiled 11.7 → 10.2 ms
(up-and-out BS), 15.0 → 12.7 (local vol), 26.6 → 23.9 (Asian BS), 32.0 →
28.2 (Asian local vol): **1.07–1.11× the hand-written reference paths**.
Compiled = interpreted bits on the whole library, as before.

### P1i — Expensive functions on the path (added after P1b; first part done)

Ablations first, to know what each function weighs (kernel time, `ncu`,
clocks locked, one V100, on P1h; the ablated code is never committed):

| Removed | Compiled, up-and-out BS | Compiled, Asian local vol | Interpreter, up-and-out BS | Interpreter, Asian local vol |
|---|---|---|---|---|
| The numeraire's `exp` per event | −7.6 % | −1 % | −5 % | −2 % |
| Φ⁻¹'s Halley step (`erfc` + `exp`) | **−37 %** | **−27 %** | −17 % | −16 % |

**Per-event tables (done, same bits).** `exp(r t)` for the numeraire and
`exp(-r (T - t))` for each `df()` do not depend on the path; script_path
computed them on every path at every event. `event_tables_kernel`
(`src/gpu/script.cu`) computes them once per request, with the same
expressions and the same device `exp`; prices read them
(`ScriptPathView::event_numeraire`, `event_disc`), the duals keep computing
them (they differentiate r). 48 GPU prices (eight scripts, BS, local vol,
Heston, Philox and Sobol) are identical to 17 digits. Compiled up-and-out
BS: 10.42 → 9.76 ms; the Asian local vol, without `df()` and dominated by its
local-vol step, does not move.

**Φ⁻¹: Wichura's AS 241, adopted by the maintainer on 2026-11-11.** The
rule of §12 allows another full-precision algorithm after a check against
high-precision values. Checked (`scripts/gen_inverse_normal_reference.py`:
45 289 points at 45+ digits with mpmath, from 1e-300 to 1 − 2⁻⁵³; a 7 374
point sample in `tests/data`):

| Algorithm | Region | Max error | Mean | Max relative |
|---|---|---|---|---|
| Acklam + Halley (before P1i) | \|x\| ≤ 3 | 36 225 ulp | 5.2 ulp | 6.2e-12 |
| Acklam + Halley (before P1i) | 3 < \|x\| ≤ 8.3 | 9.4 M ulp | 4.4 M ulp | **1.1e-9** |
| Acklam + Halley (before P1i) | \|x\| > 8.3 | 2 ulp | 0.4 ulp | 2.9e-16 |
| AS 241 | \|x\| ≤ 3 | 5 ulp | 0.8 ulp | 7.0e-16 |
| AS 241 | 3 < \|x\| ≤ 8.3 | 3 ulp | 0.7 ulp | 6.4e-16 |
| AS 241 | \|x\| > 8.3 | 6 ulp | 1.0 ulp | 7.5e-16 |

(The device versions, with their FMAs, measure the same.) **The current Φ⁻¹
is accurate to ~1e-9 only in its upper tail**: for p near 1 the Halley
correction `0.5 erfc(-x/√2) - p` subtracts two numbers close to 1 and
cancels, so the step adds nothing to Acklam's 1.15e-9; the lower tail is
fine. A defect, small on prices (about 0.13 % of the draws, at 1e-9), and
an asymmetry. AS 241 (`inverse_normal_cdf_as241`, transcription checked by
the paper's hash sums, accuracy pinned by
`InverseNormal.As241MatchesHighPrecisionReference`) is within a few ulps
everywhere, in one pass.

Its speed, measured before adoption by plugging it in place of the old Φ⁻¹:
compiled 9.76 → **6.23 ms** (up-and-out BS) and 29.95 → **21.64 ms** (Asian
local vol); interpreter 26.87 → 14.60 and 59.91 → 41.05. End to end the
compiled scripts would run at 7.1, 9.0, 16.3 and 20.8 ms on the four
products, under the hand-written reference paths (which use the same Φ⁻¹ and
would speed up as well).

Adopted: `inverse_normal_cdf` is AS 241, the Acklam + Halley code is gone.
What moved (the CPU's 54 reference prices of P1h): PCG32 none; Philox 13 of
18, by at most 5.3e-13 relative (7e-11 of a standard error: the old upper
tail's 1e-9 errors); Sobol 9 of 18, by at most 2.7e-15. The reference paths
share Φ⁻¹ and speed up too (`qm_gpu_reference_bench`: 9.3 → 6.0 ms, 11.6 →
7.5, 22.3 → 14.5, 25.5 → 15.9); against them the compiled scripts
(`qm_script_jit_bench`: 6.46, 8.84, 16.50, 20.59 ms) are at **1.08, 1.18,
1.14 and 1.29×**, P1's goal still met against a faster ceiling. CPU suite
1271/1271, GPU suite 61/61. Everything else that calls Φ⁻¹ (IRB capital,
SIMM, exposure quantiles) moves at the same level, towards the exact value.

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

**Measured, not done (2026-11-11).** By the ablations of P1a, a unit's
draws and model steps cost 20.4 µs on the daily Asian against 19.5 µs for
the whole hand-written reference: storing the scenario costs at most about
5 %, and on the GPU script_path already streams (it keeps only the current
spots). Not worth reworking the models' interface for now; to revisit if a
model with heavier samples (many assets, forwards) shows otherwise.

### P1e — Every CPU thread (added after P0)

No CPU Monte-Carlo pricing uses more than one thread (§2.1). The generic
engine already reduces through logical blocks (WP 19 G4) whose partials fold
in block order: running blocks on a thread pool gives the same bits on 1 or
56 threads. First the script engine's `simulate_impl` and `price_script`
(a `threads` argument, default the available CPUs capped as the xVA does),
then nothing else: the hand-written engines are retired by P3, not threaded.
Expected gain close to the thread count on the server's 28 cores; measured,
with the bits checked on 1, 16 and 56 threads.

**Done (PR #216, 2026-11-11).** Same bits on 1, 2, 16 and 56 threads
(`tests/testSimulationThreads.cpp`, also under ASan). Through the wheel, 2 M
paths: local-vol barrier, Philox, 10.46 s on one thread, 0.89 s on 16
(×11.7), 0.54 s on 56 (×19.4); Asian under BS, Philox, 18.8 s, 1.53 s
(×12.3), 0.82 s (×22.8). Sobol caps near ×12: one task per replicate, 16
replicates. Chunking the replicates (fixed chunk size, merged in order)
lifts the cap but changes the bits once: P1c (#214). PCG32, early exercise
and AAD stay on one thread. The C++ default is no pool, so the tests run
as before; `price_script(threads=0)` uses the available CPUs, at most 16.

### P1f — Cheap gaussians on the CPU (added after P0)

A path's CPU cost is dominated by its gaussians (§2.2): Philox + Φ⁻¹ costs
more than PCG32 + Box-Muller, and Sobol needs Φ⁻¹. Measure where a path's
time goes (`perf record` on the reference and the script runs), then make
Φ⁻¹ cheap on the CPU without changing its bits: branch-light evaluation of
the same rational approximation, batched by P1a's paths so that AVX2 runs
four at once.

**Done by P1i (2026-11-11).** Φ⁻¹ is Wichura's AS 241 on the CPU as on
the GPU: no erfc, no exp, no refinement, a log and a sqrt in the tails only.
The draws of a daily Asian unit then cost 7.1 µs (28 ns a gaussian, Philox
included), less than a quarter of the unit: not the bottleneck any more.
Batching Φ⁻¹ over P1a's lanes (AVX2) is left: the wheel is built without
-march=native, and the draws are no longer where the time goes.

### Decision point

**Settled (2026-11-11): P1a meets the target, no runtime C++ compilation.**

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

### 8.7 A second axis: approximate random variables (nested MLMC)

Added 2026-11-11, after P1h. The levels above differ by their time step. They
can also differ by **how the gaussians are made**: Φ⁻¹ exact on the fine
level, a cheap approximation on the coarse one, both applied to **the same
uniform** (Sheridan-Methven & Giles, *Approximating inverse cumulative
distribution functions to produce approximate random variables*, ACM TOMS
49(3), 2023, arXiv:2012.09715; analysis in Giles & Sheridan-Methven,
*Analysis of nested multilevel Monte Carlo using approximate Normal random
variables*, arXiv:2102.08164). The telescoping sum

```latex
\mathbb{E}[P^{\text{exact}}] = \mathbb{E}[P^{\text{approx}}]
  + \mathbb{E}[P^{\text{exact}} - P^{\text{approx}}]
```

is unbiased; the correction's variance is small because both payoffs share
their uniforms, so few paths pay for it. Their approximations are far coarser
than ours (piecewise constant or piecewise linear Φ⁻¹) and they report 5–7×
with Euler-Maruyama. The same telescoping idea with reduced *arithmetic*
precision corrected by a double-precision level: Brugger, de Schryver, Wehn,
Omland, Hefter, Ritter et al., *Mixed precision multilevel Monte Carlo on
hybrid computing systems*, IEEE CIFEr 2014; Omland et al. in *FPGA Based
Accelerators for Financial Applications* (Springer, 2015); Haas & Giles,
*A nested MLMC framework for efficient simulations on FPGAs*,
arXiv:2502.07123 (2025).

What it changes in §8.2: this axis does not need a discretisation bias, so it
applies **where time-step MLMC brings nothing** -- Black-Scholes products on
their dates, daily-monitored barriers. With a time-stepped model the two axes
combine (nested MLMC).

| Case | Time-step MLMC (§8.2) | Approximate Φ⁻¹ (this section) |
|---|---|---|
| BS, any product on its event dates | no gain | **candidate** |
| Local vol, Heston, SLV, rough Bergomi | yes | yes, nested with the time step |
| Discontinuous payoffs (digitals, barriers, autocalls) | variance decays more slowly | same: paths near the threshold flip between the two Φ⁻¹, smaller gain (the case the analysis paper treats) |
| Early exercise (LSM) | known in the literature, much harder | same; no bytecode for `exercise()` yet anyway |
| Jumps | out of P4's first cut | out |
| AAD greeks | per level | Φ⁻¹ acts on the uniforms, not the parameters: no obstacle in principle, to check |

The script analyser already knows a product's dates, discontinuities and
exercise: the engine should choose for itself whether a product goes through
MLMC (either axis) and say so in the diagnostics, as it announces its model
choice; the products it does not suit stay on plain Monte-Carlo.

Experiment for P4 (issue #210): approximate Φ⁻¹ plus its correction level,
measured on a BS vanilla (where time-step MLMC gains nothing) and on a BS
digital (the hard case), against plain Monte-Carlo at equal RMS error; the
estimator must equal plain Monte-Carlo within error, and the correction
level's variance decay must be measured, not assumed.

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

Time to reach a relative standard error of 1e-3 (`benchmarks/perf_matrix.py`;
pseudo-random runs at the number of paths that reaches it, Sobol runs at the
first power of two that does, "≤" when the first try of 16 384 points already
did). "v1" is the tag `v1-pedagogical` plus P0's fixes (`f2a4a48`):
`benchmarks/results/2026-11-11-f2a4a48.json`. CPU rows on one thread. The
reference column is `qm_gpu_reference_bench` (hand-written paths, Philox).

| Product (model) | v1 hand-written, CPU | v1 script, CPU PCG32 | v1 script, CPU Sobol | v1 script, 1 V100 Philox | v1 script, 1 V100 Sobol | Reference, 1 V100 |
|---|---|---|---|---|---|---|
| Vanilla call (BS) | 0.085 s | 0.176 s | ≤ 0.002 s | 0.002 s | ≤ 0.001 s | |
| Arithmetic Asian, 252 fixings (BS) | 29.5 s ¹ | 30.7 s ² | ≤ 0.404 s ² | 0.103 s ² | ≤ 0.050 s ² | 0.023 s |
| Arithmetic Asian, 252 fixings (local vol) | 60.3 s ¹ | 42.7 s ² | ≤ 0.544 s ² | 0.125 s ² | ≤ 0.050 s ² | 0.026 s |
| Up-and-out call, 52 weekly dates (BS) | 5.64 s (23.8 s with Greeks) | 8.25 s | 2.19 s | 0.020 s | 0.011 s | 0.010 s |
| Up-and-out call, 52 weekly dates (local vol) | 9.31 s | 12.2 s | 3.20 s | 0.041 s | 0.010 s | 0.012 s |
| Fixed-strike lookback, 52 weekly dates (BS) | 2.29 s ¹ | 1.96 s | ≤ 0.070 s | 0.009 s | ≤ 0.003 s | |
| Fixed-strike lookback, 52 weekly dates (local vol) | 4.66 s ¹ | 2.91 s | ≤ 0.094 s | 0.010 s | ≤ 0.003 s | |
| Basket call, 3 assets (BS) | 0.724 s ¹ | 0.482 s | ≤ 0.005 s | 0.003 s | ≤ 0.001 s | |
| Worst-of call, 3 assets (BS) | 0.827 s | 1.42 s | 0.011 s | 0.006 s | 0.002 s | |
| Best-of call, 3 assets (BS) | 0.163 s | | | | | |
| Autocall, 4 quarterly dates (BS) | ≤ 0.001 s | | | | | |
| Mountain range, 3 assets, 3 dates (BS) | 0.939 s | | | | | |
| Dispersion swap, 3 assets, 52 dates (BS) | 0.168 s | | | | | |
| Variance swap, 52 dates (BS) | 0.686 s | | | | | |
| Volatility swap, 52 dates (BS) | 0.178 s | | | | | |
| Quanto call (BS) | 0.072 s | | | | | |

¹ timed with the Greeks the engine always computes (pathwise; the Asian also
resimulates for theta). ² daily TARGET dates (~256), not the engine's 252
equal steps. Empty script cells: no script equivalent written yet (P3).

The CPU columns of the reference are not in the table: on one thread its
Philox + Φ⁻¹ gaussians make it slower than PCG32 + Box-Muller engines (§2.2),
so it is not a ceiling there until P1f.

Later lots add their columns (P1, P3, P4 MLMC, P5 MLQMC, P6 GPU).

## 12. Risks and open decisions

- **Precision and bias (decided 2026-11-11).** More paths reduce the
  statistical error, never a bias: an approximation that shifts every draw
  (a cheaper Φ⁻¹, reduced precision) is not paid back by simulating more.
  Rule: what changes neither precision nor bits is free (not inlining,
  per-event tables, compilation); another *full-precision* algorithm (e.g.
  Wichura's AS 241 PPND16 for Φ⁻¹) is allowed after a check against
  high-precision reference values; a genuinely approximate function enters
  only inside MLMC with its correction level (§8.7), never in the main
  estimator alone. The maintainer wants it explored there: MLMC exists to
  make such experiments safe.
- **Runtime compilation in production.** NVRTC must be in the runtime image
  (static `libnvrtc_static` exists since CUDA 11.5: check Debian's
  `nvidia-cuda-toolkit` 12.4 ships it). A CPU code generator would need a C++
  compiler in the image: decided at P1's decision point.
- **Bits and FMA.** Generated code must not contract across operations the
  interpreter keeps separate (`--fmad=false`, `-ffp-contract=off`). Done for
  the GPU script kernels in P1b, both sides: allowing FMA again (5–7 %) means
  giving up the same-bits test between compiled and interpreted scripts.
- **Wheel size.** NVRTC and nvJitLink are linked statically (P1b): the GPU
  wheel grows by their size. Loading them with `dlopen` from the toolkit's
  shared libraries would avoid it, at the price of shipping those in the
  image.
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
