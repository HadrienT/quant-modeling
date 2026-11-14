# WP 27 — An IR of our own, and derivatives by source transformation

| | |
|---|---|
| **Status** | In progress: see §14 for what is done and measured (design decided by the maintainer on 2026-11-12, option B of #239) |
| **Depends on** | 16 (scripting), 17 (AAD: the CPU tape stays the oracle), 19 (GPU Monte-Carlo), 26 (scripts compiled with NVRTC, catalogue products as scripts) |
| **Issue** | #239 (and one issue per lot, below) |
| **Rule** | Performance first, the books as references (CLAUDE.md, 2026-11-12) |

## 1. Why

Today a derivative has three implementations:

- **CPU:** Savine's tape. General and exact, but every operation allocates a
  node, and a daily Asian of 100 000 paths takes about 5 s on one thread.
- **GPU, few parameters (Black-Scholes, Heston):** dual numbers. No code to
  write, but the cost grows with the number of parameters (8 at most).
- **GPU, many parameters (local vol):** a per-path adjoint **written by hand**
  for every bytecode instruction and for the model's step, run by the
  interpreter only (the compiled kernels price, they do not differentiate).

The hand-written part is the bottleneck. SLV, more than two assets and the
CVA's sensitivities are missing because each needs another adjoint written,
and checked, by hand. Second-order risks exist only on the CPU tape and are
not exposed.

WP 27 replaces the hand-written derivatives with **one mechanism**: the
script and the model's step become a small program in an intermediate
representation (IR) of our own, and the derivatives are **new programs built
from it by a transformation** (forward, reverse, and the two composed for the
second order). The GPU then compiles them like the prices (NVRTC, P1b).

## 2. Decisions (maintainer, 2026-11-12)

1. **Option B:** an IR of our own and source-transformation AD, not
   Enzyme or CppADCodeGen (§12 says why), not more hand-written adjoints.
2. **Regularity**, for derivatives that are exact on every path: the four
   rungs of §8, **in that order** — conditional expectations first, then C¹
   smoothing, then Vibrato, the bump on the adjoint last.
3. **Second order in the API and on the front end**, not only in the core.
4. The CPU tape (WP 17) stays, as the **oracle** every new derivative is
   checked against, path by path.

## 3. What an IR is, and why ours is small

A compiler does not go straight from text to machine code. It goes through
an **intermediate representation**: simpler than the language (no syntax
sugar, every operation explicit), more abstract than the machine (no
registers, no memory addresses). LLVM IR is the best-known one: C, C++, Rust
and CUDA all go through it. Every optimisation and every transformation is
written once, on the IR, and serves every language above and every machine
below.

Ours does the same for a much smaller world. A script and a model step only
ever do arithmetic on real numbers, call a few functions (`exp`, `log`,
`sqrt`, Φ⁻¹, a grid lookup), compare, branch, loop over dates, and read the
market state of the current date. There are no pointers, no allocation, no
aliasing, no recursion. That is what makes an IR of our own reasonable: the
hard parts of a general compiler (memory, aliasing, calls) do not exist here.

### 3.1 SSA: every value assigned once

The IR is in **static single assignment** form: every intermediate value has
a name and is assigned exactly once. The script

```
2027-01-04
    acc = acc + spot()
    pays max(acc / 252 - 100, 0)
```

becomes, for that date,

```
%1 = load_spot 0
%2 = add %acc.in, %1
%3 = div %2, 252.0
%4 = sub %3, 100.0
%5 = max %4, 0.0
%6 = load_numeraire
%7 = div %5, %6
%payoff.out = add %payoff.in, %7
%acc.out = %2
```

SSA is what makes derivatives mechanical: each line is one operation whose
local derivatives are known (`d%2/d%1 = 1`, `d%5/d%4 = [%4 > 0]`, …), and the
names say which value feeds which. The bytecode's stack machine hides that
(a value is "whatever is on top of the stack"); SSA states it.

### 3.2 Control flow stays structured

A script's `if` and a model's loop over its steps are kept as **structured
regions** (an `if` with its two blocks, a loop with its body), not as jumps.
That keeps the reverse pass simple: a region is reversed as a whole. A pure
`if` (no `pays`, no assignment to a variable read later) becomes a `select`
(both sides computed, one kept), which the GPU prefers and the derivative
handles like any operation; an `if` with side effects stays a region, and the
forward pass records which way it went (one bit) for the reverse pass.

The fuzzy `if` is a region whose two blocks both run and are blended — the
same structure, with the blend as ordinary arithmetic.

### 3.3 The model's step is in the IR too

The model describes its step with an **IR builder** in C++ (functions that
append operations), once. Black-Scholes's step is three operations; local
vol's adds a grid lookup (`grid_bilinear`, an IR operation whose derivative
with respect to the grid points is known); Heston's adds the variance update.
A script and its model become one program per date: model step(s), then the
script's statements.

