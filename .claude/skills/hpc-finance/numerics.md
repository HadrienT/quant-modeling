# Numerical rules for optimized pricing code

## What "the numbers did not move" means

| Change | Expected | How to check |
|---|---|---|
| Memory layout, launch configuration, buffers, streams, RAII | **identical bits** (same operations in the same order) | full-precision output before/after (benchmarks print `%.17g`); GPU tests; `qm_compare` |
| Number of threads / GPUs | **identical bits** (logical blocks) | `GpuTwoCardsTest.*SameBits*`, CPU thread-count tests |
| Reordered floating-point sum, fused operations, different `exp` implementation | differences at the last bits only: relative ~1e-15 per operation, accumulated | compare against the previous build within the CPU/GPU tolerance; say why |
| CPU vs GPU on the same draws (since 2026-11-16) | prices within **1e-12 relative**, risks within **1e-10 relative** -- FMA, the device libm and reassociated sums allowed | `tests/support/cpu_gpu_tolerance.hpp`; `|GPU − CPU| / std err` printed by the benches |
| One configuration run twice (threads, GPUs, launches) | **identical bits** (deterministic: the API's replays) | `GpuTwoCardsTest.*SameBits*`, CPU thread-count tests |
| New estimator, variance reduction, other RNG | equal **within Monte-Carlo error** | z-score `|a − b| / sqrt(se_a² + se_b²) < 3–4`; CI coverage over seeds (`qmharness` statistics family) |
| Single precision or approximations | only with a measured error budget and the maintainer's agreement | error vs the double result over the product grid |

A tolerance is stated in the test with its reason; never widen one to make a
test pass.

## Sums

- Monte-Carlo means over millions of paths: the logical-block tree (pairwise in
  practice) keeps the rounding error at O(log N) ulps; do not replace it with a
  running sum.
- Welford (`utils/accumulators.hpp`) for mean and variance in one pass;
  standard error always reported with the estimate.

## Speed claims

- A speed-up is quoted with: workload (paths, dates, trades), device(s),
  before → after times, and the standard error reached. A Monte-Carlo speed-up
  without its error means nothing (`qm_gpu_bench` measures *time to error*).
- Warm-up excluded (first CUDA call initializes the context).
- On a shared machine, repeat and report the median; note GPU memory used by
  other processes.

## Greeks

- Optimizing a valuation path must keep the AAD / pathwise greeks consistent:
  run the greeks tests (`qmharness` greeks family: pathwise vs LRM vs finite
  differences vs analytic).
