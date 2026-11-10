# CPU patterns (C++20, gcc 14, Ninja)

## Build flags

- `cmake --preset release` for measurements: Release, `-march=native`
  (`QM_NATIVE_ARCH`), google-benchmark. The `default` preset (CI) has no
  `-march=native`: a speed-up that only exists with it must say so.
- Never `-ffast-math`, `-Ofast`, `-funsafe-math-optimizations` on the library.
- Vectorization report: add `-fopt-info-vec-missed` to one translation unit
  locally; do not commit it.

## Hot loops

- **Structure of arrays** for per-path data (one `std::vector<double>` per
  quantity, or one flat buffer indexed `quantity * paths + p`), so that the
  compiler vectorizes across paths.
- **No virtual call, allocation, `std::function` or exception inside the
  per-path or per-step loop.** Resolve the polymorphism once outside (the
  registry / adapters), then run a template instantiation or a flat program
  (`ExposureProgram` is the model: virtual `compile()` once, flat evaluation).
- Preallocate scratch per thread (workspace structs reused across paths);
  `reserve` before `push_back`.
- Hoist invariants: `exp(-r dt)`, `sqrt(dt)`, `sigma * sqrt(dt)` per step, not
  per path.
- `const T* QM_RESTRICT` on non-aliasing input/output arrays of hot kernels.
- Prefer `double` math functions without errno side effects; the code already
  builds with the defaults CI uses, keep it that way.

## Threads

- `utils/thread_pool.hpp`: the caller is the last worker. Parallelize over
  logical blocks of paths, never over a shared accumulator: same partials,
  same fold order as the GPU (`engines/mc/logical_blocks.hpp`) so that the CPU
  result does not depend on the thread count.
- Align per-thread accumulators to `QM_CACHELINE` (false sharing).
- The server has 16 usable threads for tests and benchmarks and shares its RAM
  with other services; measure with the thread count production uses.

## AAD

- Tape-based AAD (`aad/`) for CPU risks; per-path adjoint without a tape and
  dual numbers on the GPU (`etc/bytecode-aad-gpu-ordre-2.md`). Cost target:
  risks for a small multiple of one valuation (`qm_xva_aad_bench` measures it).
- Shared templates over the number type `T` (double, dual, tape number): an
  optimization in the double path must keep compiling and giving the same
  numbers for the other types.

## Measuring

- google-benchmark: `--benchmark_repetitions=10 --benchmark_report_aggregates_only=true`,
  report the median and its spread, pin nothing else heavy on the machine.
- `cppdev` MCP `cpp_bench` compares against a stored reference run.
- `perf stat -e cycles,instructions,cache-misses <binary>` for a first
  diagnosis, `perf record -g` for the hot functions.
