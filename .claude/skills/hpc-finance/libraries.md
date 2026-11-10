# Established libraries: use the best one, or know why not

Most of this repository was written from scratch to learn. From now on, a
component that an established library does better is **not** rewritten: the
library is used, unless one of the checks below fails. Equally, a library is
not adopted on its reputation: "NVIDIA ships it" is a reason to measure, not
a measurement.

## Before writing (or keeping) a component

1. **Look for the reference library** (table below, then the CUDA toolkit,
   Boost, Eigen, the literature's own code). Say in the pull request which
   one exists and what was decided.
2. **Features.** Does it do what the engine needs? (Owen scrambling, inline
   per-path generation inside our kernels, AAD number types, our dimensions.)
3. **CPU parity and the CPU-only build.** The library must not make the
   CPU-only wheel and CI (no CUDA) impossible, and must not break CPU = GPU
   (bits where we promise bits, Monte-Carlo error elsewhere).
4. **Reproducibility.** Same seed, same bits whatever the number of threads or
   devices (logical blocks).
5. **Measured performance**, on the real workload (a fused kernel, not only a
   generation micro-benchmark), against our code.
6. **Deployment.** Is it in the production image at run time? (The runtime
   image has no CUDA toolkit: link statically or ship the library.) Licence.

If the library passes, adopt it. If it fails on features or parity but wins
on speed, **adopt its algorithm** in our code and cite it.

## Decisions so far

| Component | Library | Decision | Why |
|---|---|---|---|
| Sobol on the GPU | cuRAND (`CURAND_RNG_QUASI_*SOBOL*`) | **not adopted; its algorithm is** | Measured 2026-11-10 (`qm_gpu_rng_bench`): cuRAND ~820 GB/s, ours 249 GB/s (random access), 456–500 GB/s branchless with the directions in shared memory, same bits. cuRAND gets the rest from Gray-code stepping (one XOR from a point to the next). It has no Owen scrambling, its host API writes to memory where our kernels draw inline, its device API needs one state per dimension per thread, and the CPU-only build could not use it. To do: branchless `sobol_point_bits` in the kernels; Gray-code stepping where a thread walks consecutive points (the logical-block mapping gives a thread units t, t + 256, …: changing it changes the reduction order on CPU and GPU together) |
| Philox | cuRAND (`CURAND_RNG_PSEUDO_PHILOX4_32_10`) | not adopted | Ours 426–470 GB/s against cuRAND's 300 GB/s, both 64 bits a double; ours is validated against the Random123 test vectors and runs on the CPU bit for bit |
| Runtime compilation of scripts for the GPU | NVRTC | **adopted** (WP 26 P1b, approved 2026-11-10) | Removes the interpreter's dispatch and register pressure; link `libnvrtc_static` (runtime image has no toolkit) |
| CPU linear algebra | Eigen | in use | `find_package(Eigen3)`, correlation factors, multi-asset models |
| Unit tests, micro-benchmarks | GoogleTest, google-benchmark | in use | |

## Candidates to evaluate when the need comes

| Need | Library | Check first |
|---|---|---|
| Vectorised `exp`, `log`, `erfc` for the batched CPU interpreter (WP 26 P1a) | SLEEF, glibc `libmvec` | bits differ from scalar libm by an ulp: a tolerance decision with the maintainer, or keep scalar calls; `libmvec` usually needs `-ffast-math`, which is banned |
| Block and warp primitives (scan, reduce, sort) on the GPU | CUB (in the CUDA toolkit) | deterministic for a fixed launch configuration, but its tree is not our logical-block tree: fine for non-pricing work, not for estimators that must equal the CPU fold |
| Batched least squares (Longstaff-Schwartz on the GPU) | cuBLAS / cuSOLVER batched QR | determinism of the batched routines, small sizes (basis of 3 to 10 functions) |
| Accuracy reference for special functions (Φ⁻¹, erfc) in tests | Boost.Math | tests only, never in kernels |
| Low-discrepancy alternatives (lattice rules, higher-order nets) for MLQMC | QMCPy, Kuo's lattice generating vectors | WP 26 P5: Giles & Waterhouse used a rank-1 lattice |

The server's CPUs are Intel Xeon E5-2680 v4 (Broadwell): **AVX2, no
AVX-512**. Vector widths, library builds and `-march=native` results follow
from that.