This is where the hand-written adjoints disappear: a new model is written
once, as its price, and every derivative of it follows.

### 3.4 The operations

| Family | Operations |
|---|---|
| Arithmetic | `add sub mul div neg` |
| Functions | `exp log sqrt pow abs min max inv_normal` (Φ⁻¹: AS 241, as P1i) |
| Logic | `cmp_lt cmp_le cmp_eq …`, `select`, fuzzy `ramp` (§8) |
| Market state of the date | `load_spot i`, `load_df k`, `load_survival k`, `load_numeraire` |
| Model | `draw j` (the Gaussian of the step), `grid_bilinear`, `param p` (a differentiable input: spot, rate, a vol, a grid point) |
| State | variables in and out of a date, `pays` |
| Regions | `if`, fuzzy `if`, `loop` over steps |

Every operation carries the rules the transformations need (§5) — its local
derivatives, and what its reverse needs from its forward.

## 4. The pipeline

```
script text ──parser──> AST ──lowering──> IR ──passes──> IR ──transformations──> IR' ──backends──> code
model step  ─────────────IR builder──────────┘                (tangent, adjoint,          CUDA (NVRTC)
                                                                tangent∘adjoint)          CPU bytecode
```

- **Lowering:** the AST (after the existing passes: variable indexing, domain
  processing, df and barrier lookups) to IR. The parser and the language do
  not change.
- **Passes:** constant folding, common subexpressions, dead code (a value
  nothing reads, a variable never paid from). They follow the bits rule of
  P1: no reassociation, no contraction into FMA, the same operations in the
  same order as the interpreter, so that a price stays bit for bit.
- **Backends:** CUDA source compiled with NVRTC, as P1b does from the
  bytecode today; and the existing bytecode for the CPU (scalar and batched
  interpreters), so that the CPU keeps its current paths unchanged.

## 5. The transformations

### 5.1 Tangent (forward mode)

For k directions (the parameters one differentiates), every value `v`
carries `v̇₁ … v̇ₖ`, and every operation computes them from its operands'
(`mul a, b` → `ȧ·b + a·ḃ`). This is what the dual numbers do today, but as
generated code: the compiler sees the whole computation, keeps the tangents
in registers and drops the ones that are always zero.

Cost: about k + 1 prices. Right for few parameters and for the second order
(§5.3).

### 5.2 Adjoint (reverse mode)

One forward pass computes the values; one backward pass, through the
operations in reverse order, propagates `v̄ = ∂payoff/∂v` from the payoff to
the parameters (`mul a, b` sends `v̄·b` to `ā` and `v̄·a` to `b̄`).

Cost: about 3–4 prices, **whatever the number of parameters** — every
local-vol grid point at once.

The question is what the backward pass needs from the forward one. The tape
answers "everything, always". We answer at compile time:

