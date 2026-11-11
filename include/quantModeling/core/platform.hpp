#ifndef CORE_PLATFORM_HPP
#define CORE_PLATFORM_HPP

// Portability macros so that Monte-Carlo path kernels can be compiled both
// by the host compiler (CPU engines) and by nvcc (future CUDA backend).
// On a pure CPU build these expand to nothing.

#if defined(__CUDACC__)
#define QM_HOST_DEVICE __host__ __device__
#else
#define QM_HOST_DEVICE
#endif

// A function the GPU calls rather than inlines: one copy of a large body
// (Φ⁻¹ with its erfc and exp) instead of one per call site, for kernels
// bound by the instruction cache (blueprint/wp/26-performance.md §5, P1h).
// The host build inlines as it likes.
#if defined(__CUDA_ARCH__)
#define QM_DEVICE_NOINLINE __noinline__
#else
#define QM_DEVICE_NOINLINE
#endif

#if defined(__GNUC__) || defined(__clang__)
#define QM_RESTRICT __restrict__
#else
#define QM_RESTRICT
#endif

// Cache-line size used for alignment of hot accumulator structs
// (avoids false sharing once MC loops are multithreaded).
#define QM_CACHELINE 64

#endif // CORE_PLATFORM_HPP
