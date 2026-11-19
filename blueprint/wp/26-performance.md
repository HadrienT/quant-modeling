# WP 26 — Performance: a fast generic engine, then multilevel Monte-Carlo with QMC

| | |
|---|---|
| **Status** | In progress: tracking issue #217 holds the state and the next step. |
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

**Follow-up (2026-11-14): the worst-of autocall reference.** The third
reference kernel, left out of P0: `qm_reference::worst_of_autocall` (three
assets, 8 quarters, the script library's Phoenix terms) and the same product
as a script (`benchmarks/reference/reference_scripts.hpp`). One CPU thread,
Philox + antithetic, 100 000 pairs: script 247 ms against 102 ms hand-written
(2.4x, the highest ratio of the five cases: eight dates, so the per-event
overhead is not amortised over steps). One V100: the reference reaches 1e-3
in 0.9 ms; the compiled script runs 2 000 000 paths in 3.5 ms (9.0 ms
interpreted), the same bits.

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
  share one `case`: a daily schedule is three bodies, not 252. (Since WP 27
  lot L1 the generator reads the script's IR, `scripting/ir_cuda.cpp`: the
  same operations, SSA values instead of stack slots, `if`s instead of
  `goto`s; the bytecode generator is gone.)
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

**Default switch done (2026-11-11, option (a) chosen by the maintainer:
every layer, C++ included).** `PricingSettings::mc_sampler`,
`price_script`, the dated Asian request and its preview default to Sobol.
The hand-written BS vanilla and Asian engines have a Sobol path and take
it; the others run pseudo-random and the registry's diagnostics say so
(`PricingRegistry::price`); a path of more draws than the generator's
21 201 dimensions runs pseudo-random with a note instead of failing. Jump
models keep Sobol: randomised QMC with a digital shift is unbiased for any
function of the uniforms (measured on Merton below); whether it gains there
is in #226. Pseudo-random noise had been hiding five things, which Sobol's
tighter errors showed in the CPU suite -- each checked against
pseudo-random at 4-16 M paths before deciding which side was wrong:

| Test | What it was | Done |
|---|---|---|
| Merton call against the closed form | **the test's formula**: Poisson weights of λ instead of λ(1+k) (0.34 % low) | fixed: 10.663048 against Sobol 10.662981 ± 0.000109 |
| Vasicek bond from the simulated integral | **a model bug**: Var[∫r \| r] subtracted Cov² instead of Cov²/Var[r] (4e-4 high at 7 years); the same in Schwartz-Smith's (χ, ξ) draw | fixed both; the Schwartz-Smith test's 5 % tolerance, which could not see its 4.6 %, is now 1 % |
| SLV against the local vol it was calibrated to | the calibration's own error, 4.4 % on a 90 put | tolerance at the measured error, #227 |
| LSM American call without dividends | the fitted rule exercises early, ~1 % low | tolerance at the measured loss, #228 |
| Turnbull-Wakeman arithmetic Asian | an approximation, 7.5e-4 off the exact price | tolerance allows it |

Four tests about pseudo-random itself (Philox, Sobol-vs-pseudo, the bridge's
convergence slope, the hard digital's bump noise) pin their sampler. The
vanilla convergence ladder of the site now falls at slope −0.86, not −1/2
(the page already says "faster than 1/√N").

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
- ~~path construction: Brownian bridge (today) against PCA of the Brownian
  increments (`etc/ROADMAP.md` §1.1 lists it), and, for several factors, which
  factor's coarse coordinates take the first Sobol dimensions~~ **measured
  2026-11-18 (#226): the bridge stays.** `brownian_pca_matrix`
  (src/utils/brownian_pca.cpp) builds A = V Λ^{1/2} of the covariance
  min(t_i, t_j), its components by decreasing variance and its signs fixed.
  It sits behind the internal setting `mc_brownian_pca`, on the plain engine
  CPU and GPU, with the same bits. Tested: A Aᵀ = C to 1e-12, the components
  sorted, the GPU's flat path equal to the generic engine's, the prices
  unbiased.
  - Protocol `qm_gpu_paths_bench 200` (49 scripts, hard, 2^16 and 2^19
    points): PCA is worse in 27 of 98 comparisons, better in 7, and never
    better at both point counts. The geometric mean of the variance ratio
    is 0.82.
  - It loses most on the payoffs that depend on the terminal value, which
    the bridge's first coordinate fixes exactly and PCA spreads over its
    components: European, quanto, discount certificate and reverse
    convertible ×2.5, call spread ×2 to ×2.4.
  - The Asian flips with the point count: ×1.9 worse, then ×1.85 better.
  - Cost: 1.6x to 3.4x the bridge's time per run in the median, up to 18x
    on long daily grids (O(n²) a path).
  - The factor order was not measured separately: the bridge's and PCA's
    both interleave the factors, so that the first coordinates fix every
    factor's terminal value (or first component).
- ~~randomisation: digital shift (today's decision, WP 19 §12) against a true
  Owen scrambling, random bits per node of the tree, which the hash of
  Burley only approximates~~ **measured 2026-11-18 (#226): no change, the
  digital shift stays.** `owen_scramble_nested` (utils/sobol.hpp) flips each
  bit with a coin of its own node, (1 << k) | prefix, from a full-avalanche
  hash (internal setting `mc_sobol_owen_nested`, the plain engine CPU and
  GPU, the same bits; tested nested, a net, fair coins independent between
  siblings). Two findings:
  - Burley's hash passes the same sibling test (fair, 50 % agreement at
    levels 1 to 20), so its approximation is not visible pairwise.
  - Protocol of WP 19 §12 (`qm_gpu_owen_bench 200 nested shift|hash`), 49
    library scripts, hard and fuzzy, 2^16 and 2^19 points. The five with an
    exercise() are left out, since the GPU needs a fitted rule.
    - Against the hash, it is indistinguishable: 92 of 98 comparisons, 2
      better, 4 worse, chance at a 95 % interval; geometric mean of the
      variance ratio 0.99.
    - Against the shift, it has the hash's profile: 24 better, 69
      indistinguishable, 5 worse (reverse cliquet ×2.5, capital-protected
      note ×1.9, phoenix ×2.0); geometric mean 1.12. The verdict still
      flips with the point count for one product (capital-protected note,
      phoenix).

    So a true Owen scrambling is not what was missing. There is no rule
    a priori, and the decision of WP 19 §12 stands. The setting stays
    internal, for a later protocol;
- ~~discontinuous payoffs lose most of QMC's edge: P2's smoothed indicators and
  bridge survival probabilities are what gives it back (barriers, digitals,
  autocalls)~~ **measured 2026-11-18 (#226): no default changes.** The
  library's barriers are monitored at the closes, so the bridge survivals,
  which price a continuous barrier, would make them another product. The
  lever for the products as written is the smoothed indicator (`fuzzy`).
  Protocol `qm_gpu_discontinuous_bench 200`: pseudo-random and Sobol, hard
  and fuzzy, 49 scripts, 2^16 and 2^19 points, local vol / correlated
  Black-Scholes.
  - Sobol keeps its edge on the scripts with a hard spot threshold: the
    variance is 11x below pseudo-random's (geometric mean, 42
    comparisons), against 78x on the others. The threshold costs a factor
    7, not most of the edge; the worst case is the double no-touch at 1.4x.
  - Smoothing never helps a Sobol run (0 of 42 significantly better), and
    makes some worse (himalaya ×2, worst-of autocall ×5).
  - Its bias at the default eps is large on several multi-asset and
    autocall scripts, in standard errors of one hard run at 2^19 points:
    worst-of autocall −537, worst-of reverse convertible +60, himalaya
    −72, phoenix −6.
  - So `fuzzy` stays what it is for: a usable pathwise delta, off by
    default.

  **Follow-up, the same day: the smoothing width measured, and a bug
  fixed.**
  - **Bench.** `qm_gpu_fuzzy_eps_bench`: the 17 single-underlying scripts
    with a spot threshold, local vol, 2^16 points, 16 seeds, eps from 0.001
    to 3. eps is the ramp's half-width in the units of the compared
    expression: about 1 for a performance, about 100 for a spot.
  - **What it measures:** the price bias in errors of one hard run, and the
    adjoint delta's bias and noise against a central difference of the hard
    price on common draws.
  - **The bug.** It showed adjoint deltas at zero on the barriers, even
    smoothed. The cause is the flag pattern: the fuzzy `if` blends `alive`
    to 0.97, and the discrete test `alive = 1` stayed hard. Discrete
    conditions now interpolate between their domain's values (WP 16 §4.2,
    a departure from the book). Hard prices are unchanged, and the tree,
    bytecode and IR agree to the bit.
  - **After the fix**, a well-chosen eps brings the delta's error from
    50-100 % (nearly hard) down to 1-6 % on almost every product.
  - **Rule per product.** eps minimises the delta's error. One run
    (`# greeks: joint`) when the price moves by less than half an error at
    that eps, or at a smaller eps whose delta error stays within 1.5x (or
    +2 points). Otherwise two runs (`separate`): the price unsmoothed, the
    greeks from a second, smoothed run.
    - eps 0.3, joint: accumulator, corridor variance swap, range accrual,
      barrier reverse convertible, ladder call.
    - eps 3, separate: digital call and digital put. They were joint
      under the forward start; started today, smoothing moves the price by
      3.7 errors under Black-Scholes. eps 0.001, joint: reverse convertible.
    - eps 1, separate: athena and phoenix autocalls, bonus certificate,
      double no-touch, one-touch, down-and-in put, shark-fin, twin-win.
    - eps 3, separate: up-and-out call, double barrier call.
  - **Not covered.** The four multi-asset performance scripts (altiplano,
    himalaya, worst-of autocall, worst-of reverse convertible) have no
    delta to calibrate: their payoff does not depend on today's spot under
    correlated Black-Scholes.
  - **Plumbing.** The API applies the width to a library product's adjoint
    greeks unless the request sets `fuzzy` or `default_eps` itself
    (`pricing_service._price_library_script`). The response's diagnostics
    say which mode ran.
  - **Limit.** Measured under one local-vol surface at the default terms,
    on the delta: the vega keeps a smoothing bias of a few percent
    (up-and-out, 11 %). The widths were measured before the products
    started today (the follow-up below), on the forward-start scripts; the
    bench is to be rerun on the started ones (#324).

**Decided and done (2026-11-11, measurements in #214).** R stays 16: at
equal total paths, R = 32 costs 1.4-2.3x in variance on smooth payoffs, and
with R = 16 the displayed error already covers the true price 92-98 % of
the time at ±1.96 (it overstates the true RMSE by 4-30 %). Points per
replicate are now the power of two nearest to paths / R on a log scale
(`sobol_points_per_replicate`, every Sobol path of the library: the generic
engine CPU and GPU, AAD CPU and GPU, the hand-written BS vanilla and Asian):
a truncated net (12 500 points at 200 000 paths) was 1.2-2.9x less
efficient per path on smooth payoffs, a barrier indifferent. The work stays
within [1/√2, √2] of the request; the diagnostics give the count. GPU
scratch per launch is capped at 1 GiB (it was 4.8 GB at once for the daily
Asian, 89 ms of a 131 ms call to allocate: compiled 134 -> 43 ms) and
`sobol_point_bits` is branchless (-2 to -5 % of the kernel). Not done, by
measurement: chunked replicates beyond 16 threads (the production container
has 2 CPUs) and Gray-code stepping on the GPU (a thread computes one point
since P1g). Left: the scratch's memory traffic, which a bridge built in time
order with O(log n) memory per path would remove.

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

**WP 19 §9.1 measured again (2026-11-11, `ncu`, one V100, P1a's tree):**

| Kernel | Registers | Occupancy, theoretical / achieved | `no_instruction` stall (cycles per issued instruction) |
|---|---|---|---|
| Interpreter, up-and-out BS | 126 | 25 % / 24 % | 2.6 |
| Compiled, up-and-out BS | 64 | 50 % / 49 % | 0.4 |
| Interpreter, Asian local vol | 126 | 25 % / 25 % | 0.6 |
| Compiled, Asian local vol | 62 | 50 % / 48 % | 0.9 |

WP 19 §9.1 had 128 registers, 25 % occupancy and 12–15 cycles of
`no_instruction` per issued instruction; P1b's first compiled kernel still
had 9. Compilation with specialisation halved the registers, and Φ⁻¹ called
rather than inlined (P1h) then AS 241 (P1i) emptied the instruction-cache
stalls.

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

**Follow-up done (2026-11-16): the bridge survivals on the GPU.** P2's
survivals ran on the CPU only, waiting for WP 27's IR. They are now in its
paths and in the compiled kernels, prices and risks, under Black-Scholes and
local vol (WP 27 §14.12): a local-vol knock-out call at 200 000 points, 3.6 s
on two CPU threads → 0.016 s on one V100, with AAD risks 14.3 s → 0.05 s.
Barrier monitoring under Heston, SLV and several assets stays refused, by the
CPU models as by the IR.

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

**The local-vol endpoint's vanilla (follow-up, 2026-11-12).**
`price_local_vol_mc`, the Monte Carlo of the API's `/local-vol/price`, was a
hand-written log-Euler loop outside the registry (`engines/mc/local_vol.cpp`,
PCG32 + Box-Muller, antithetic, five bumped re-simulations for the greeks).
It is now a script route (`pricers/adapters/equity_vanilla_lv.cpp`, builder
`script_builders::european`): the same scheme and steps (local vol read at
each step's start), Sobol, the GPU when present. The retired loop stays in
`tests/testLocalVol.cpp` as the oracle on a skewed surface; parity and the
grid's parallel-shift vega are checked there too. Same inputs as the API's
defaults (50 000 paths, 252 steps a year, one year, K = 95, skewed surface),
one V100:

| | Price | Std error | Time with greeks | Price only |
|---|---|---|---|---|
| Retired loop (CPU, one thread) | 12.403 | 0.063 | 3.3 s | 0.56 s |
| Script route (GPU, Sobol) | 12.468 | 0.0017 | 0.07 s (0.62 s cold) | 0.006 s (0.54 s cold) |

The cold times are the kernel's compilation (NVRTC), paid once per shape and
process; WP 27's lot L1b removes it. Left in `src/engines/mc/` outside the
generic engine: the plain vanilla's `BSEuroVanillaMCEngine`, the GPU's
reference kernel.

**The American option by Longstaff-Schwartz (follow-up, 2026-11-18, #313).**
The maintainer's decision: the site offers Analytic and Monte-Carlo only, so
the American, priced by the binomial and trinomial trees until then, becomes
a script route (`pricers/adapters/equity_vanilla_american.cpp`, builder
`script_builders::american`, `EngineKind::MonteCarlo`). The trees stay in the
registry, for the portfolio's daily marks and as the oracle
(`tests/testAmericanLsm.cpp`).

- A Bermudan on `exercise_per_year` dates a year (50 by default) stands for
  continuous exercise; the rule is Longstaff & Schwartz's
  (`engines/mc/lsm.hpp`), fitted once at the route's model and held fixed in
  every run, the bumped ones and the adjoint included (`price_by_script`,
  which had no early exercise before).
- The script gates `exercise()` with a condition every optimal exercise
  meets: the payoff in the money and above the forward held to the expiry,
  S e^(-q tau) - K df(T) for a call. The generic rule otherwise fits every
  path whose value exercising would change -- an out-of-the-money put that
  ends in the money among them -- and was 0.7 % low on an at-the-money put;
  on a call with r = 5 %, q = 4 %, whose premium is 0.015, it exercised below
  rK/q and priced under the European (8.081 against 8.103).
- A call with no dividend and r >= 0 is the European, priced as such
  (Merton 1973): exact, where any estimated rule loses value.
- The pilot is quadratic in the number of dates (each path re-run on every
  date it reaches): 2.4 s on one thread at 50 dates, 0.4 s on the API's
  threads (`PricingSettings::mc_threads`, 0 = the available CPUs, at most 16;
  the C++ default stays 1), the same bits.

S = K = 100, T = 1, r = 5 %, sigma = 20 %, 100,000 Sobol paths, against a
4,000-step binomial tree:

| | European | Tree | LSM | Gap |
|---|---|---|---|---|
| Put, q = 0 | 5.5735 | 6.0902 | 6.0763 ± 0.0053 | -0.23 % |
| Put, q = 4 % | 7.1466 | 7.3055 | 7.2946 ± 0.0054 | -0.15 % |
| Call, q = 4 % | 8.1026 | 8.1178 | 8.1009 ± 0.0058 | -0.21 % |
| Call, q = 8 % | 6.1430 | 6.5419 | 6.5157 ± 0.0061 | -0.40 % |
| Call, q = 0 | 10.4506 | 10.4501 | 10.4506 (European) | 0 |

The gap is the Bermudan's and the estimated rule's, both lower bounds for the
holder. The tree's gamma is no oracle: on the European call it gives 0.0206
where the closed form gives 0.01876 (the script route: 0.0188).

**One catalog of scripts, Analytic and Monte-Carlo only (follow-up, 2026-11-18, #313).**
P3 moved every Monte-Carlo route onto scripts but left the site's catalog in
two halves: the twelve hand-written pricers next to the 46 scripts of the
library, nothing deduplicating them, so the Price page listed the same
product twice (European option and European call, Barrier option and
Up-and-out call, ...). The maintainer's decision: the scripts are the
catalog; the hand-written pricers leave the site and stay in the library,
the API and the tests as oracles. Three lots:

- **b1 (this pull request).** The Price page lists the library's scripts
  (`web/src/shared/products/catalog.ts`: the hand-written equity entries are
  `PORTFOLIO_ONLY`, an old link to one opens the script that replaced it).
  The library gains an **American put** (`american-put.qms`: weekly
  `exercise()`, Longstaff & Schwartz on the moneyness spot / s0, considered
  only in the money; 0.38 % under a 4,000-step tree under Black-Scholes --
  on the spot alone, which ignores that the strike moves with s0, it was
  1.5 % under) and a **quanto call** (`quanto-call.qms`, header `# quanto:
  USD`: one underlying in its listing currency on `underlyings`, paid in
  dollars; greyed out on the page until data-ingest stores the dividend
  yields of non-US listings, which only ten US tickers have today). No
  American call: on a dividend payer the bound that keeps the estimated rule
  above the European needs the dividend, which a script does not see; on a
  non-payer it is the European call. **Analytic** is offered where a closed
  form exists (`engine: "analytic"` on `/price/scripted-product`,
  `ANALYTIC_SCRIPTED`): the European and the digital call, whose strike the
  library fixes on the start date t1, so forward starts --
  S0 e^(-q t1) C(1, k, T - t1) and P e^(-rT) N(d2) -- at the at-the-money
  implied vol of the ticker's smile, equal to the script's Monte-Carlo under
  that flat vol within its error (`api/tests/test_scripted_analytic.py`).
  The site's engines are Analytic and Monte-Carlo only.
- **b2a.** The bonds and the future as scripts. A library header
  `# market: rates` marks a product with no stock: it is priced by Hull-White
  fitted to the chosen currency's government curve (USD, EUR or JPY, those the
  database derives), the hybrid model's stock unread -- the request asks for
  no ticker and refuses one. The zero-coupon and the semiannual fixed-rate
  bond: Analytic is the script valued with no short-rate volatility (its
  flows deterministic, each discounted by the curve -- the curve's PV exactly,
  for any payoff linear in it), Monte-Carlo agrees within its error (USD
  2026-11-16: 95.7196 against 95.7201 ± 0.0004 for the 4 % five-year bond).
  The future becomes the library's equity forward, with a forward-start
  closed form (AAPL: 12.7414 against Monte-Carlo 12.7408 ± 0.0042). Their
  hand-written entries leave the catalog (the future stays a portfolio form),
  old links open the scripts.
- **b2b.** The swap and the swaptions as scripts on the market the rates
  workbench already priced on: a header `# curve: sofr` puts a rates product
  on the USD SOFR curve bootstrapped from the swaps DTCC published (one
  curve: SOFR discounts and projects), with Hull-White `(a, sigma)`
  calibrated to the ATM normal vols implied from the swaptions traded over
  the last ten days (`rates_derivatives.sofr_hull_white`, the same quotes and
  the same calibration as the `usd-sofr` set; 2026-11-17: a = 0.048,
  sigma = 120bp, 5bp RMSE on 20 points). `hull_white.curve = "sofr"` on the
  request; mean reversion and sigma are the calibrated ones unless given.
  Three scripts: a five-year payer swap (annual `fwdRate()` against the
  fixed rate), a one-year-into-five European swaption (the swap's value at
  expiry from `df()`, which reads the simulated bond prices) and the
  co-terminal Bermudan (yearly `exercise()`, Longstaff-Schwartz on the
  swap's value, in the money only). Their oracles price the same model on
  the script's own dates (`qm.hull_white_swap_on_dates`): Analytic for the
  swap is the curve's value of its legs; for the European it is the
  Hull-White closed form (191,794.5 against Monte-Carlo 191,815.6 ± 33.1 on
  2026-11-17); the Bermudan's lattice is a test oracle, Longstaff-Schwartz
  within 1.5 % under it. Analytic gives DV01 (every SOFR par quote +1bp, the
  curve rebuilt, `(a, sigma)` held), its gamma and the vega to sigma. The
  hand-written swap and swaption entries leave the catalog (old links open
  the scripts), along with their quote editor and the illustrative EUR
  two-curve set. Their results become the Diagnostics of the scripts: the
  same trade on the rates engines on a regular grid, with the curve, the
  calibration point by point, the cash flows, the swaption under Bachelier,
  Black and SABR, and the Bermudan on the lattice. 
- **b2c. Curve adjoints.** The Hull-White hybrid
  (`models/hybrid/hull_white_equity_sim_model.hpp`) differentiates the short
  rate's sigma (`rate_vol`, a held) and its curve on the tape. With a fixed,
  the step's moments scale exactly: the drifts and y(t) as sigma², the rates
  rows of the Cholesky factor as sigma. So they are computed once at
  sigma = 1 and scaled on the path, and the simulation stays exact. The
  curve enters only through log P(0, ·) at known dates, which is linear in
  the pillars' log discount factors (`DiscountCurve::log_discount_weights`).
  The model takes `CurveFactors`, a basis of zero-rate moves, one parameter
  per column. The API passes the bootstrap's Jacobian dz/dq for the SOFR
  curve, so each risk is to a par swap rate (a bucketed DV01). For a
  government curve it passes the identity, so each risk is to a pillar's
  zero rate. Both get a parallel column, and every risk comes with its own
  Monte-Carlo error. That is why the map is built into the model, unlike
  xVA lot X8, which applies its Jacobians batch by batch: the script engine
  reports no per-batch risks. 2026-11-17, 100,000 paths, against the closed
  form's bumps: swap DV01 4,369.42 ± 0.02 vs 4,369.43; European swaption
  DV01 2,075.07 ± 0.44 vs 2,074.96, vega 1,542.76 ± 0.12 vs 1,542.75 per bp
  of sigma. The Bermudan gets them too, by Longstaff-Schwartz. Rates scripts
  request `greeks_method = "aad"`. The response drops the hybrid's unread
  stock; rho and vega are the parallel and sigma risks. Not differentiated:
  the mean reversion, nor the recalibration of (a, sigma) when the curve or
  the vols move (xVA does both). DV01 holds (a, sigma) fixed, as the closed
  form's bump does.
- **b3. The portfolio books scripts** (decided by the maintainer on
  2026-11-18: positions converted, closed form where there is one, rates
  scripts included).
  - **Booking.** A position (`ScriptSpec`) holds the product's script,
    rendered with its terms on the trade date (`lead_days = 0`: the first
    date falls in the week of the trade), so its dates are the contract's
    from then on. `POST /api/portfolio-valuation/script/quote` books it and
    values it that day, which is the default price. "Add to portfolio" is
    offered from every single-underlying script and every rates script.
    Products on several underlyings and quantos are refused, because no
    correlated history is stored for them.
  - **Marks** (`portfolio_scripts.py`, one per day, stored once).
    - Past fixings are replayed on the stored closes.
    - A past exercise date is a right not used, a new engine setting
      `ScriptSettings::past_rights_unexercised`: the event is skipped, where
      it used to be refused.
    - The flows already paid stay in the mark, not compounded. The engine
      now reports them (`ScriptedProduct::historical_payments`), because
      the ledger has no cash account; this is the convention of the rate
      trades.
    - A rates script's past events that read the curve are taken out (its
      terms kept), and what they paid is valued on the curve of their own
      date (`season`).
    - Closed form where there is one. Converted positions use their
      hand-written pricer (`closed_form`). The European, the digital and the
      forward use the vanilla on the smile, the digital and cost of carry
      once their strike is fixed. Bonds and the swap use the day's curve.
      The European swaption uses Hull-White's closed form on the day's SOFR
      curve and calibration.
    - Monte-Carlo otherwise, on the desk policy's model (stochastic-local
      vol, local vol, Black-Scholes at the realised-vol proxy; Hull-White
      calibrated to the day's swaptions), with the same draws every day.
  - **Conversion.** Portfolio version 3: on read (`migrate`), each
    hand-written derivative becomes the script of its payoff, its terms kept
    as its closed form, so it marks as before. In production that is 3
    European vanillas, plus the demo portfolios.
  - **xVA.** A converted European stays the netting set's equity option. A
    SOFR script is a scripted trade of the exposure engine, seen from the
    day: its past events taken out, their terms carried (`for_exposure`).
    The rest is left out, and the response says so.
  - **Front end.** The hand-written equity and bond descriptors are deleted
    (`catalog.ts`, `portfolioProducts.ts`, `DerivativeFields`), and so is
    the payoff profile that served them. The trade form books library
    products (`ScriptFields`), and a position links back to its product on
    the Price page.
  - Tests: `tests/testScriptSeasoned.cpp`, `api/tests/test_portfolio_scripts.py`,
    `script-booking.integration.test.tsx`.

**Follow-up (2026-11-18, the Price page's products).** Decided by the
maintainer:
- **Products start today.** A library product on a stock now starts on
  the valuation date, its first fixing today's spot
  (`product_templates.start_on`, the `historical_fixings` of
  `price_script`). Its levels in % of the initial level are therefore % of
  today's spot: a true European, not a forward start a week ahead. Its
  delta is the one a desk quotes (the European call's moved from 0.08 to
  about 0.55).
  - The closed forms are plain Black-Scholes at a fixed strike: European
    and digital calls and puts, and the forward.
  - The library gains the European put and the digital put.
  - A rates product keeps its start a few days ahead, because its first
    coupon reads the curve, which a past date cannot.
- **A maturity on every product.** Equity scripts get one `maturity`
  field: their dates stretch from the start (`product_templates.stretch`),
  and a schedule keeps its frequency. A swap or a swaption is written for
  the asked length in whole years, with an `expiry` for the swaptions
  (`RATES_GENERATED`).
- **Page.** Payer or receiver is a menu (param unit `side`). Tickers come
  from a menu of the database's markets (`shared/market/TickerSelect`).
  The device is Auto by default. Multi-asset products default to 20,000
  paths: their local-vol adjoint runs on the CPU.
- **Typed inputs** (2026-11-19). Every product can be priced on inputs
  typed by hand, with nothing read from the database: a fictitious
  underlying or market. The Price page's "Market inputs" switch offers it.
  - One underlying: spot, vol, dividend and rate under flat Black-Scholes,
    the closed forms included.
  - Several underlyings: per-underlying spot, vol and dividend, plus one
    correlation for every pair.
  - Rates: a flat curve at `rate`, with Hull-White's a and sigma
    (`hull_white.curve = "flat"`), the closed forms included.
  - In typed mode the page hides the model comparison, the market vega and
    "Add to portfolio", and labels the result's provenance "manual".

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

### 8.6a P4a: time-step MLMC in the generic engine (done, 2026-11-12)

Sub-lots of P4: **P4a** the engine (this section); **P4b** Milstein for local
vol and the complexity slope; **P4c** scripts and registry routes through
MLMC when the product suits it, announced in the diagnostics; **P4d** the
approximate-Φ⁻¹ axis (§8.7).

`engines/mc/mlmc.hpp`: `simulate_mlmc` (Giles's adaptive algorithm, §8.4,
M = 2) and `simulate_mlmc_fixed` (given paths per level, for tests and
measurements), CPU, one thread, PCG32 + Box-Muller, the payoff of the
product's first label.

**Departure from §8.3.** The model does not generate the coupled fine and
coarse paths. It only accepts a refinement (`ISimulationModel::
set_refinement(r)`: every step of its own grid split into r equal steps,
`refine_steps` in `core/timegrid`), and the engine holds two clones, at 2^l
and 2^(l-1), and sums the fine Brownian increments over each coarse step,
through `brownian_layout()`. Why: the model stays unaware of MLMC, any model
that describes its increments couples without a line of its own, and r = 1
leaves every existing bit as it was. The grids nest exactly (a step divided
by a power of two), so the coupling matches dates with a 1e-12 tolerance,
not `TIMELINE_EPS` (an hour, longer than a fine level's step);
`event_indices` now takes the closest step to an event for the same reason
(no change on grids whose steps are a day or more). Models that accept a
refinement: local vol (log-Euler). Black-Scholes refuses (no time-step
bias); Heston, SLV and the multi-asset models come with P4b/P4c.

Tests (`tests/testMlmc.cpp`): nested grids; a flat surface's correction
levels are zero to rounding (the coupling's sharpest check); the telescoping
sum equals plain Monte Carlo on the finest grid within error; the §8.6
Euler-GBM test bed (Euler on S, E[S_T] = S0 (1 + r h)^n known): alpha
= 1 ± 0.15, beta = 1 ± 0.25, each level's mean the analytic difference of
biases, the adaptive run reprices the forward within 3 eps; local vol:
beta ~ 1, gamma = 1. The weak error of log-Euler under the test's skewed
surface is too small (~0.009 on level 1) to measure alpha without ~1e8
paths: the test bed measures it.

Measured (one thread, skewed local vol, one-year call K = 95, monthly level
0; cost in model steps; plain = 2 V_0 / eps^2 paths on the finest grid
MLMC chose):

| eps | Finest level | MLMC cost | Plain cost | Gain | Time |
|---|---|---|---|---|---|
| 0.04 | 2 | 5.9e6 | 1.3e7 | 2.2× | 0.38 s |
| 0.02 | 4 | 3.6e7 | 2.1e8 | 5.8× | 2.2 s |
| 0.01 | 4 | 1.4e8 | 8.3e8 | 5.8× | 9.0 s |
| 0.005 | 7 | 9.5e8 | 2.7e10 | 28× | 58 s |

Open for P4b (done, §8.6b): alpha estimated by regression over noisy level means swings
(0.66 to 2.2 in the runs above), and the bias test divides by 2^alpha - 1; a
large estimate stops adding levels early (eps = 0.01 stopped at level 4 with
alpha 2.2). Giles's code has the same estimator; Milstein (beta = 2) and a
floor or prior on alpha are the candidates, decided on measurements.

### 8.6b P4b: Milstein for local vol, a robust alpha (done, 2026-11-12)

**Milstein.** `mc::local_vol_milstein_step` (`models/equity/path_steps.hpp`)
steps the log-spot X = ln S with the Milstein term of its diffusion
b(X) = σ(e^X, t), b' = S ∂σ/∂S:
X += (r − q − σ²/2) dt + σ √dt z + ½ σ (S ∂σ/∂S) dt (z² − 1). The slope comes
from the Dupire grid's bilinear lookup (`mc::local_vol_with_slope`: the
strike interpolation's slope blended in time, 0 outside the strike grid and
where the floor binds). Opt-in (`LocalVolSimModel::set_milstein`,
`ScriptModelSpec::milstein`): log-Euler stays the default, no bit moves. CPU
only (the device steps log-Euler; `describe_device` refuses), and the
deflated spot is no longer an exact martingale on the grid
(`deflated_spot_mean` refuses, so the generic spot controls stay off).

**alpha.** The regression now uses only the levels whose sample mean is
resolved (|mean| > 2 standard errors), 1 (the weak order of Euler and
Milstein) when fewer than two are. P4a's swings (0.66 to 2.2) came from
regressing on means lost in their noise.

Measured (`tests/testMlmc.cpp`):

- smooth surface (σ linear in S, no grid node on the paths' range):
  beta = 2.00 with Milstein, 1.02 with log-Euler;
- Milstein-GBM test bed: MLMC cost against eps in log-log, slope −2.01
  (Giles 2008, Theorem 3.1: O(eps^-2) when beta > gamma);
- the toy skewed grid of §8.6a (5 strike nodes, 80 % to 10 % vol), whose
  bilinear interpolation has large kinks at the strike nodes (∂σ/∂S jumps):
  beta = 1.66 with Milstein. The production grid (100 x 50, from SVI) is not
  measured yet (§9.1). Gains over plain
  Monte Carlo at equal error, one thread:

| eps | log-Euler MLMC | Milstein MLMC |
|---|---|---|
| 0.02 | 2.2× (1.6 s) | 10.5× (1.2 s) |
| 0.01 | 28× (14.8 s) | 20× (5.1 s) |
| 0.005 | 28× (59 s) | 38× (22 s) |

With the robust alpha, log-Euler goes to level 7 at eps = 0.01 instead of
stopping at 4 on a noisy estimate: dearer, but its bias is controlled.
Milstein's level-0 bias is larger (0.078 against ~0.002 for log-Euler on
level 1) and resolved, which is what makes its alpha stable (0.94–1.17).

Left: Milstein for Heston and SLV (the doc's later decision, §8.5); beta on
the production Dupire grid, to measure before any change to how σ_loc is
read (§9.1).

### 8.6c P4c: MLMC opt-in through the script engine (done, 2026-11-12)

The maintainer chose opt-in (2026-11-12): the CPU pseudo-random MLMC is
slower than the default Sobol/GPU route until P5/P6, so no default route
moves; what it brings today is a controlled discretisation bias.

- `PricingSettings::mc_mlmc_eps` (0: off), read by `simulate_script`. The
  engine checks that the script suits it -- no early exercise, one payoff
  label, a model that refines its grid -- and otherwise runs the usual route,
  adding "(no multilevel Monte Carlo: <why>)" to the diagnostics. The
  diagnostics of an MLMC run give the levels, paths per level, alpha, beta,
  gamma, the target and the estimated remaining bias.
- Bindings: `price_script(..., mlmc_eps=0.0, milstein=False)`; under AAD
  greeks the plain engine runs, and says so.
- API: `ScriptRequest.mlmc_eps` and `ScriptRequest.milstein` (price only;
  `n_paths`, `sampler` and `device` are then ignored). The front end does not
  expose them yet.
- Registry routes (`price_by_script`) are unchanged: their greeks need the
  adjoint and the common-number differences, which MLMC does not give yet.

Left in P4: the Dupire vanillas repriced within eps on a calibrated surface
(acceptance of §8.6), Heston/SLV refinement, greeks per level, P4d (§8.7).

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

**Measured first (P4d, 2026-11-12): not worth doing on our paths.** The gain
is bounded by Φ⁻¹'s share of a path. CPU, one thread, on the most favourable
case, a Black-Scholes vanilla (one draw per path, an exact step):

| | ns |
|---|---|
| Φ⁻¹, AS 241 (since P1i) | 12.5 per draw |
| Φ⁻¹, piecewise-linear table, 1024 cells (the papers' kind of approximation) | 6.9 per draw |
| One path through the generic engine, Sobol | 92 |

A free Φ⁻¹ would save 13.5 % of the path, the table 6 %, before paying the
correction level. The papers' 5–7× came from an Euler step whose inverse CDF
dominated the cost; AS 241 (P1i) removed that on the CPU, and on the GPU the
Halley step it replaced was the 37 % of the P1i ablations.

**The full experiment, run anyway at the maintainer's request (2026-11-12).**
`engines/mc/approx_inverse_cdf.hpp`: `PiecewiseLinearInverseCdf` (equal cells
of p in [1/2, 1), mirrored; the last cell calls the exact Φ⁻¹, so the
approximation stays bounded) and `simulate_approx_inverse_cdf` (the
approximate level, the correction on the same uniforms, and plain exact
paths, each timed). Gain = plain cost over two-level cost at equal RMS
error with the optimal split, costs in wall time per sample (one thread,
Black-Scholes, PCG32 uniforms):

| Product | 256 cells | 1024 cells | 4096 cells | Correction variance, 256 → 4096 |
|---|---|---|---|---|
| Vanilla call | 1.15× | 1.17× | 1.12× | 4.2e-4 → 2.8e-5 (price variance 217) |
| Digital (the "hard" case) | 1.17× | 1.24× | 1.19× | 0: no path flipped in 1e6 |
| Arithmetic Asian, 52 weekly dates | 1.06× | 1.08× | 1.09× | 6.7e-5 → 3.0e-6 (price variance 65) |

The two-level estimator equals plain Monte Carlo within error (tested). The
digital is not hard here: the table's error near the threshold is ~1e-7, so
paths do not flip; the correction is free, and the gain still stops at the
13.5 % bound. Tests (`tests/testMlmc.cpp`): the table's accuracy and exact
tail, the estimator unbiased, the correction's variance falling with the
cells; no timing assertion (#223 is a flaky one).

Conclusion: **8–24 %, not 5–7×**. Not wired into any route. To revisit only
where draws dominate a path again (reduced precision on the GPU, many
factors).

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

### 9.1 P5 done (2026-11-12)

`engines/mc/mlqmc.hpp`: `simulate_mlqmc`. P4's levels and coupling
(`LevelSampler`, whose sampling and evaluation are now separate), each level
drawing points of R = 32 randomised Sobol sequences of its fine dimension,
through a Brownian bridge on the fine grid (`BridgedGaussians` from
`brownian_layout()`); the level's estimate is the mean of its replicate
means, its variance their sample variance over R. N_l starts at 16 points a
replicate and doubles on the level with the largest V_l / (C_l N_l) until
sum V_l <= eps² / 2; the bias test is P4's. A level that needs more draws
than the 21 201 Sobol dimensions is refused (`InvalidInput`), and the
script engine's opt-in then falls back to MLMC and says so: the padding with
pseudo-random dimensions of the design is not done.

The opt-in of P4c (`mc_mlmc_eps`) now runs multilevel QMC when the sampler
is Sobol (the default) and MLMC when it is pseudo-random.

Measured, one thread, Milstein local vol, cost in model steps:

| Case | eps | MLMC cost (time) | MLQMC cost (time) | Slopes, MLMC / MLQMC |
|---|---|---|---|---|
| Toy skewed grid (5 strikes, 80 % to 10 % vol), vanilla K = 95 | 0.005 | 3.2e8 (21.6 s) | 1.5e7 (0.89 s) | −2.07 / −2.11 |
| Same toy grid, Asian on 12 monthly fixings | 0.005 | 1.5e8 (14.6 s) | 9.8e6 (0.70 s) | −2.05 / −2.17 |
| Smooth surface (σ linear in S), vanilla | 0.0025 | 5.0e9 (306 s) | 6.8e6 (0.41 s) | −2.00 / **−1.01** |

(slopes over eps = 0.04 … 0.005, and 0.02 … 0.0025 for the smooth surface;
prices equal within error in every row.)

Acceptance: met on the smooth surface (slope −1.01 against −2.00, better
than Giles & Waterhouse's ~−1.5); not met on the toy skewed grid (5 strike
nodes, extreme skew), where MLQMC is a constant 15–25× cheaper than MLMC
with the same slope.

### 9.2 Measured on SPY's production grid (2026-11-12)

The grid the API calibrates (`market_snapshot.local_vol_market`, snapshot of
2026-11-11): SVI slices through Dupire, sampled on 100 strikes x 50
maturities, spot 759.8. One-year ATM call and a 12-fixing monthly Asian,
monthly level 0, one thread.

**The interior bilinear kinks are not the problem.** The jump of S ∂σ/∂S
from one strike node to the next is 0.02–0.03 at 6 months and 1 year, 0.14
at 3 months. **The strike grid's edges are.** It covers only 608–848, −20 %
to +11 % of the spot (the configured ±0.6 log-moneyness is cut to the quoted
range); at its left edge σ_loc is already 36–53 % with S ∂σ/∂S between −1.1
and −2.3, and the flat extrapolation beyond drops that slope to 0 -- a large
kink many one-year paths cross.

| | beta, log-Euler | beta, Milstein |
|---|---|---|
| Vanilla | 0.85 | **0.41**, level variances erratic (49, 77, 586, 19, 24): heavy tails |
| Asian | 1.04 | 0.91 |

Milstein's term ½ σ (S ∂σ/∂S) dt (z² − 1) is large where the slope is steep
and switches off past the edge, so fine and coarse paths on either side of it
get very different corrections: **on market grids Milstein makes the coupling
worse**. With it, MLQMC lost its advantage on the vanilla (cost 2.8e7
against MLMC's 1.25e7 at eps = 0.2) and its prices drifted from MLMC's by
1.5–2 standard errors. With log-Euler:

| Log-Euler, SPY | MLMC | MLQMC | Slopes, MLMC / MLQMC |
|---|---|---|---|
| Vanilla, eps = 0.1 (0.19 % of the price) | 1.9 s | **0.22 s** | −1.58 / −1.22 |
| Asian, eps = 0.1 | 7.2 s | **1.2 s** | −2.01 / −2.05 |

Prices agree within error throughout. On the vanilla the levels stop at 2
(weekly steps): SPY's time-step bias is below eps / sqrt(2) there, most of
the gain is QMC's.

Conclusions: (1) for market grids, MLQMC with log-Euler (6–9× over MLMC);
Milstein stays opt-in and is not advised there, said in the API's
description; (2) the narrow, flat-extrapolated strike range is a property of
the local-vol calibration, not of MLMC, and affects every local-vol price
whose paths leave it: a smooth extrapolation of σ_loc beyond the quoted
strikes (continuous slope, decaying to flat) would serve both, to decide
with the calibration's owner; (3) β = 2 needs a σ smooth across the paths'
range, as the smooth surface showed.

Owen's hash scrambling against the digital shift: no consistent gain
(costs within a factor 0.3–3 either way across the rows), as WP 19 §12.

Tests (`tests/testMlmc.cpp`): MLQMC equals MLMC within eps and is more than
5× cheaper on the skewed case, its cost slope on the smooth surface in
(−1.5, −0.7), the Sobol dimension limit refused, the script engine's opt-in
on Sobol (MLQMC) and on pseudo-random draws (MLMC).

### 9.3 The local-vol grid's strike band, and the fixes that follow (2026-11-12)

**Why the grid was narrow.** `calibrate_vol_surface` clamped the requested
log-moneyness range (±0.6) to the intersection of what every slice of at
least `min_ttm` was quoted over (commit `803e7e4`, from a real AAPL chain:
a 6-day slice's SVI wing extrapolated to k = 0.6 gave a 600 % vol, since
svi_implied_vol = sqrt(w/T) amplifies a short slice's wing). One range for
every maturity let the narrowest short slice cap the long ones: on SPY a
three-week slice quoted to +8.4 % capped the whole two-year grid there,
while the one-year slices are quoted to +30 %.

**The fix: a band per maturity** (`market/dupire_from_svi`). Each slice is
trusted on its quotes' range (`quoted_band`); each grid maturity's local vol
is computed on the intersection of the two slices it interpolates
(`local_vol_column_bands`), so no SVI wing is read past its quotes; the
grid's strikes span the union of the kept slices' bands; a cell outside its
band, or where Dupire is unstable, takes its column's nearest valid cell
along K (`banded_sources`): flat in the strike within the maturity, never
borrowed from another one (the ring search of `nearest_valid_sources`
copied 50–110 % short-maturity vols into SPY's two-year call wing once that
wing entered the grid). The calendar checks of the slice selection read each
pair's common band. The superbucket rebuilds the same bands from the same
quotes, so its tape replays the same grid. The reach rule
(`reach_std_devs`) is gone with the single range it relaxed. The IV display
grid (`vol_surface.svi_implied_vol_grid`) leaves out what lies outside the
bands of the two slices it interpolates.

SPY (snapshot of 2026-11-11, spot 757.1): grid k in [−0.425, +0.275]
instead of [−0.367, +0.084]. One-year vanillas against the SVI market (GPU,
Sobol, 1e6 paths, daily steps), with the same grid made flat past +8.4 % to
stand for the old one:

| | SVI market | New grid | Old-style capped grid |
|---|---|---|---|
| Put 80 % | 9.919 | 9.890 (−0.03) | 9.872 (−0.05) |
| Put 90 % | 17.372 | 17.457 (+0.09) | 17.426 (+0.05) |
| Call 100 % | 54.021 | 54.181 (+0.16) | 54.092 (+0.07) |
| Call 110 % | 13.681 | 13.759 (+0.08) | 13.161 (**−0.52**) |
| Call 120 % | 7.075 | 6.956 (−0.12) | 3.581 (**−3.49, −49 %**) |

**MLMC's bias test reads three corrections** (`mlmc_detail::
remaining_bias`, Giles's later code): max over the last three correction
levels of |Y_l| / r^(L−l), over r − 1. The two-level test stopped at level 2
on SPY (corrections 1.06, 0.12, then 0.03–0.06) with about twice its bias
target. On the new grid, against a 8064-steps-a-year GPU reference of
54.071 ± 0.004:

| eps | MLMC (finest level, error) | MLQMC (finest level, error) |
|---|---|---|
| 0.2 | 6, −0.20 (1.4 se) | 6, +0.00 |
| 0.1 | 6, +0.09 | 7, +0.05 |
| 0.05 | 7, +0.02 | 8, −0.01 |

every estimate within its target, at the price of finer levels (MLQMC
11 s at eps = 0.1 on one thread: QMC gains little on the fine,
high-dimensional levels). beta on the new grid: log-Euler 0.98, Milstein
1.25 (0.41 before; still irregular, the advice against it stands).

**The scripts' default steps** (`ScriptRequest.steps_per_year`, not sent by
the front end): 52 → 252. The same call is 0.57 above its fine-step limit at
52 steps a year, 0.13 at 252, 0.017 at 2016, against a Sobol standard error
of 0.005: the displayed error does not include this bias. 252 removes most
of it at ~5x the cost of 52; a discretisation-bias estimate shown next to
the standard error (two step counts on common Sobol points) would make the
displayed error defensible, left to decide.

### 9.4 Multilevel by default, and the bias shown (2026-11-12)

Decided by the maintainer: the scripting API runs multilevel Monte Carlo by
default and returns the discretisation bias of every Monte-Carlo price.

- **Default** (`ScriptRequest.mlmc = "auto"`): MLQMC (Sobol, the default
  sampler) whenever the script suits it -- local vol, one payoff, no early
  exercise, no AAD greeks -- to a root-mean-square error of
  `mlmc_rel_eps` = 0.2 % of the price (`MlqmcSettings::rel_eps`, the target
  following the running estimate, floored at 1e-5 of the spot so that a
  worthless option still stops); `mlmc_eps` gives an absolute target
  instead; `mlmc = "off"` the usual engine. The binding keeps everything off
  by default: the other callers of `price_script` (portfolio marks, risk
  profiles, market vega) do not move without a decision.
- **Threads**: each Sobol replicate of a level has its own sampler (models,
  scenarios, payoff state) and runs as a task on the pool; its sum
  accumulates in point order, so the bits are the same on any number of
  threads (tested on 1 and 8). A product without a concurrent payoff state
  stays on one thread.
- **Bias** (`PricingResult::discretisation_bias`, `PricingResponse.
  discretisation_bias`, shown under the price on the scripting page): the
  multilevel run's own estimate of its remaining bias; for a plain run of a
  model that refines its grid (`mc_bias_estimate`, on by default in the
  API), the price with every step halved and the bias as the difference from
  the unhalved run on the same seed (weak order one, about three times the
  cost of one run); 0 for a model simulated exactly between the product's
  dates (Black-Scholes); absent otherwise (Heston, SLV, rough Bergomi: their
  grids do not refine yet; AAD greeks).

SPY through the API service (16 threads, snapshot of 2026-11-12):

| ATM call | Default (MLQMC, 0.2 %) | `mlmc = "off"`, CPU | `mlmc = "off"`, GPU |
|---|---|---|---|
| 1 year | 55.096 ± 0.071, bias ~0.074, 0.64 s | 55.076 ± 0.014, bias ~0.126, 1.47 s | 0.73 s |
| 3 months | 23.571 ± 0.032, bias ~0.016, 1.11 s | 23.666 ± 0.003, bias ~0.057, 0.61 s | 0.13 s |

The multilevel standard error is larger by design (half the mean-square
budget goes to the bias); the plain run's small ± now sits next to a bias
five to twenty times larger, which the page shows. Left: MLQMC on the GPU
(P6), which would make the default as fast as the plain GPU route; Heston and
SLV refinement, so that their bias is estimated too.

### 9.5 The stochastic-volatility models refine; multilevel stays for local vol (2026-11-13)

- `set_refinement` on Bates (Heston is Bates with lambda = 0; with jumps it
  refuses any r > 1: their draws would couple poorly, §8.3), SLV and SABR.
  Rough Bergomi does not refine: its hybrid scheme has a Volterra kernel on a
  fixed daily grid, and coupling two of its levels is a research question,
  not a refinement.
- The coupling now carries a step's non-Brownian draws (Bates's jump count
  and size, unused at lambda = 0): the coarse step takes its last fine
  step's, which keeps every level's law exact.
- `ISimulationModel::exact_between_dates()`, true for Black-Scholes (one and
  several assets) only: the bias reported as 0 is now a model's declaration,
  not inferred from its grid (rough Bergomi exposes the product's dates but
  steps on a grid of its own; the inference reported it exact).

Measured on SPY through the API (16 threads), one-year contracts:

| | Multilevel (MLQMC, 0.2 %) | Plain, bias estimated |
|---|---|---|
| Call, Heston | 62.343 ± 0.085, bias ~0.054, 12.5 s | 62.200 ± 0.063, bias ~0.174, 1.8 s |
| Call, SLV | 54.948 ± 0.071, bias ~0.052, 59.5 s | 55.358 ± 0.089, bias ~0.428, 2.0 s |
| Weekly up-and-out call, auto (SLV) | 23.270 ± 0.030, bias ~0.031, 163 s | 23.320 ± 0.072, bias ~0.174, 1.8 s |

The two agree (SLV call: 55.36 − 0.43 ≈ 54.93 against 54.95), and the
plain run's bias is large -- five times its standard error on the SLV call.
But MLQMC is far too slow on these models: its correction levels keep as
many points as level 0, so their variance hardly decays -- the variance's
full truncation and the leverage's staircase in T couple fine and coarse
paths poorly. Hence `ScriptRequest.mlmc`: "auto" (the default) runs
multilevel under local vol only, where it was measured fast; Heston and SLV
get the plain engine with their bias estimated and shown; "on" forces
multilevel on any model that refines.

Left when written: a better-coupled scheme for the variance and a leverage
interpolated in T, to make multilevel pay under Heston and SLV -- measured
since, and bounded by the calibration itself: §9.6. P6 for speed: §10.

### 9.6 Why multilevel cannot pay under SPY's Heston: the Feller ratio (2026-11-13)

§9.5 left "a better-coupled scheme for the variance" as the way to make
multilevel pay under Heston and SLV. Measured before writing one, on the
calibration production serves (SPY, snapshot of 2026-11-12: v0 0.0189,
kappa 9.16, theta 0.0365, xi 2.06, rho −0.62; one-year call, target 1 % of
the price, CPU):

| | beta (decay of the corrections' variance) | Levels | Time |
|---|---|---|---|
| Heston, MLMC | 0.21 | 0..2 | 11.5 s |
| Heston, MLQMC | 0.24 | 0..4 | 5.2 s |
| SLV, MLMC | 0.24 | 0..2 | 17.3 s |
| SLV, MLQMC | 0.90 (three levels, noisy) | 0..2 | 2.1 s |

Multilevel needs beta close to 1 to beat plain Monte Carlo and above 1 to
reach its eps^-2 cost. A beta of 0.2 is not the full-truncation Euler step's
doing. These parameters break the Feller condition by a wide margin:
delta = 4 kappa theta / xi^2 = 0.31 (Feller needs delta >= 2), so the
variance sits at zero much of the time, where its square-root diffusion is
not Lipschitz. For the CIR process in that regime:

- no scheme that reads the Brownian path at equidistant points converges
  strongly faster than order delta / 2 (Hefter & Jentzen, *On arbitrarily
  slow convergence rates for strong numerical approximations of
  Cox-Ingersoll-Ross processes and squared Bessel processes*, Finance and
  Stochastics 23, 2019);
- the truncated Milstein scheme reaches min(1/2, delta / 2) up to an epsilon
  (Hefter & Herzwurm, *Strong convergence rates for Cox-Ingersoll-Ross
  processes -- full parameter range*, J. Math. Anal. Appl. 459, 2018).

Order delta / 2 = 0.16 in L1 means a variance decaying as about
2^(−0.31 l): what we measure. An implicit or Milstein variance step would
change the constant, not the rate; QE (Andersen 2008) is not driven by
Brownian increments at all and has no natural coupling between levels. So:

- **Time-step multilevel stays off under Heston and SLV** (`mlmc = "auto"`
  is local vol only, §9.5), and this is a property of the calibrated
  parameters, not a gap in the engine. It would pay on a calibration that
  respects Feller (delta >= 2), which SPY's smile does not give (#146).
- What remains improvable is the **bias of the plain run** (0.17 to 0.43 on
  a price of 55 to 62, estimated and shown since §9.5): a QE variance step
  has a far smaller weak error at the same number of steps. That is a
  change of scheme on the CPU, the GPU and their derivatives, and moves
  every Heston and SLV price: a decision for the maintainer, outside
  multilevel.
- The leverage interpolated in T (§9.5) would smooth SLV's levels, which
  the Heston factor underneath bounds all the same: not pursued.

## 10. P6 — Multilevel on the GPU

Levels map onto the segments of `run_logical_block_segments` (WP 19, lot G3):
each level is reduced by its own tree, all levels fill the device at once,
and the pilot and correction rounds of the adaptive algorithm are launches.
Bits identical on one or two cards, CPU = GPU within error.

Acceptance: §11's P6 column; `GpuTwoCardsTest`-style test for MLMC.

### 10.1 P6 done: multilevel QMC on the GPU, with no new kernel (2026-11-13)

**Why first.** Production runs the API on two CPUs (`docker-compose.prod.yml`),
not the 16 threads of §9.4's measurements: the default local-vol route took
2.0 s (SPY one-year at-the-money call) to 12 s (the 120 % call) there.

**Departure from the plan above.** No kernel prices a fine and a coarse path
together. A level is **two ordinary Sobol runs** of the script's compiled
kernel (P1b), one on the fine grid and one on the coarse grid, on the same
points; replicate b's sample of the correction is the difference of its two
means. What couples them is the tables alone
(`mc::coupled_sobol_tables`, `engines/mc/sobol_bridge_host.hpp`):

- the fine grid's Brownian bridge is built in a **nested order**: its first
  n_c variates construct W on the coarse dates with the coarse bridge's own
  indices and weights, the n_c others fill each coarse step's midpoint;
- the coarse run reads the first n_c × factors coordinates of the same
  randomised sequences (Joe-Kuo directions and digital shifts are prefixes).

Point p of replicate b therefore gives both grids the same Brownian path, as
`LevelSampler` does by summing increments. The estimator is P5's
(`simulate_mlqmc_levels`, the adaptive loop now written on an `IQmcLevel`:
the CPU's `QmcLevel`, the GPU's `GpuQmcLevel` in
`src/engines/mc/script_engine.cpp`), with the CPU's keys per level and
replicate. Every existing property of a GPU script run carries over: the
compiled kernel, the logical-block reduction, the same bits on one or two
cards.

Limits, each said in the diagnostics:

- levels start at 1024 points a replicate (16 on the CPU): a launch has a
  fixed cost. (A level whose points grew was first run again from its first
  point; §10.3 removed that.)
- draws that are not Brownian increments (a jump's) do not nest: refused,
  the CPU runs;
- what the device cannot step (Milstein, a model without `describe_device`,
  `survivalAbove()`) stays on the CPU on `device = "auto"` and is refused on
  `"gpu"`;
- **the API's default** (maintainer, 2026-11-13): a script request that
  names no device (`ScriptPricingInputs.device`, now optional) runs a
  multilevel price on `"auto"` -- the GPU when the server has one -- and
  every other run on the CPU as before; a named device is obeyed. The
  scripting page starts with no device picked and says so.

Tests: `tests/testMlmc.cpp` (host, in the CI): every coarse increment is the
sum of its two fine ones to 1e-12 on an uneven two-factor grid, the fine
draws stay independent N(0, 1), the coarse bridge is the coarse grid's own,
grids that do not nest are refused. `tests/gpu/testGpuMlqmc.cpp`: under a
flat surface the estimated bias is below 1e-10 (two runs on unrelated points
would leave noise); GPU = CPU within the target on a call and a 12-fixing
Asian; the same bits on one or two cards; Milstein refused on `"gpu"`, on the
CPU with the reason on `"auto"`.

Measured on SPY through the API (snapshot of 2026-11-12, rate 4 %, one-year
contracts of the script library, target 0.2 % of the price, warm kernel
cache, one V100; the request's time, calibration read included):

| | GPU multilevel | CPU multilevel, dev (16 threads) | CPU multilevel, production (2 CPUs) | Plain GPU, no bias estimate |
|---|---|---|---|---|
| Call 100 % | 56.087 ± 0.078, bias ~0.060, **0.38 s** | 56.160 ± 0.070, bias ~0.070, 1.95 s | 2.0 s | 56.217 ± 0.035, 0.15 s |
| Call 120 % | 12.633 ± 0.018, bias ~0.012, **0.32 s** | 12.646 ± 0.017, bias ~0.011, 2.26 s | 12.1 s | 12.664 ± 0.034, 0.17 s |
| Arithmetic Asian | 33.137 ± 0.043, bias ~0.026, **0.51 s** | 33.202 ± 0.040, bias ~0.047, 0.75 s | | 33.242 ± 0.014, 0.19 s |

The GPU launches themselves take about 0.15 s of each request (the 120 %
call: 12 launches of 3 to 37 ms); the first request of a script pays the
kernel's compilation once (WP 27 L1b removes it).

Left: multilevel under Heston and SLV stays a matter of coupling (§9.5), not
of device.

### 10.2 The risks of a multilevel price (2026-11-13)

Before: a request for AAD greeks dropped multilevel ("not available under
AAD"), and both the scripting page and the workbench ask for greeks by
default -- from the site, the multilevel route was only reached with the
greeks off.

The multilevel price is a sum of expectations, so each risk is the sum of
its levels' risks: the pathwise derivative of the fine payoff minus that of
the coarse one on the same Brownian path (Burgos & Giles 2012). With §10.1's
design this is the same run with another kernel: each launch is the GPU's
per-path adjoint (WP 19 lot G3, local vol) instead of the price kernel, fine
then coarse, and a replicate's sample of a level's risks is the difference of
its two rows (`simulate_script_aad_mlqmc_gpu`; `ScriptAdjointGpuResult` now
returns each replicate's price and risks). A risk's error is the spread of
the replicates, summed over the levels. The points per level follow the
price's target: the risks get an error, not a target of their own.

- GPU, local vol, Sobol. On the CPU, under another model, or for a script the
  device cannot take, the risks come from the plain engine as before and the
  diagnostics say why. The other models wait for WP 27's generated adjoint
  (lot L4); a CPU version needs the tape inside `LevelSampler`.
- The API's default device (§10.1) covers it: no device named, local vol,
  greeks by AAD -> `"auto"`.
- A discontinuous payoff's pathwise derivative is as wrong per level as on
  one grid: `fuzzy` applies as before (WP 27 §8 is where it gets exact).

Tests (`tests/gpu/testGpuMlqmc.cpp`): on a flat surface the delta is
Black-Scholes's and the sigma_loc risks add up to its vega; on a skewed one
the price is the price route's to 1e-9 and the delta the central difference
of that price within error; a model without time-step bias is refused with
the reason. API: `test_script_mlmc.py`.

Measured on SPY as in §10.1 (5003 risks: spot, rate, dividend, the 100 x 50
sigma_loc grid), against the plain adjoint at 65 536 points:

| | Multilevel, GPU | Plain adjoint, GPU | Plain adjoint, CPU tape (16 threads) |
|---|---|---|---|
| Call 100 % | 56.087 ± 0.078, bias ~0.060, delta −0.2512 ± 0.0096, **0.69 s** | 56.217 ± 0.035, delta −0.2389 ± 0.0023, 0.30 s | 6.0 s |
| Call 120 % | 12.633 ± 0.018, bias ~0.012, delta −0.0085 ± 0.0017, **0.96 s** | 12.664 ± 0.034, delta −0.0096 ± 0.0012, 0.28 s | 5.8 s |
| Arithmetic Asian | 33.137 ± 0.043, bias ~0.026, delta −0.2541 ± 0.0043, **0.71 s** | 33.242 ± 0.014, delta −0.2568 ± 0.0015, 0.27 s | 6.3 s |

(The library's contracts fix their strike on the spot a week ahead: their
delta is the surface's, small and negative.) The risks agree within their
errors. The multilevel errors are larger on these contracts because the
default target, 0.2 % of the price, asks for fewer points than the plain
run's 65 536: what multilevel adds is the bias, estimated and bounded, not a
smaller statistical error at this target.

### 10.3 A run over a range of points; the directions built once (2026-11-13)

- `SobolTables::first_point` (and `SobolBridgeView`): a run prices points
  first_point .. first_point + n_units − 1 of every replicate. A level that
  doubles its points prices the new half alone and folds it into its means
  and risks. Every GPU script kernel reads the field (0 for every other
  run: the same bits as before). Test: two halves of a Sobol run of 10 000
  points give the whole run's replicate means to 1e-12, on other points.
- `sobol_tables` built a `SobolSequence` per replicate, each recomputing the
  direction integers of every dimension -- 32 times the work for a
  multilevel level. They are built once; the shifts come from
  `SobolSequence::shifts_for` (the constructor's own draws: the same bits).

The requests of §10.1 and §10.2 after both (same prices and errors):

| | Price | Price and 5003 risks |
|---|---|---|
| Call 100 % | 0.35 s | 0.61 s |
| Call 120 % (one level doubles three times) | 0.26 s | 0.81 s |
| Arithmetic Asian | 0.33 s | 0.64 s |

### 10.4 The registry's prices carry their bias (2026-11-13)

The scripting API returns the discretisation bias of every Monte-Carlo price
since §9.4; the registry's script routes (`price_by_script`: the workbench's
local-vol vanilla, barrier, Asian and lookback, and the catalogue under
Black-Scholes) did not. They now ask for it the same way
(`mc_bias_estimate`): a model that steps on a grid is priced with every step
halved and its bias read from the unhalved run on the same points; a model
exact between the dates returns 0 at no cost; a model that cannot refine its
grid returns none. With greeks, the central differences stay on the route's
own grid, beside the priced run. The workbench's result panel shows the
bias under the price, as the scripting page does.

What moves: a local-vol registry price is now the halved-step one (closer to
the limit by about its former bias), at three runs instead of one (§11's
local-vol rows of the first column: 0.008 → 0.023 s for the barrier). Tests
(`tests/testLocalVol.cpp`): the bias is there, positive and small against
the price, the same with and without greeks; the hand-written oracle steps
on the halved grid; a Black-Scholes route returns exactly 0.

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

**Today** (2026-11-13, after P1 to P6;
`benchmarks/results/2026-11-13-699a1e2.json`, run on that commit plus §10.4
and the two fixes below). The first column is no longer a hand-written
engine: since P3 the registry's route is the script engine with Sobol, on the
GPU when present, and it estimates its bias (§10.4).

| Product (model) | Registry route | Script, CPU PCG32 | Script, CPU Sobol | Script, 1 V100 Philox | Script, 1 V100 Sobol |
|---|---|---|---|---|---|
| Vanilla call (BS) | 0.002 s | 0.187 s | 0.001 s | 0.002 s | 0.002 s |
| Arithmetic Asian, 252 fixings (BS) | 0.101 s | 27.5 s | 0.077 s | 0.055 s | 0.043 s |
| Arithmetic Asian, 252 fixings (local vol) | 0.079 s | 42.4 s | 0.098 s | 0.060 s | 0.041 s |
| Up-and-out call, 52 weekly dates (BS) | 0.005 s | 7.77 s | 0.169 s | 0.006 s | 0.006 s |
| Up-and-out call, 52 weekly dates (local vol) | 0.023 s | 12.5 s | 0.286 s | 0.011 s | 0.006 s |
| Fixed-strike lookback, 52 weekly dates (BS) | 0.016 s | 1.91 s | 0.009 s | 0.002 s | 0.002 s |
| Fixed-strike lookback, 52 weekly dates (local vol) | 0.023 s | 3.04 s | 0.015 s | 0.003 s | 0.002 s |
| Basket call, 3 assets (BS) | 0.018 s | 0.418 s | 0.002 s | 0.002 s | 0.002 s |
| Worst-of call, 3 assets (BS) | 0.001 s | 1.18 s | 0.002 s | 0.003 s | 0.002 s |
| Best-of call, 3 assets (BS) | 0.001 s | | | | |
| Autocall, 4 quarterly dates (BS) | 0.001 s | | | | |
| Mountain range, 3 assets, 3 dates (BS) | 0.002 s | | | | |
| Dispersion swap, 3 assets, 52 dates (BS) | 0.006 s | | | | |
| Variance swap, 52 dates (BS) | 0.002 s | | | | |
| Volatility swap, 52 dates (BS) | 0.002 s | | | | |
| Quanto call (BS) | 0.044 s | | | | |

Against v1: the routes the site uses went from seconds to milliseconds
(Asian under local vol 60.3 s → 0.079 s, up-and-out under local vol
9.31 s → 0.023 s, mountain range 0.939 s → 0.002 s); the CPU Sobol script
route is 5 to 13 times faster on the path-dependent products (batched
interpreter, AS 241).

**A regression this run found, and its fix.** The first run of the day had
the CPU PCG32 column 9 to 22 % *slower* than v1 at the same paths and the
same prices (Asian BS 30.7 → 36.2 s, lookback BS 1.96 → 2.37 s). Timed commit
by commit on a 52-date lookback (800 000 paths, one thread): 1.83 s at v1,
2.20 s after P1a, 2.36 s after P2a, 2.32 s today. Two causes, both fixed:

- the PCG32 loop was the one route left on the path-by-path interpreter
  (P1a batched Philox and Sobol only), and its code got slower when the
  engine's function grew around it. It now feeds the batched interpreter
  like a Sobol sequence does (`batch_path` / `flush_batch`: the payoffs
  added in the same order);
- since P2a the Black-Scholes model resized every event's survival vector on
  every path, barrier or not. It now touches it only for an event that has
  one (`allocate_scenario` sized it already).

After both: 1.80 s on the lookback, below v1, and the nine PCG32 prices of
the matrix identical to v1's to 17 digits. The table above is the run after
the fix. Lesson for the method (`hpc-finance`): the matrix compares
*columns*, so a route nobody was optimising was not watched; rerun it at the
end of each lot, not only its own bench.

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
  **Decided otherwise on 2026-11-16 (maintainer)**: CPU and GPU, and the
  compiled and interpreted scripts, no longer need the same bits -- prices
  within 1e-12 relative, risks within 1e-10, on the same draws; one
  configuration stays deterministic run to run (the API's replays). The
  optimisations set aside for the bits come back one by one, measured (§14).
- **Wheel size.** NVRTC and nvJitLink are linked statically (P1b): the GPU
  wheel grows by their size. Loading them with `dlopen` from the toolkit's
  shared libraries would avoid it, at the price of shipping those in the
  image.
- **Greeks.** Moving products to scripts moves their greeks from bump to AAD:
  the displayed values change within their error. Said in every P3 pull
  request.
- **Exercise.** Early exercise runs on the tree evaluator only today
  (`scripted_product.hpp`): Bermudan scripts stay on it until the bytecode
  handles `exercise()`. **Threads since 2026-11-16**: the tree evaluator has
  a per-thread copy (`ScriptedProduct::TreeState`), so a script with
  exercise() prices and takes its tape's risks on every thread, the same
  bits on any number of them. A monthly Bermudan put under local vol (252
  steps a year, 200 000 Sobol points, two threads): price 6.9 → 4.4 s, price
  and AAD risks 25.4 → 13.3 s (it was over the API's 20 s); the exercise
  rule's fit stays on one thread. The bytecode, the batched interpreter and
  the GPU still have no exercise(). **Since 2026-11-17 it compiles (§15)**: the scalar and the batched interpreter, the pilot included; the GPU is what remains.
- **Sobol dimension.** The finest MLQMC level is capped by 21 201 dimensions
  (§9).
- **GPU memory.** The LLM of the scripting assistant shares both cards
  (13 to 14 GB of 16 when loaded): the benchmarks note what was free, and the
  GPU tests that fail with `out of memory` while it is loaded are an
  environment limit, not a regression.

## 13. Eight units per thread (2026-11-16)

Measured on the maintainer's request, then adopted by them: the logical
block's units per thread (`LogicalBlocks::kUnitsPerThread`) from 16 to 8,
so a block is 2 048 units. At 200 000 paths a 4 096-unit block left the
generated adjoint with 49 blocks for 80 SMs (0.31 wave, Nsight Compute).
One V100, ms, 16 → 8 (→ 4):

| | 200k paths | 1M paths |
|---|---|---|
| local vol adjoint (4 scripts, 363 risks) | 61-100 → 67-109 (→ 101-164) | 425-748 → 241-394 (→ 282-451) |
| Heston tangent | 20.7-25.7 → 20.9-25.9 | 97-122 → 51-64 |
| SLV adjoint | 71-88 → 51-64 | 222-366 → 159-211 |

The price kernels (`perf_matrix.py`, GPU columns) 5-15 % faster; the CPU
columns unchanged (same prices); the exposure engine at 2M paths 0.72 →
0.74 s on one card, 0.58 → 0.40 s on two. Local vol's adjoint at 200k is the
one case slower (about 8 %); every GPU result keeps being the same bits on
one or two cards, and its last bits change once (the tree's shape).
`mc::stratum_of`, which hard-coded the 4 096-unit block, now reads the
constants.

## 14. The optimisations set aside for the bits, measured again (2026-11-16)

With CPU and GPU no longer held to the same bits (§12), each optimisation set
aside for them is measured again and kept only if it pays.

- **FMA contraction in the GPU script kernels** (`--fmad=true`, and plain
  operators instead of `__dadd_rn` / `__dmul_rn` in the generated code):
  **no gain, not kept.** `qm_script_jit_bench` (one V100): compiled scripts
  6.31 / 12.03 / 15.98 / 28.55 / 3.53 ms before, 6.33 / 12.05 / 15.95 /
  28.51 / 3.67 after; `qm_gpu_risk_bench 1000000 gpu` (three runs):
  local-vol adjoint +1-3 %, SLV +8 % (134 → 146 ms), Heston unchanged. The
  kernels are instruction-cache bound (§2.2): fusing operations does not
  shorten them, and the 5-7 % P1b measured has since been taken where it
  pays by `mul_add` in Φ⁻¹ (P1h). The compiled-against-interpreted tests now
  check the tolerance anyway (`GpuScriptJitTest.*WithinTolerance`).
- **FMA and AVX2 on the CPU** (the wheel built for x86-64-v3, the server's
  ISA, instead of the baseline x86-64): **not kept.** `perf_matrix.py
  --no-gpu`, same machine, back to back: the Sobol script routes unchanged
  (0.97-1.03), the PCG32 ones 5-28 % slower (basket 0.41 → 0.52 s, worst-of
  1.27 → 1.56 s), one hand-written engine faster (barrier under local vol
  0.079 → 0.046 s); rough Bergomi's price 6.60 → 5.68 s on two threads, its
  AAD unchanged. Prices moved by at most 6.5e-16 relative. Mixed, slower
  where the routes are the site's: the baseline ISA stays.
- **The gradient row buffered per grid cell in the GPU adjoint** (tried and
  reverted on 2026-11-16 under the bits rule, #239): **kept, for local vol.**
  The C¹ grid's step reaches eight nodes, whose read-modify-writes in the
  thread's global gradient row dominated; consecutive steps mostly share a
  cell, so the nodes' adjoints are summed per thread and written when the
  cell changes. The pending nodes exist in the local-vol kernels only (a
  generated constant, `qm_vol_grid`, folded at link time): with them in
  every kernel SLV was 13 % slower. One V100, `qm_gpu_risk_bench`, ms
  (four scripts, 363 risks): 200k paths 132 / 183 / 232 / 181 → 108 / 135 /
  179 / 141; 1M paths 497 / 640 / 800 / 655 → 337 / 403 / 528 / 453; SLV
  and Heston unchanged; the tape matched to 1e-15 as before (the sum is
  reassociated, within the tolerance; deterministic run to run).
- **CUDA's own Φ⁻¹ on the device** (`normcdfinv`, libdevice, a few ulps),
  where AS 241 ran on both sides for the bits: **kept.** `qm_script_jit_bench`
  (one V100, compiled scripts, ms): barrier BS 6.28 → 4.89, barrier LV
  12.02 → 10.55, Asian BS 15.51 → 11.30, Asian LV 28.48 → 24.84, autocall
  3.61 → 3.08 (12-27 %); the GPU suite passes unchanged (its CPU/GPU checks
  already had tolerances); the host keeps AS 241.
- **Reassociating rough Bergomi's far-field sums** on the CPU: not done --
  they already run as eight independent chains per path (§ WP 26 rough
  Bergomi), and only an FFT would change their order of cost, for at most
  about 12 % of that model's path.

## 15. Early exercise in the bytecode (#282, 2026-11-17)

Scripts with `exercise()` / `call()` ran on the tree evaluator only: the
pilot, the pricing pass and the tape's risks (§12). They now compile, and
run on the scalar interpreter (prices and the tape) and on the batched one
(prices).

**One instruction.** `Exercise a b`: pops `a` regressors (none in the script:
the event's spots, which the compiler pushes), decides for event `b`, and
pushes 1 / 0 on the value stack -- a crisp degree on the degree stack in
fuzzy mode, as `FuzzyEvaluator` does. The machine carries what
`Evaluator::exercise` carries: the right exercised or not, the last event
that decided (a second decision in one event is the tree's error, raised
after the path), and the mode -- the fitted rule, or the pilot's two
branches (never; at one event) with a log of the decisions reached.

**The rule as flat data.** `ExerciseView` holds one `ExerciseRule` per event
(means, scales, the box of issue #228, the monomials' exponents and
coefficients) and `exercise_decide` repeats `ExercisePolicy::decide`
operation for operation; `ScriptedProduct::set_exercise_policy` flattens the
rule. No pointer to host structures, so the same function can run on the
device (step 3).

**The batched interpreter.** Each active lane decides on its own regressors
and keeps its own state (a bit of `exercised`, its last event); a lane that
does not reach the instruction (masked by an `if`) is left alone, as its
path is on the scalar interpreter.

**The pilot too.** With the pricing pass compiled, the pilot became most of a
request: it runs the script once without exercising and once per decision
reached, restarted at that event. `exercise_pilot` now does those runs on
the machine (the variables and the payoff saved at the start of each event);
the regression itself is unchanged.

**Routing.** Before a rule is set the script still goes to the tree
evaluator, which refuses it and says what to call. More than 8 regressors
(`kMaxExerciseRegressors`) stay on the tree evaluator. No IR is lowered for
these scripts, and `script_gpu_unsupported` says so: the GPU is step 3.

**Same bits.** Tree evaluator against bytecode, the pilot included, on five
scripts (a Bermudan put with two regressors, `exercise()` without argument,
a chooser, an issuer's `call()` nested in an `if` before a smoothed
condition, a negated decision in a conjunction), hard and fuzzy: the price
and its error (`Lsm.TheBytecodeDecidesAsTheTreeEvaluatorBitForBit`), the
tape's risks (`TheBytecodesTapeRisksAreTheTreeEvaluators`), path by path
against batched against a pool with a partly filled batch
(`TheBatchedInterpreterDecidesLaneByLane`), and the two errors kept.

**Measured** (one thread, the server, a monthly Bermudan put under
Black-Scholes, two regressors; prices and risks identical to the last bit):

| | tree evaluator | bytecode |
|---|---|---|
| pilot, 50 000 paths: the script's runs | 0.758 s | 0.187 s |
| fit (pilot + regression) | 1.06 s | 0.46-0.51 s |
| price, 200 000 paths | 0.753 s | 0.290 s path by path, 0.247-0.256 s batched |
| request (fit + price) | 1.82 s | 0.72 s |
| tape risks, 100 000 paths | 0.415 s | 0.233 s |

What is left of the fit is the regression (about 0.27 s here: a pivoted QR
per date on up to 50 000 rows) and it is still one thread, as the pilot is.

**Left.** Step 3 of #282: an `Exercise` operation in the IR reading the rule
as data, the price kernel on the GPU, then the risks (the decision held
fixed, as on the tape). The pilot on a pool.

## 16. Early exercise on the GPU, and the exercise regression's rank (#282 step 3, 2026-11-17)

**In the IR.** A script with `exercise()` / `call()` is lowered once its
rule is fitted (`ScriptedProduct::set_exercise_policy`, then
`ir::lower_script(..., ExerciseLowering)`). The decision is written with
the IR's own operations, `exercise_decide`'s in its order: the box as
`(r < lo) or (r > hi)` (no box is (-inf, inf), the same decision for every
regressor), `z = (r - mean) / scale`, the gain `0 + beta_t * phi_t` with
`phi_t` the product of the z's, then `not exercised and not outside and
gain > 0` (`< 0` for the issuer). The rule's numbers are each event's
terms, after the script's own, so the code depends only on the rule's
shape -- the regressors, the monomials (one degree for every date), holder
or issuer -- and the dates of a schedule keep sharing one block: one kernel
per product shape, cached as the others. Whether the right was exercised is
one more variable (index `n_vars`), which the existing passes carry like any
other. In a fuzzy `if` the interpreters do not blend it (a branch that ran
and exercised leaves it set); the IR writes that as the two flags' blend
read as a condition, `FuzzyPhi(then, else) != 0`, exact because the flag only
goes from 0 to 1 and one event decides once.

A comparison has no derivative, so the tangent and the adjoint hold the
decision fixed, as the tape does, with nothing new in the transformations.

What the IR does not follow stays on the CPU, the reason kept
(`ScriptedProduct::ir_refusal`) and shown in the diagnostics: two
`exercise()` in one event (the interpreters check one decision per date as
the path runs; the IR refuses it statically, also when the two sit in
exclusive branches), more than 8 regressors, a rule whose dates have
different monomials. The interpreted GPU kernels have no rule: with
`QM_SCRIPT_JIT=0` the GPU refuses these scripts and `auto` prices them on
the CPU.

**The rule decided by rounding: fixed.** Comparing the GPU with the CPU
found the first script that disagreed beyond the tolerance -- a Bermudan put
on `(max(K - S, 0), S)` -- and the reason was not the GPU. On in-the-money
paths the two regressors are collinear, and the fitted coefficients were
`1e13`-`1e14` and cancelling: a move of the spot by 4e-16 relative flipped
67 236 decisions on a 1e-4 grid of S. Issue #228's fix had set the QR's
threshold to 1e-10, but Eigen's `ColPivHouseholderQR::solve()` keeps every
pivot above machine epsilon (`nonzeroPivots()`); the prescribed threshold
only reaches `rank()`. The solve is now truncated at `rank()`: R's leading
block, the other coefficients 0. Where the basis has full rank at 1e-10 the
operations are the same, hence the same bits; only bases dependent to within
1e-10 move, and there the dropped coefficients were noise. After the fix the
rule above has no coefficient above 15, no decision flips, and the put on
`(payoff, spot)` prices as the put on `spot` alone (the payoff is a function
of the spot) -- which it should.

Displayed prices of early-exercise scripts move (200 000 paths against a
CRR tree of 60 steps per exercise period):

| Bermudan put | before | after | tree |
|---|---|---|---|
| 36/40, r 6 %, vol 20 %, 26 fortnights, (payoff, spot) | 4.46834 ± 0.00134 | 4.46875 ± 0.00175 | 4.46888 |
| 44/40, vol 40 %, 12 months, (payoff, spot) | 3.90000 ± 0.00187 | 3.90036 ± 0.00235 | 3.90271 |
| 40/40, vol 20 %, 12 months, (payoff, spot) | 2.28544 ± 0.00088 | 2.28543 ± 0.00099 | 2.28614 |
| 36/40, 26 fortnights, (spot) | 4.46875 ± 0.00175 | unchanged | 4.46888 |

The same regression fits the xVA's future values and the dynamic initial
margin (`PolynomialRegression`): it moves there only where a basis was
dependent to within 1e-10.

**Checked.** The IR's path against the bytecode's under the same rule (the
rule in `mc::ScriptPathView`), bit for bit, on six scripts -- two
regressors, none, a chooser, an issuer's call nested in an if, a negated
decision, an exercise inside an if on the spot whose fuzzy degree runs both
branches -- hard and fuzzy, numbers as constants and as terms, under
Black-Scholes, local vol and Heston, and the risks' variant against the dual
path (`ScriptIrExercise`); two rules of one shape give the same blocks; the
refusals. On the GPU (`GpuRisksTest.EarlyExercise*`): prices equal to the
CPU's on the same Philox paths (1e-9), risks to the tape's (1e-8), under
Black-Scholes (tangent), local vol (adjoint) and Heston, hard and fuzzy;
with compilation off the GPU refuses and `auto` says why.

**Measured** through `price_script` (one V100 against the CPU on two
threads, as in production; a monthly Bermudan put, 200 000 Sobol points,
the same prices and deltas on both):

| | CPU (2 threads) | GPU |
|---|---|---|
| Black-Scholes, price | 0.58 s | 0.48 s |
| Black-Scholes, price + AAD risks | 0.80 s | 0.44 s |
| local vol 252 steps/yr, price | 5.87 s | 1.98 s |
| local vol 252 steps/yr, price + AAD risks (38) | 44.6 s | 1.96 s |

The local-vol risks were over the API's 20 s. What the GPU requests spend
now is mostly the pilot: 50 000 paths simulated and run on one CPU thread.

**Left.** The pilot (and the regression) on a pool or on the device: §17.

## 17. The LSM pilot on the request's threads (2026-11-17)

With the pricing on the GPU (§16), the exercise rule's fit became most of a
request, and it ran on one thread. Measured on one core (a monthly Bermudan
put, 50 000 pilot paths): under Black-Scholes the draws and steps take
0.03 s, the script's runs 0.17 s, the regression stage 0.2 s; under local
vol at 252 steps a year the draws and steps take 1.7 s of a 2.0 s fit.

**The pilot's paths on a pool.** `fit_exercise_policy(..., pool)`: chunks of
paths, each worker with its own model clone, scenario, Philox source and
pilot state (`ScriptedProduct::make_pilot_state`: the machine's buffers and
scratch, or its own copy of the tree evaluator); path p keeps its draws (the
seed's complement, at path p) and writes slot p, so the rule is the same
bits on any number of threads (`Lsm.ThePilotOnAPoolFitsTheSameRule`:
compiled and tree, hard and fuzzy, a pilot of 10 007 paths in unequal
chunks). The binding gives it the request's threads, for prices and risks.

**The regression stage without copies.** The QRs themselves cost 0.06 s
for 12 dates of 25 000 rows; the rest was copying every member's regressors
into vectors of vectors, twice a date. The rows are now read where the pilot
left them and the fitted ones packed once, row-major
(`fit_exercise_regression(const double *rows, m, n, ...)`, the same
standardisation and operations in the same order), and the monomials are
evaluated without allocating: the rule's bits do not move.

Fit of the monthly Bermudan put (best of three; the rule identical to the
bit on every line):

| threads | Black-Scholes | local vol 252 steps/yr |
|---|---|---|
| 1 (before) | 0.39 s | 1.73 s |
| 1 | 0.38 s | 1.73 s |
| 2 (production) | 0.30 s | 0.99 s |
| 4 | 0.25 s | 0.60 s |
| 16 | 0.18 s | 0.26 s |

What does not scale under Black-Scholes is the pilot's bookkeeping (a path
of 12 steps allocates its decisions' vectors) and the regression, now
0.12 s. Under local vol on two CPUs the rest is the steps themselves: the
next lever is to simulate the pilot's paths on the GPU and run only the
script's branches on the host (0.17 s).

## 18. The fixed cost of a GPU call (#330, 2026-11-19)

Every GPU pricing paid ~1.8 ms whatever its path count: on a one-date
digital at 1 024 paths, nvprof counted **21 µs of kernels** in a 1.8 ms
call. The rest was administration repeated on every request. Each step
below was measured on its own (`benchmarks/gpu_call_floor.py`: library
products priced through `pricing_service` on typed Black-Scholes inputs,
warm kernel cache, one V100 with nothing else loaded, best of 30), and
every price and delta of the bench is the same to the bit after each step.

1. **The device's name read once** (`device_name`): `cudaGetDeviceProperties`
   cost 127 µs a call, only to label the diagnostics.
2. **CUDA's stream-ordered pool** (`detail::device_alloc` / `device_free`,
   `cudaMallocAsync` on the legacy default stream): the ~30 `cudaMalloc` and
   `cudaFree` of a request (430 µs under nvprof) reuse what the previous
   request freed. The pool keeps at most `QM_GPU_POOL_RETAIN_MB` (256 MiB by
   default) of idle memory, since the assistant's LLM shares the cards; out of
   memory, it releases its idle memory and retries once.
3. **The CPU pool started on demand** (`ThreadPool::start_on_demand`): the
   binding started and joined 15 threads for every request, GPU ones
   included -- ~30 % of a warm call (py-spy). They now start when a CPU
   engine first asks for them (`num_threads()`, `spawn_task`), so an `auto`
   request that falls back to the CPU keeps its threads. `available_cpus()`
   reads the cgroup files once.
4. **One copy for the request's arrays** (`DeviceArrays::put` / `flush`):
   the ~26 arrays are packed, 256-byte aligned, into one pooled chunk and
   copied once; the three functions that call `put()` flush before their own
   launches and before returning.
5. **No free-memory query for a small request** (`detail::launch_budget`):
   below 64 MiB in all, half of the free memory could only cap a launch on a
   card already nearly full; the two `cudaMemGetInfo` cost 44 µs.

Best of 30 (of 5 at 1 M paths), milliseconds:

| case | paths | before | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|---|
| digital call | 1 024 | 1.84 | 1.87 | 1.31 | 0.62 | 0.49 | **0.44** |
| digital call | 1 M | 2.38 | 2.48 | 1.52 | 0.78 | 0.64 | **0.59** |
| digital call, AAD greeks | 1 024 | 3.19 | 3.00 | 2.10 | 1.38 | 1.11 | **1.01** |
| digital call, AAD greeks | 1 M | 5.53 | 5.20 | 4.24 | 3.01 | 2.61 | **2.56** |
| phoenix autocall | 1 024 | 2.66 | 2.55 | 2.09 | 1.26 | 1.13 | **1.09** |
| phoenix autocall | 1 M | 3.30 | 3.29 | 2.78 | 1.69 | 1.50 | **1.48** |
| phoenix autocall, AAD greeks | 1 M | 8.95 | 8.75 | 7.39 | 6.62 | 6.19 | **6.20** |

What is left of the digital's 0.44 ms: ~0.19 ms of Python around the engine
(the template's render, pydantic, `start_on`, tracing spans), ~0.15 ms of
CUDA calls (four copies, four launches, six pooled allocations), and the
host's preparation of the request, with no single hotspot left. The
kernels themselves run 21 µs.

## 19. The errors of the page's defaults (#331 step 1, 2026-11-19)

Lot B must not optimise time per path at a fixed path count: the metric is
the time to reach an error target, discretisation bias included. Before
choosing targets, this step measures where the page's defaults stand.

`benchmarks/page_errors.py` sends every product of the library the request
the Price page sends with its default inputs (`model=auto`, SPY, or AAPL,
MSFT, GOOGL, AMZN for several underlyings, SAP.DE for a quanto; 4 % rate;
50 000 paths, 20 000 with several underlyings; seed 1; AAD greeks; device
`auto` with Philox), through `pricing_service` on the 2026-11-18 snapshot.
AAD greeks return no bias, so a second, price-only run gives it (halved
steps, or the multilevel run's bias test). Raw results, with every scalar AAD
sensitivity and each grid's nodes summed: `benchmarks/results/2026-11-19-page-errors.json`.

Columns: SE and bias as a share of the price. "delta / rho / div SE" are the
relative standard errors of the AAD sensitivities to spot, rate and
dividend; with several underlyings, the worst asset's (a minor asset's small
delta is relatively noisy by nature). Hull-White products: rho is the
parallel curve shift. "–" for the bias: not estimated (early exercise,
several assets, Hull-White).

| product | model | device | paths | price | SE % | bias % | bias / SE | delta SE % | rho SE % | div SE % |
|---|---|---|---|---|---|---|---|---|---|---|
| accumulator | slv | gpu | 50,000 | 671.2 | 0.52 | 0.21 | 0.4 | 11.2 | 8.4 | 8.9 |
| altiplano | local_vol | cpu | 20,000 | 1055 | 0.03 | – | – | 28.5 | 2.7 | 27.3 |
| american-call | slv | gpu | 50,000 | 57.55 | 0.41 | – | – | 27.5 | 5.3 | 4.8 |
| american-put | slv | gpu | 50,000 | 34.52 | 0.36 | – | – | 1.3 | 0.7 | 0.7 |
| asian-call | slv | gpu | 50,000 | 35.65 | 0.22 | 0.56 | **2.5** | 4.7 | 1.1 | 0.9 |
| asian-in-call | slv | gpu | 50,000 | 51.89 | 0.40 | 0.02 | 0.1 | 3.1 | 0.5 | 0.4 |
| athena-autocall | slv | gpu | 50,000 | 995.3 | 0.04 | 0.02 | 0.4 | 176.6 | 106.4 | 105.9 |
| atlas | local_vol | cpu | 20,000 | 95.01 | 0.28 | – | – | 76.4 | 5.6 | 16.0 |
| barrier-reverse-convertible | slv | gpu | 50,000 | 1032 | 0.02 | 0.00 | 0.1 | 18.1 | 1.7 | 17.8 |
| basket-call | local_vol | cpu | 20,000 | 94.18 | 0.42 | – | – | 98.4 | 6.9 | 19.7 |
| bermudan-swaption | hull_white | cpu | 50,000 | 2.712e+05 | 0.06 | – | – | – | 0.1 | – |
| best-of-call | local_vol | cpu | 20,000 | 274 | 0.40 | – | – | 106.4 | 16.0 | 46.6 |
| bonus-certificate | slv | gpu | 50,000 | 1103 | 0.04 | 0.03 | 0.6 | 10.9 | 1.4 | 4.3 |
| call-spread | slv | gpu | 50,000 | 53.81 | 0.23 | 0.01 | 0.1 | 0.5 | 0.4 | 0.3 |
| capital-protected-note | slv | gpu | 50,000 | 994.5 | 0.03 | 0.02 | 0.5 | 5.0 | 3.3 | 5.2 |
| chooser | slv | gpu | 50,000 | 81.06 | 0.32 | – | – | 2.1 | 0.9 | 0.7 |
| cliquet | slv | gpu | 50,000 | 63.21 | 0.09 | 0.43 | **4.6** | 1.1 | 0.2 | 0.2 |
| compound | slv | gpu | 50,000 | 28.63 | 0.68 | – | – | 1.6 | 0.6 | 0.5 |
| corridor-variance-swap | slv | gpu | 50,000 | -148.8 | 0.33 | 0.48 | **1.4** | 50.8 | 15.8 | 51.3 |
| digital-call | slv | gpu | 50,000 | 69.46 | 0.14 | 0.05 | 0.4 | 3.9 | 4.1 | 2.8 |
| digital-put | slv | gpu | 50,000 | 26.62 | 0.37 | 0.13 | 0.4 | 3.9 | 2.5 | 2.8 |
| discount-certificate | slv | gpu | 50,000 | 945.3 | 0.03 | 0.03 | **1.0** | 0.9 | 0.3 | 0.5 |
| dispersion-swap | local_vol | cpu | 20,000 | 384.3 | 0.10 | – | – | 4.5 | 1.4 | 5.9 |
| double-barrier-call | slv | gpu | 50,000 | 49.91 | 0.24 | 0.07 | 0.3 | 2.4 | 2.8 | 2.5 |
| double-no-touch | slv | gpu | 50,000 | 73.92 | 0.14 | 0.19 | **1.4** | 31.7 | 8.4 | 16.3 |
| down-and-in-put | slv | gpu | 50,000 | 25.11 | 0.75 | 0.47 | 0.6 | 4.4 | 3.1 | 4.0 |
| equity-forward | slv | gpu | 50,000 | 23.08 | 1.21 | 0.02 | 0.0 | 2.7 | 0.3 | 0.3 |
| european-call | slv | gpu | 50,000 | 57.59 | 0.38 | 0.30 | 0.8 | 3.8 | 0.6 | 0.5 |
| european-put | slv | gpu | 50,000 | 34.51 | 0.51 | 0.51 | **1.0** | 1.4 | 0.5 | 0.6 |
| european-swaption | hull_white | cpu | 50,000 | 1.881e+05 | 0.02 | – | – | – | 0.0 | – |
| everest | local_vol | cpu | 20,000 | 977.8 | 0.02 | – | – | 89.0 | 1.3 | 16.8 |
| fixed-lookback-call | slv | gpu | 50,000 | 74.51 | 0.25 | 0.03 | 0.1 | 10.5 | 0.5 | 0.5 |
| fixed-rate-bond | hull_white | cpu | 50,000 | 95.87 | 0.00 | – | – | – | 0.0 | – |
| floating-lookback-call | slv | gpu | 50,000 | 107.7 | 0.15 | 0.14 | 0.9 | 33.1 | 1.0 | 0.8 |
| forward-start-call | slv | gpu | 50,000 | 48.62 | 0.19 | 0.22 | **1.2** | 2.9 | 0.7 | 0.7 |
| globally-floored-cliquet | slv | gpu | 50,000 | 63.58 | 0.15 | 0.91 | **6.2** | 10.6 | 1.0 | 0.9 |
| himalaya | local_vol | cpu | 20,000 | 120.8 | 0.45 | – | – | 56.5 | 9.6 | 17.5 |
| interest-rate-swap | hull_white | cpu | 50,000 | 1850 | 3.21 | – | – | – | 0.0 | – |
| ladder-call | slv | gpu | 50,000 | 63.25 | 0.23 | 0.05 | 0.2 | 5.1 | 2.5 | 2.3 |
| napoleon | slv | gpu | 50,000 | 976.8 | 0.01 | 0.01 | **1.5** | 18.6 | 0.4 | 10.2 |
| one-touch | slv | gpu | 50,000 | 18.52 | 0.53 | 0.78 | **1.5** | 8.5 | 6.9 | 9.6 |
| outperformance-option | local_vol | cpu | 20,000 | 145.2 | 0.18 | – | – | 1.4 | 9.2 | 0.5 |
| phoenix-autocall | slv | gpu | 50,000 | 1020 | 0.04 | 0.04 | **1.0** | 50.6 | 7.8 | 132.5 |
| quanto-call | – | – | – | **fails**: MarketDataUnavailable | | | | | | |
| rainbow-call | local_vol | cpu | 20,000 | 138.1 | 0.44 | – | – | 252.5 | 8.7 | 24.6 |
| range-accrual | slv | gpu | 50,000 | 1014 | 0.01 | 0.01 | **2.2** | 10.0 | 0.2 | 1.5 |
| reverse-cliquet | slv | gpu | 50,000 | 1220 | 0.02 | 0.01 | 0.3 | 15.9 | 0.5 | 1.7 |
| reverse-convertible | slv | gpu | 50,000 | 1010 | 0.02 | 0.02 | **1.0** | 1.4 | 0.2 | 0.6 |
| shark-fin | slv | gpu | 50,000 | 1029 | 0.02 | 0.00 | 0.0 | 3.8 | 3.5 | 2.8 |
| twin-win | slv | gpu | 50,000 | 1018 | 0.04 | 0.04 | 0.9 | 7.8 | 3.4 | 1.4 |
| up-and-out-call | slv | gpu | 50,000 | 51.56 | 0.25 | 0.11 | 0.4 | 2.6 | 3.0 | 2.6 |
| variance-swap | slv | gpu | 50,000 | 23.65 | 5.10 | 6.72 | **1.3** | 154.6 | 50.9 | 64.3 |
| volatility-swap | slv | gpu | 50,000 | -3342 | 0.55 | 1.54 | **2.8** | 152.1 | 255.6 | 32.6 |
| worst-of-autocall | local_vol | cpu | 20,000 | 902 | 0.14 | – | – | 3.7 | 0.8 | 3.0 |
| worst-of-call | local_vol | cpu | 20,000 | 22.53 | 0.62 | – | – | 8.3 | 2.4 | 7.4 |
| worst-of-reverse-convertible | local_vol | cpu | 20,000 | 930.5 | 0.05 | – | – | 2.7 | 0.3 | 1.9 |
| zero-coupon-bond | hull_white | cpu | 50,000 | 78.32 | 0.00 | – | – | – | 0.0 | – |

### What it says

1. **`auto` means SLV on the GPU for every single-asset product** (its
   state carried across dates or not), local vol on the CPU for several
   assets, Hull-White for rates. Throughput work on single-asset products is
   therefore SLV work (#317's QE scheme).
2. **The price is precise; its bias often is not.** The standard error is
   below 0.5 % of the price on 49 of 56 products. But on **13 products the
   bias is at least the standard error** (bias / SE ≥ 1, in bold): the
   cliquets (4.6 and 6.2×), the Asian call (2.5×), the variance, volatility
   and corridor swaps, the touches, the forward-start call, the European put.
   On these, more paths buy nothing: the floor is the time step (stopping
   rule 3 of #331). Even the SLV European call carries 0.30 % of bias at the
   default 252 steps a year.
3. **The greeks are the binding constraint.** At the paths where the price is
   at 0.02-0.5 %, deltas are at 10-50 % on barriers, touches, lookbacks and
   the corridor swap, and pure noise on the Athena autocall (177 %), the
   variance and volatility swaps (150 %) and several multi-asset products.
   A precision target on the price alone would leave them as they are: they
   need their own tolerance (#331 step 2).
4. **Under SLV the page has no aggregate vega.** The adjoint gives one
   sensitivity per leverage-grid node (12 703 for SPY), each mostly noise,
   and no parallel vega with its standard error. The market-inferred target
   (ε ≈ k × |vega| × δσ) needs one: an AAD sensitivity to a single scale of
   the vol surface, so its standard error comes from the same paths.
5. **One default fails:** the quanto call's SAP.DE has no row in
   `prices.dividend_yields` (data-ingest).
6. Rates products are exact to the curve (bonds' SE 0) except the swap
   (3.2 %): its Monte-Carlo noise is all on a near-zero value, which a
   control variate on the closed-form swap would remove.

## 20. Andersen's QE for the price runs of Heston and SLV (#317, 2026-11-19)

§19 showed `auto` prices every single-asset product under SLV, and that on
13 products the discretisation bias was at least the standard error. SPY's
calibrated Heston breaks Feller badly (2κθ/ξ² = 0.13): the variance sits near
0, where full-truncation Euler is most biased.

**What changed.**
- `mc::qe_step` (`models/equity/path_steps.hpp`, host and device): Andersen's
  QE-M step (JCF 11(3), 2008), martingale-corrected, for Heston (L = 1) and
  SLV (L, the leverage at the step's start, scaling ρ and the drift: outside
  Andersen's derivation). Draw 0 sets the variance, draw 1 the spot's
  independent part. Written in the IR's operations and order so that the
  CPU models, the interpreted kernel and the kernel compiled from the IR
  (`ir_path.cpp`, `PathLayout::qe`) give the same numbers on the same draws.
- **Price runs only.** `simulate_script` steps by QE when the model asks
  (`ScriptModelSpec::variance_scheme`, `price_script(variance_scheme=)`,
  Euler by default in C++). Every risk route (`describe_for_risks`: duals,
  IR adjoint, Vibrato, second order) and multilevel (`at_refinement`) step by
  Euler: QE's branch switch and its atom at 0 have no pathwise derivative,
  and its inversion does not couple on summed increments.
- **The API** (`pricing_service._qe_price_euler_risks`): Heston and SLV
  price by QE; with AAD greeks, a QE price run and an Euler adjoint run,
  merged, the diagnostics naming both (and the adjoint run's own price).
  The kernel warm-up compiles the QE price kernels.
- **A fix in the old engine** (`engines/mc/heston_qe.cpp`): the exponential
  branch's moment generating function lacked the atom's probability p, so
  its correction blew the forward up once Feller fails; its tests all
  satisfied Feller.

**Bias against COS** (`benchmarks/heston_qe_bias.cpp`, 1y options, 4M
paths, 16 CPU threads), SPY's fit (Feller 0.13):

| steps/yr | Euler call / put | QE call / put | ms Euler / QE |
|---|---|---|---|
| 16 | +6.74 % / +13.6 % | +0.10 % / −0.29 % | 467 / 800 |
| 32 | +3.20 % / +6.15 % | +0.01 % / −0.03 % | 888 / 1 376 |
| 64 | +1.43 % / +3.11 % | +0.06 % / +0.09 % | 1 557 / 2 516 |
| 252 | +0.32 % / +0.86 % | +0.15 % / −0.17 % | 5 232 / 9 240 |

(noise: 0.05 % call, 0.16 % put). A QE step costs 1.7× an Euler step on
the CPU. **Time to a 0.1 % bias**: call, Euler ~800 steps (~17 s) against QE
32 (1.4 s), ~12×; put, Euler ~2 200 steps (~46 s, extrapolated at 1/n)
against QE 32, ~33×. At Feller 1.5, 0.5 and 0.18 the same picture (the
bench prints all four).

**On the page's products** (SLV, SPY, 50 000 paths, warm kernels): the price
moves where Euler was biased (the globally floored cliquet +0.75 %, 11.8
standard errors; the volatility swap 3.5 %); the price-only run costs +4 %
(median), the page with AAD greeks +11 % (the extra QE run), American
options +60 % (their exercise rule is fitted again for the price run).
Over 400 000 paths, against 504 steps a year, QE is within its noise at 64
steps a year on every product scanned (vanillas, Asian, cliquet, touches,
autocall, volatility swap) and drifts on the cliquet below that (−0.34 % at
32, the leverage frozen over a long step); Euler at 64 is 1 to 9 % off.

**The steps, lowered where it holds (2026-11-19, evening).** The scan above
ran on the 2026-11-18 snapshot. On the 2026-11-19 one (a new SLV fit, its
leverage on 129 time columns dense at the short end), a 1y put under SLV at
64 steps a year was 2 % above 504 a year, at 128 0.65 %, at 252 within its
noise: QE removed the variance's bias, not the one from holding the
leverage over a step. So:
- **Heston: 64 steps a year**, at least 32 over the product's life
  (`pricing_service._steps_per_year`). Against 504 a year, 400 000 paths:
  within 0.15 % on vanillas, Asian and digital from 0.25y up; at 0.1y the
  digital put was 0.75 % off with 6 steps, hence the floor.
- **SLV and local vol: 252**, unchanged. Since `auto` picks SLV for every
  single-asset product, the page's step count does not move; stepping SLV on
  its leverage's own time nodes would be the way to lower it.

**The bias shown, with its error.** The `discretisation_bias` from halving
the steps is |fine − coarse| from one seed: across 8 seeds its standard
deviation is 0.1–0.3 % of the price, under Euler as under QE, about as large
as the values shown. It now carries `discretisation_bias_std_error`: the
spread of the two runs' replicate-wise differences, the Sobol replicates
being shared (common random numbers), or the two errors combined without
replicates (an upper bound). The page shows a bias below twice its error as
"not resolved, below ≈ 2σ", a bound rather than a number the estimate
cannot back. The multilevel route's bias test carries no error yet.

**Not done here.** At 50 000 paths the page's time is mostly fixed costs
(the chain read until the load registry exists, #332).
