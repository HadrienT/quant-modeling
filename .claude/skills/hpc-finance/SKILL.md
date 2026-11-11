---
name: hpc-finance
description: Use when writing, reviewing or optimizing performance-critical C++ or CUDA in quant-modeling (Monte-Carlo kernels, path generation, exposure/xVA engines, AAD, GPU backend in src/gpu/), or when the user asks for faster, optimized or "desk-grade" numerical code. Enforces measure → change one thing → prove the numbers did not move → record the gain.
---

# High-performance C++ / CUDA for quant-modeling

Fast code that changes a price is a bug. Every optimization here is judged on
two numbers: the **measured** speed-up and the **proof** that the results did
not move. A change with only one of them is not done.

## Non-negotiable rules

1. **Reproducibility first.** A result must not depend on the number of
   threads, of GPUs, or on launch sizes. Keep the logical-block reduction tree
   (`engines/mc/logical_blocks.hpp`, `src/gpu/logical_blocks.cuh`): fixed tree
   inside a block, partials folded **in block order** on the host. Counter-based
   RNG (Philox: draw `j` of path `p` is a pure function of `(seed, p, j)`) or
   Sobol split by point index. Never `atomicAdd` on a double result.
2. **No `-ffast-math` / `--use_fast_math`**, no `__expf`-style intrinsics on
   pricing paths: they break NaN/Inf handling, reorder sums and change the bits.
   Single precision only with a measured error budget and the maintainer's
   agreement. The V100 runs FP64 at half its FP32 rate: `double` is the default.
3. **Host and device share the math.** Payoff and model code is `QM_HOST_DEVICE`
   and written once (`engines/xva/exposure_program.hpp`, `engines/mc/*`); the GPU
   is tested against the CPU. Never fork a "GPU version" of a formula.
4. **The desk split holds.** Templates and compile-time flags are welcome in hot
   loops, but no engine branches on a product type and no instrument reads
   market data, whatever the speed-up.
5. **The GPU is shared** with the scripting assistant's LLM (~13–14 GB of each
   16 GB V100 are usually taken). Size launches from `free_memory()` (at most
   half), give memory back (`StackLimitRestore`, RAII device arrays), and treat
   `out of memory` as an expected, user-facing error.
6. **Ownership:** RAII for every device and host buffer (`DeviceArray`-style or
   `std::unique_ptr` with a deleter); no raw `cudaMalloc` whose `cudaFree` an
   exception can skip.
7. **The best existing library before our own code.** Before writing or
   keeping a component, look for the reference library (cuRAND, CUB, NVRTC,
   cuBLAS, Eigen, SLEEF, Boost.Math…) and run the checks of
   [libraries.md](libraries.md): features, CPU-only build and CPU = GPU,
   reproducibility, **measured** speed on our workload, deployment. Adopt it
   when it passes; when it only wins on speed, adopt its algorithm. Record the
   decision in libraries.md's table.

## The loop (every optimization, no shortcut)

1. **Measure the baseline** on the real workload, and keep the output:
   - GPU: `build-cuda/qm_gpu_bench`, `qm_gpu_exposure_bench`, `qm_gpu_vr_bench`,
     `qm_gpu_risk_bench`, `qm_gpu_table_bench`, `qm_gpu_owen_bench`,
     `qm_gpu_rng_bench` (our generators against cuRAND).
   - CPU: `quantModeling_bench` (google-benchmark, `cmake --preset release`),
     `build/qm_xva_aad_bench`. The `cppdev` MCP's `cpp_bench` compares a run
     against a reference.
   - Note the GPU memory in use (`nvidia-smi`): the LLM changes what fits.
2. **Find the bottleneck with a profiler, not by reading code.**
   `ncu --kernel-name regex:<kernel> --launch-skip 4 --launch-count 1 --section
   SpeedOfLight --section Occupancy --section WarpStateStats --section
   SourceCounters <binary> <args>`. Classify: memory-bound, latency-bound (low
   memory *and* compute throughput, warps stalled), or compute-bound. On this
   server `nsys` cannot import its reports; use `ncu`. CPU: `perf stat` /
   `perf record`, `-fopt-info-vec-missed` for vectorization.
3. **Change one thing.** See [cuda-patterns.md](cuda-patterns.md) and
   [cpu-patterns.md](cpu-patterns.md) for what to try for each diagnosis.
4. **Prove nothing moved** — see [numerics.md](numerics.md) for tolerances:
   - same seed, same result: compare full-precision outputs before/after
     (the benchmarks print 17 digits); on the GPU expect **identical bits**
     unless the change reorders a floating-point sum on purpose;
   - `ctest --test-dir build-cuda -L gpu` and `ctest --test-dir build -j16`
     (or `scripts/make.sh`); `scripts/make_asan.sh` when memory handling changed;
   - `qmharness` MCP: `qm_run` before and after, then `qm_compare` (golden
     prices, cross-engine, invariants, convergence slope, greeks, determinism).
5. **Re-profile** to confirm the diagnosis changed as predicted (e.g. excessive
   sectors gone), then **record the gain** in the commit message and in a code
   comment near the change: workload, before → after, device, date.

GPU tests that fail with `out of memory` while the LLM is loaded are an
environment limit, not a regression: check the failing test does not depend on
the changed files (`ninja -t deps <object>`), and say so explicitly.

## What already works here (keep it)

- Deterministic logical-block reduction, bit-identical on one or two V100s.
- Warp reduction with `__shfl_down_sync`, `__launch_bounds__(256)`.
- Option flags (call/put, antithetic, importance sampling) as template
  parameters: no runtime branch inside the kernel (`src/gpu/vanilla_bs.cu`).
- Date-major per-path scratch through `Strided<T>` (`core/strided.hpp`) in
  `src/gpu/exposure.cu`: the netting kernel went from 161.6 ms to 59.1 ms a
  launch (1 M paths: 1.31 s → 0.52 s on one V100), identical CVA bits.

## Known open leads (measure before starting)

- `src/gpu/script.cu`: dual numbers in local memory (~13 KB per thread) —
  occupancy and local-memory traffic; the OOM failures of `GpuRisksTest` when
  the LLM is loaded come from the same stack.
- Exposure cube to the host (`cube_kernel`): outputs are still path-major for
  the host cube; synchronous pageable `cudaMemcpy`, no overlap with compute.
- `cudaMalloc`/`cudaFree` per request in every GPU entry point: a pool
  (`cudaMallocAsync`) or reused buffers for the API's repeated calls.
- `src/gpu/rng.cu`, `src/gpu/paths.cu`: raw `cudaMalloc` without RAII.
