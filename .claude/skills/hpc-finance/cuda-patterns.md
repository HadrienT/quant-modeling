# CUDA patterns (V100, sm_70, CUDA 12.4, host compiler g++-13)

Pick the pattern from the **profiler's diagnosis**, not from intuition.

## Diagnosis → what to try

| `ncu` says | Likely cause | Try |
|---|---|---|
| "uncoalesced global accesses", many excessive sectors; stalls on L1TEX | per-thread rows stored path-major | date-major layout through `Strided<T>` (below) |
| Memory and compute throughput both low, "No Eligible" high | latency-bound: too few warps or long dependency chains | more independent work per thread (unroll over dates/trades), raise occupancy (fewer registers: `__launch_bounds__(256, minBlocks)`, smaller live state) |
| Stalls on "LG Throttle" / local memory traffic | large per-thread arrays or stack spill to local memory | keep the live set in registers, split the work (e.g. derivatives in chunks of N), shared memory for data reused by a block |
| Compute (SM) throughput high, FP64 pipe dominant | compute-bound: the math itself | fewer transcendental calls (hoist `exp` out of loops, reuse a stored discount factor), algebraic simplification **that keeps the bits** or is validated against tolerances |
| Grid of ~1–2 waves, last wave partial | tail effect | more paths per launch or several segments per launch (`run_logical_block_segments`) |
| Host time between kernels in a timeline | synchronous pageable copies, per-call `cudaMalloc` | pinned host buffers, `cudaMemcpyAsync` on streams, double-buffered launches, memory pool |

## Coalescing: one path per thread, date-major rows

A warp runs 32 consecutive paths. If each thread keeps its row of `n` dates
contiguously (`base + path * n + i`), the warp's 32 accesses at date `i` are
`n * 8` bytes apart: up to 32 transactions. Date-major (`base + i * rows + path`)
makes them one 256-byte line.

```cpp
#include "quantModeling/core/strided.hpp"
// rows = capacity of the launch (slots per date), local = path slot
const Strided<Real> x{scratch + local, rows};
x[i] = ...;                       // base[i * rows]
shared_host_device_function(x);   // takes `const State &state`, indexes state[i]
```

Shared `QM_HOST_DEVICE` functions take the row as a template parameter
(`template <class T, class State> ... const State &state`), so the host keeps
passing plain pointers and the AAD tape is untouched. Data read uniformly by
the whole warp (trade program, curve coefficients) is a broadcast and is fine
in global memory; `const __restrict__` lets the compiler use the read-only path.

Measured here: netting kernel of `src/gpu/exposure.cu`, excessive sectors 52 % → 1 %,
161.6 ms → 59.1 ms a launch, now compute-bound (SM 31 % → 75 %).

## Reductions: deterministic or nothing

- Accumulate per thread in a fixed order, then warp shuffle (`detail::shfl_down`
  of the whole stats struct), then warps in warp order through shared memory,
  one partial per logical block, folded on the host in block order.
- The result must be the same bits whatever the number of devices and the
  launch size. A test like `GpuTwoCardsTest.*SameBits*` guards it: keep one for
  every new reduction.
- Never `atomicAdd` on floating-point results (order-dependent).

## Random numbers

- Philox (`utils/philox.hpp`): counter-based, `philox_uniform(seed, path, draw)`,
  no state to store or skip ahead; `PhiloxGaussianSource` for a path's sequence.
- Sobol (`utils/sobol.hpp`): point index → direction numbers; scrambling by
  digital shift or Owen (`qm_gpu_owen_bench`). Brownian bridge for the time
  dimension (`engines/mc/sobol_bridge.hpp`).
- Inverse CDF (`inverse_normal_cdf`) rather than Box-Muller: one uniform per
  normal, keeps the Sobol dimension mapping.

## Multi-GPU

Split **contiguous ranges of logical blocks** between devices, one host thread
per device, partials concatenated in block order (`detail::run_on_devices`).
Bound every launch by the device's own range (`std::min(per_launch, last - b)`).

## Memory and launches

- Launch size from free memory (`free_memory(device) / 2`), never a hard-coded
  default; the LLM shares the card.
- `StackLimitRestore` around kernels with a large stack frame: the driver keeps
  `stack × resident threads` (2 048 × 80 SMs) reserved otherwise.
- RAII for device memory; check every `cudaGetLastError()` after a launch.
- Templates for flags known per request (instead of runtime branches);
  runtime branches that are uniform across the grid cost little.

## Profiling commands

```bash
ncu --kernel-name regex:<kernel> --launch-skip 4 --launch-count 1 \
    --section SpeedOfLight --section Occupancy --section WarpStateStats \
    --section SourceCounters --section MemoryWorkloadAnalysis <binary> <args>
```

`--launch-skip` avoids the warm-up launches. `nsys` is installed but cannot
import its reports on this server (missing importer).
