#ifndef CORE_STRIDED_HPP
#define CORE_STRIDED_HPP

#include "quantModeling/core/platform.hpp"

#include <cstddef>

namespace quantModeling
{

    /**
     * @brief A row of values spaced `stride` elements apart, read and written
     *        like an array: `row[i]` is `base[i * stride]`.
     *
     * The device kernels store one path per thread in "date-major" order,
     * value of date i of path p at `i * paths + p`: the 32 threads of a warp,
     * which run 32 consecutive paths, then touch 32 consecutive doubles at
     * every date, one memory transaction where the "path-major" order (date i
     * of path p at `p * dates + i`) takes up to 32. The functions shared with
     * the host take any indexable row, so the host keeps passing plain
     * pointers and the device passes this view.
     */
    template <class T>
    struct Strided
    {
        T *base = nullptr;
        std::size_t stride = 1;

        QM_HOST_DEVICE T &operator[](int i) const { return base[static_cast<std::size_t>(i) * stride]; }
    };

} // namespace quantModeling

#endif // CORE_STRIDED_HPP
