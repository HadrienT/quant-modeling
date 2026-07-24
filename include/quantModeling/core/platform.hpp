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

#if defined(__GNUC__) || defined(__clang__)
#define QM_RESTRICT __restrict__
#else
#define QM_RESTRICT
#endif

// Cache-line size used for alignment of hot accumulator structs
// (avoids false sharing once MC loops are multithreaded).
#define QM_CACHELINE 64

#endif // CORE_PLATFORM_HPP