- **Checkpoints at dates.** The forward pass keeps the state at the start of
  each date (the model state and the script's variables): a few numbers per
  date per path. The backward pass, date by date from the last, recomputes
  that date's forward operations from its checkpoint, then reverses them.
  Within a date, nothing is stored but what the recompute gives back.
- **Branches:** one bit per `if` (and per `min`/`max`) recorded forward, read
  backward.
- **Draws:** not stored; regenerated (Philox and Sobol are counters: draw j
  of path p is a pure function of `(seed, p, j)`), as the current per-path
  adjoint does.

The memory per path is therefore known before the kernel runs (checkpoints ×
dates + bits), small, and in registers or local memory, never allocated.

### 5.3 Second order: tangent over adjoint

Applying the tangent transformation to the adjoint program gives, in one
pass, `∂²payoff/∂θ ∂θ_d` for every parameter θ and one direction d — a row of
the Hessian. Gamma is the spot direction's row read at the spot; vanna, the
same row read at the vol; volga, the vol direction's row. One pass per
direction asked, each costing a few adjoints.

This is exact only where the payoff is smooth enough on each path (C¹ in the
parameters): §8 is what makes it so.

### 5.4 How we know it is right

- **The tape:** for every library script and every model, the adjoint's
  gradient equals the CPU tape's on the same path, to 1e-12 relative.
- **Finite differences:** the tangent and the adjoint agree with each other
  and with central differences of the price.
- **Symmetry:** `∂²/∂θᵢ∂θⱼ = ∂²/∂θⱼ∂θᵢ` from two directions.
- **The bits of the price:** the IR's CUDA backend prices to the same bits as
  today's compiled kernels.

## 6. GPU specifics

- The derivative kernels are compiled per request like the prices
  (specialised to the model and the script's constants, cached by hash).
- Gradients summed over paths through the logical blocks (no `atomicAdd`),
  the same bits on one or two V100s.
- Register pressure is the risk: an adjoint kernel holds more live values
  than a price. Measured per model, with the occupancy lever of #130 if
  needed.

### 6.1 What is code and what is data

A compiled kernel is specialised to what it is built from; everything else
is an argument. Today (P1b) the cache key is (GPU architecture, generated
source), and the source holds:

- the script's operations, **with the numbers written in the script as
  literals** (exact hexadecimal floats);
- the request's shape (`CudaKernelSpec`): the model kind, the number of
  assets, the sampling options.

The market inputs (spot, rate, dividend, vols, a local-vol grid, Heston's
parameters, correlations), the event times, the discount maturities, the
number of paths and the seed are arguments: changing them reuses the kernel.

Three things keep the catalogue from being compiled once and for all:

1. **Product terms are literals.** WP 26 P3's builders write the strike, the
   barriers, the coupons, the notional and, for the lookback, the autocall,
   the worst-of and the mountain, the initial spot into the script: every new
   set of terms is a new kernel (about 0.5 s on a V100).
2. **The number of dates is code.** The generated function dispatches on the
   event index; identical events share their code, but an Asian on 252
   fixings and one on 253, or an autocall on 8 dates and one on 12, are two
   kernels.
3. **The cache lives in the API process.** It is empty after every restart
   and every deployment: the first user of each product pays the compile.

Lot L1b removes the three:

1. **Terms as kernel parameters.** A term becomes an IR input (`param`, like
   a market input) rather than a constant: the builders mark the numbers that
   are terms, and the backend passes them as arguments. Scripts written by
   users keep their numbers as literals (they are the script).
2. **Dates as data.** The kernel reads the events' kinds and order from an
   array and runs each distinct event's code from it, instead of a `switch`
   over every index: one kernel per product *shape* ("autocall, memory
   coupon, terminal knock-in"), whatever its number of dates.
3. **A cache on disk.** The compiled binary (cubin) stored under the hash of
   its source, the GPU architecture and the library's version: it survives
   restarts, and deployments that leave the generated code unchanged.
4. **A warm-up at start.** The API compiles the catalogue's shapes when it
   starts (as `calibration_warmup.py` warms the calibrations), before the
   first request.

The bits do not move: a term read from an argument is the same double as the
literal it replaces, and the operations stay in the same order (checked
against the literal kernels).

Then a catalogue product never waits for a compile, whatever its terms and
its dates; a script written live on the scripting page compiles once, at its
first use, and is cached like the rest. The CPU interprets and compiles
nothing.

## 7. CPU specifics

The CPU keeps its tape for the oracle and for what the IR does not cover yet.
Once the adjoint IR runs through the batched interpreter (lot L7), the CPU's
risks come from it too, several times faster than the tape.

## 8. Regularity: making pathwise derivatives exact

A pathwise derivative is the derivative of the payoff on one path, averaged.
It is right only if the payoff is continuous in the parameters on each path
(first order) or continuously differentiable (second order). A hard barrier,
a digital or the kink of a call break that. In the order decided:

1. **Conditional expectations, computed by the model.** Integrate the
   discontinuity out analytically, where a formula exists. `survivalAbove /
   survivalBelow` (WP 26 P2) already does it for barriers. Extended to the
   last step of a path: a payoff `max(S_T − K, 0)` replaced by its expectation
   over the last model step given the state before it — Black-Scholes's
   formula on one step, smooth to every order. No bias.
2. **C¹ smoothing in the fuzzy logic.** The fuzzy ramp today is linear, with
   corners where it meets 0 and 1: its derivative jumps there, and the
   pathwise second derivative of a fuzzy call is twice the true gamma
   (`etc/bytecode-aad-gpu-ordre-2.md` §III.3). A ramp with smooth joins
   (C¹ or C², e.g. a smoothstep polynomial) makes the tangent-over-adjoint
   exact for the smoothed payoff. The smoothing's bias stays controlled by ε,
   and is said.
3. **Vibrato (Giles).** Pathwise everywhere but on the last step, a
   likelihood-ratio weight there. Unbiased even for a digital, noisier: for
   what 1 and 2 do not cover.
4. **The bump on the adjoint** (WP 17's method): an adjoint at the spot
   bumped up and down. Biased in h², robust; the fallback and an oracle.

The engine picks, per product, the first rung that applies, and the
diagnostics say which.

## 9. Second order in the API and on the front end

- **API:** the pricing responses gain second-order greeks with their errors:
  gamma (already reported, now by tangent over adjoint where exact), vanna,
  volga, and the cross-gammas for several underlyings; the method in the
  diagnostics. The OpenAPI document and `schema.gen.ts` are regenerated (ADR-005).
- **Front end:** the greeks table shows a second-order section, each number
  with its error and the method that gave it; nothing shown without its
  error (displayed metrics must be defensible).

## 10. Lots

| Lot | Content | Acceptance |
|---|---|---|
| **L1** IR and lowering | The IR's data structures, the lowering from the AST, the passes, a printer; the CUDA backend for prices | Every library script prices **to the same bits** as P1b's compiled kernels, on BS, local vol, Heston |
| **L1b** Compiled once | Terms as kernel parameters, dates as data, a cache on disk, a warm-up of the catalogue's shapes at start (§6.1) | Same bits as the literal kernels; one kernel per product shape; no catalogue request compiles after start (measured: the first request after a restart at the warm cache's latency) |
| **L2** Model steps in the IR | BS, local vol, Heston written with the IR builder; the price kernels generated from model + script together | Same bits as the current kernels; the hand-written GPU step code no longer used for scripts |
| **L3** Tangent | The forward transformation; replaces the GPU duals | Same risks as the duals to 1e-14; no direction limit of 8 |
| **L4** Adjoint | The reverse transformation, checkpoints at dates, branch bits | Gradient = CPU tape per path to 1e-12 on every library script and model; local vol faster than today's hand-written per-path adjoint; the hand-written instruction adjoints deleted |
| **L5** More models | SLV, several assets beyond two, jump models (where the pathwise derivative exists) in the IR | GPU risks for each, checked against the tape |
| **L6** Second order | Tangent over adjoint; the regularity rungs of §8 in order; API and front end | Gamma, vanna, volga against closed forms (BS vanilla, barrier); symmetry; the fuzzy call's factor 2 gone with the C¹ ramp |
| **L7** CPU | The adjoint IR through the batched interpreter | Same gradients as L4; faster than the tape |
| **L8** CVA (WP 23) | The exposure engine's sensitivities on the same machinery | Against the tape of lot X8 |

## 11. What does not change

The language, the parser, the scripts users write, the CPU bytecode
interpreters (still the CPU's path and the fallback), the CPU tape (the
oracle), the logical-block reduction and its bits.

## 12. Options not taken

- **More hand-written adjoints (option A):** a linear cost in models and in
  checks; what WP 27 exists to end.
- **Enzyme (AD on LLVM IR):** mature, but needs clang instead of NVRTC, so a
  full LLVM toolchain in the production image, and differentiates our C++ as
  written (templates, indirections) rather than the small programs above.
- **CppADCodeGen (tape once, generate code):** closer to the tape, but a tape
  recorded on one path does not hold for another when branches depend on the
  data, which every barrier and autocall does.

## 13. Risks

- **Compile time:** an adjoint kernel is several times a price's; NVRTC's
  cost per new script grows. Mitigation: lot L1b (terms and dates as data,
  one kernel per product shape, a cache on disk, a warm-up at start).
- **Registers:** see §6.
- **Bits:** the passes must not reorder floating-point operations (§4).
- **Scope:** L1–L4 replace what exists; L5–L8 extend it. The lots are
  ordered so that each one ends with something measured and merged.

## 14. Progress

### 14.1 L1 done: the IR, its lowering, and the CUDA backend for prices (2026-11-14)

What was built:

- `scripting/ir.hpp`, `src/scripting/ir.cpp`: the instructions (§3.4), a
  block per event, the builder, the lowering from the tree
  (`ir::lower_script`, a visitor that mirrors `scripting/compiler.hpp`
  statement for statement), dead-code removal, a printer
  (`ir::to_string`), and a reference evaluator on the CPU (`ir::run_block`).
  `ScriptedProduct::ir()` holds it next to the bytecode.
- `src/scripting/ir_cuda.cpp`: the CUDA backend. It replaces the
  bytecode-to-CUDA generator of WP 26 P1b, which is deleted; the kernel it
  links into (`src/gpu/script_jit_kernel.cu`) is unchanged.

How the design's choices came out:

- **SSA with structured regions**, as §3.1–§3.2. Every variable is renamed
  at each assignment; a hard `if` ends with one `phi` per variable its two
  blocks leave different. A fuzzy `if` needs no save and restore of the
  variables any more: both blocks read the values from before the `if`, and
  a `fuzzy_phi` picks the then-value, the else-value or their blend from the
  mode (the degree at or below 0, at or above 1, in between). The blend is
  one instruction with its own rule rather than three arithmetic ones, so
  that a block that did not run is never read.
- **Passes at construction.** The builder shares a value when the same
  operation on the same operands is visible from the current region (common
  subexpressions, scoped by region), and folds an operation on constants only
  through `+ − × ÷ √ min max abs neg`, which IEEE 754 rounds the same on
  every machine; `exp`, `log` and `pow` are left to run time (their last bit
  is the library's, and the GPU's is not the host's). Dead code is removed
  per event.
- **Not done in L1** (no measured need): turning a pure `if` into a `select`
  (the device compiler already does it where it pays), dead variables across
  events, and the `loop` region -- the loop over the model's steps stays in
  the kernel that calls the generated code, which is how the adjoint will
  walk it backwards too (§5.2's checkpoints are per date).

Acceptance (same bits as P1b's kernels):

- CPU, in the CI: `tests/testScriptIr.cpp` -- every library script, crisp and
  fuzzy, event by event on 400 random paths: every variable and the payoff
  equal the bytecode interpreter's bit for bit (and the fuzzy blend mode is
  hit more than 1 000 times).
- GPU: `tests/gpu/testGpuScriptJit.cpp`, unchanged -- the kernel compiled from
  the IR against the interpreter on the device, Black-Scholes (one and
  several assets), local vol, Heston and SLV, crisp and fuzzy, Philox and
  Sobol: same bits.

Measured (one V100, `qm_script_jit_bench`, Philox + antithetic, ms for the
paths of §11's cases; before = the bytecode generator on the same commit):

| Case | Before | From the IR |
|---|---|---|
| barrier_bs | 6.48 | 6.35 |
| barrier_lv | 8.81 | 8.77 |
| asian_bs | 16.47 | 15.69 |
| asian_lv | 20.62 | 20.08 |
| autocall_bs (3 assets) | 3.48 | 3.48 |

The same prices to the bit, 0 to 5 % faster (shared subexpressions).

### 14.2 L1b done: compiled once (2026-11-14)

The four items of §6.1, in the order built:

1. **Dates as data.** The generated function no longer has one `case` per
   event index but one per distinct block; which block a date runs is an
   array on the device (`ir::Script::event_block`). An Asian on 252 fixings
   and one on 253 are the same source.
2. **Terms as kernel parameters.** `ScriptSettings::terms_as_parameters`
   lowers every number of the script to a `param` read from an array of
   terms, one array slice per event (`ir::Script::event_terms`), instead of a
   constant. It is set for the scripts the library writes
   (`pricers/script_route.cpp`: the catalogue's routes); a script a user
   wrote keeps its numbers as constants. Two dates of an autocall with
   different coupons are then the same block, and two autocalls with other
   barriers the same kernel. Not done as "the builders mark the numbers that
   are terms": every number of a library script is a term, which needs no
   marking and no change to the language.
3. **A cache on disk** (`src/gpu/script_jit.cpp`). Each linked cubin is
   written under the hash of (the embedded kernel it was linked with, the
   architecture, the source), in `QM_KERNEL_CACHE_DIR` (default
   `~/.cache/quantmodeling/kernels`; `/app/data/kernels`, on the data volume,
   in the compose files; `QM_KERNEL_CACHE=0` turns it off). The file holds
   the source, compared on load, so a hash collision cannot hand out the
   wrong code. A kernel is 35 to 60 kB.
4. **A warm-up at start** (`api/app/kernel_warmup.py`, `QM_KERNEL_WARMUP=1`,
   set in production). Two steps in a background thread: the kernels the
   cache holds from an *older build* are compiled again from the source
   stored with them (`rebuild_script_kernels`: after a deployment that
   changes the embedded kernel, every script priced before -- catalogue and
   users' -- is ready again); then 47 seed pricings of the catalogue's usual
   shapes (call / put, the four barrier kinds, the averages, the lookback
   styles, the multi-asset products) on 4 096 paths, which compile what the
   cache has never seen.

Acceptance:

- **Same bits as the literal kernels.** `tests/testScriptIr.cpp` runs every
  library script with its numbers as terms through the IR evaluator against
  the bytecode (which holds them as constants): same bits.
  `tests/gpu/testGpuScriptJit.cpp` does the same on the device
  (Black-Scholes, and local vol fuzzy under Sobol).
- **One kernel per product shape.** `ScriptCudaCodegen.
  TermsAndDatesAreDataNotCode`: another strike and barrier, or 30 monthly
  dates instead of 12, give the identical source.
- **No catalogue request compiles after start.** Measured with
  `qm_script_jit_bench` (first call minus the median of the next five, per
  case): 405 to 571 ms in a process that compiles, 0 ms in a second process
  that reads the cache. The warm-up takes 31 s on an empty cache (45 kernels
  compiled, in the background) and 1.7 s when the cache is warm.
  `GpuScriptJitTest.KernelsAreReadBackFromTheDiskCache` checks the read-back,
  a corrupt file ignored, and a stale kernel rebuilt from its source.

What it costs, measured (one V100, ms, the script's numbers as constants
against as terms; `qm_script_jit_bench 1 0 "" philox|sobol terms`):

| Case | Philox, constants | Philox, terms | Sobol, constants | Sobol, terms |
|---|---|---|---|---|
| barrier_bs | 6.35 | 6.62 | 13.24 | 13.61 |
| barrier_lv | 8.77 | 9.27 | 14.9 | 17.7 |
| asian_bs | 15.69 | 15.96 | 36.24 | 36.77 |
| asian_lv | 20.08 | 20.43 | 44.03 | 44.47 |
| autocall_bs | 3.48 | 3.62 | 5.62 | 5.62 |

2 to 6 % a path (a term is a load, where a constant was folded into the
code), 18 % in one case (the local-vol barrier under Sobol, repeated three
times), against the 0.4 to 0.6 s every new set of terms cost before. The
same prices to the bit.

Found on the way: NVRTC and nvJitLink already keep a cache of their own
(`~/.nv/ComputeCache`): a second process compiled the same source in about
35 ms instead of 500. It is capped at 1 GiB, shared with every other CUDA
program of the user and lost with the container's file system, hence the
cache of our own on the data volume.

### 14.3 L2 to L5 done: the path in the IR, tangent and adjoint by transformation (2026-11-14)

**What was built.**

- `scripting/ir_path.*` (L2). A path is three kinds of block on one state
  array `[spots | variance | script variables | payoff]`: `init` (the state
  at t = 0 from the parameters), the model's `step`, and the script's events
  bound to the state. The steps of Black-Scholes (one and several assets),
  local vol, Heston and SLV are written once with the IR builder, operation
  for operation as `path_steps.hpp` and `mc::script_path` compute them. The
  grid lookups are IR operations (`grid_vol`, `grid_leverage`) that call the
  models' own functions; their slope and the share of each grid point are
  in `scripting/ir_grid.hpp`. Two variants, as the hand-written path has for
  `double` and `Dual`: the price (Black-Scholes on its tabulated
  coefficients, numeraire and discount factors read as data) and the one the
  derivatives are taken from (Black-Scholes on its parameters, `exp(r t)`
  from the rate).
- `scripting/ir_ad.*` (L3, L4). `tangent(block, directions)` and
  `adjoint(block)` return blocks. Neither knows a model or a product: only
  the IR's operations, with the tape's local derivatives and its choices at
  a kink. A derivative known to be zero when the block is built is not
  emitted. The adjoint recomputes the block's values, then walks it
  backwards; an `if` becomes an `if` on the same condition around the
  reverse of the side that ran, a fuzzy `if` two guarded regions (one per
  side) with the blend's weights, and the adjoints the two sides leave
  different are merged by phis.
- `scripting/ir_cuda.cpp`, `src/gpu/script_jit_kernel.cu`. The generator
  writes whole paths (`cuda_path_source`: price, tangent or adjoint), and
  the kernel file is compiled three times (one fatbin per kind). What stays
  hand-written is what is the same for every script: the loop over the
  steps, the draws, the antithetic pair, the logical blocks, and for the
  adjoint the checkpoints. The state's arrays are declared in the generated
  source, sized for the script.
- The adjoint's memory, as §5.2 planned: per path, the model's state before
  each step and the script's variables and payoff before each date (rolled
  back in place, last in first out); no record per operation, no branch
  bits (the recomputed values give the conditions), draws regenerated. The
  scalar parameters' adjoints stay in registers for the path; only the
  grid's go to the thread's row in global memory.

**Departures from the design.**

- No `loop` region in the IR: the loop over steps is the kernel's (and the
  host runner's), forwards and backwards.
- One state array rather than separate model state and variables: a spot
  read by the script *is* the model's slot, so no copy at a date.
- L5's "jump models" and "several assets beyond two": the second came free
  (the Black-Scholes step is written for n assets; the tangent has no limit
  of 8 directions); jumps are not done (the device has no jump model).
- "The hand-written instruction adjoints deleted" (L4): **not deleted.**
  `bytecode_adjoint.hpp`, `script_adjoint.hpp` and the dual path stay as the
  oracle the generated kernels are tested against (`QM_SCRIPT_JIT=0` runs
  them), and as the fallback when compilation is off. Deleting them is a
  decision for the maintainer once the generated kernels have been in
  production.
- L3's tolerance: the tangent equals the duals to 1e-12 relative, not
  1e-14 (the formulas are equivalent, not identical: one case of the
  library, a corridor variance swap under Heston, differs by 1.3e-13). The
  prices are the same bits.

**Acceptance.** On the host, in the CI (`tests/testScriptIrPath.cpp`,
`tests/testScriptIrAd.cpp`; every library script, crisp and fuzzy):

- a path run from the blocks is `mc::script_path`'s bit for bit -- price
  variant under Black-Scholes, local vol, Heston and SLV, with numbers as
  constants and as terms; risk variant against the dual path's value;
- tangent = dual path: same value bits, derivatives to 1e-12;
- adjoint = tangent to 1e-11 (two transformations sharing only the table of
  local derivatives), under Black-Scholes, Heston, local vol and SLV;
- adjoint = the CPU tape (`simulate_aad`, 256 paths), same four models;
- under local vol, adjoint = the adjoint written by hand, path by path,
  every grid point, to 1e-11, and the same price to the bit.

On the device (`tests/gpu/`):

- prices: compiled = interpreted, same bits, every model, Philox, Sobol and
  stratified, with spot control variates too (`SpotControlsSameBits`: #239's
  "compiled control variates");
- `TangentKernelIsTheDuals` (same price bits, risks to 1e-12),
  `AdjointKernelIsTheHandWrittenAdjoint` (same price bits, 1e-11);
- the earlier tests against the tape now run on the generated kernels;
- L5: `SlvRisksMatchTheTape` (8 parameters and the leverage grid) and
  `ThreeAssetRisksMatchTheTape` (10 directions) -- two cases that had no GPU
  risks before.

**Measured** (one V100, `qm_gpu_risk_bench <paths> gpu`, median of three
calls after the first, ms; before = `QM_SCRIPT_JIT=0`, the duals on the
interpreter and the hand-written adjoint):

| Script, model (risks) | 100k before | 100k now | 1M before | 1M now |
|---|---|---|---|---|
| phoenix-autocall, Black-Scholes (4, tangent) | 5.4 | 1.3 | 35.9 | 2.7 |
| phoenix-autocall, Heston (8, tangent) | 24–130 | 13.8 | 183 | 122 |
| up-and-out-call, Heston (8, tangent) | 35–41 | 11.1 | 271 | 96 |
| european-call, local vol (363, adjoint) | 61.5 | 75.5 | 460 | 448 |
| up-and-out-call, local vol (363, adjoint) | 128 | 98 | 753 | 744 |
| phoenix-autocall, local vol (363, adjoint) | 103 | 127 | 766 | 986 |
| variance-swap, local vol (363, adjoint) | 165 | 109 | 692 | 717 |

- **Tangent: the gain is there** -- 13 times under Black-Scholes at a
  million paths, 1.5 to 2.8 times under Heston.
- **Adjoint under local vol: on par, not faster.** L4's criterion ("local
  vol faster than today's hand-written per-path adjoint") is **not met**: two
  cases a little faster, one equal, one 29 % slower (the autocall, whose
  eight dates checkpoint every variable). Both adjoints share what #130
  measured as the bottleneck -- four read-modify-writes of the thread's
  gradient row in global memory per step -- and the generated one has not
  moved it yet. Next levers, not tried: checkpoint only the variables a
  date overwrites and a later date reads; keep the gradient of the current
  time slice (the two grid columns a step can touch) out of global memory.
- Prices with the model's step generated (`qm_script_jit_bench`, against
  §14.1's column): within 2 % either way, except asian_lv under Philox,
  20.1 → 21.7 ms. Same bits.
- The first call of a new script: 0.5 to 1.2 s to compile a price kernel
  (more code than the events alone), then the caches of §14.2.

**Not done in these lots:** the API's choice between tangent and adjoint is
by model (grid models take the adjoint) rather than by cost; multilevel
risks (`simulate_script_aad_mlqmc_gpu`) reach the generated adjoint through
the same entry point but were not re-measured.

### 14.4 L7 measured: the IR's adjoint on the CPU is correct, and slower than the tape (2026-11-14)

`simulate_script_aad_ir` (`engines/mc/script_engine.hpp`) runs the adjoint
blocks of §14.3 on the CPU with the IR's reference evaluator
(`ir::run_block`, one instruction at a time, one path at a time), with the
tape's paths, labels and batches of 64. `qm_script_ir_adjoint_bench`, 20 000
paths, one thread:

| Script, model (risks) | Tape ms | IR ms | Tape / IR | Largest gap |
|---|---|---|---|---|
| european-call, Black-Scholes (4) | 11 | 23 | 0.5 | 2e-16 |
| asian-call, Black-Scholes (4) | 61 | 113 | 0.5 | 2e-16 |
| phoenix-autocall, Black-Scholes (4) | 26 | 79 | 0.3 | 1e-16 |
| up-and-out-call, Heston (11) | 4 089 | 5 869 | 0.7 | 8e-16 |
| variance-swap, Heston (11) | 4 883 | 6 380 | 0.8 | 1e-16 |
| european-call, local vol (363) | 2 467 | 2 972 | 0.8 | 1e-16 |
| up-and-out-call, local vol (363) | 3 747 | 4 258 | 0.9 | 8e-18 |
| phoenix-autocall, local vol (363) | 3 812 | 5 066 | 0.8 | 9e-18 |

**Same gradients as the tape (to 1e-16), not faster: the lot's criterion is
not met, and no route uses it.** The CPU's risks stay on the tape.

Why, and what would change it: the reference evaluator dispatches on every
instruction for one path and clears its values at each block; the tape pays
a node per operation but its sweep is a tight loop. The lot's design was
"through the batched interpreter", which this is not. In SSA the batched
form is simple -- both sides of an `if` can be computed for every lane and
the phis select, so a block is a straight line of loops over 8 lanes with no
control flow (only `grid_accumulate` needs a mask) -- and P1a measured 5 to
13 times from that on prices. Not built: with risks on the GPU (§14.3) the
CPU's are the fallback, so second order (L6) comes first.
