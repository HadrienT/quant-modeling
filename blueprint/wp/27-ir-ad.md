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
