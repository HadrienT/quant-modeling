#ifndef UTILS_SOBOL_HPP
#define UTILS_SOBOL_HPP

#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "quantModeling/core/types.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/sobol_directions.hpp"

namespace quantModeling
{

  /**
   * @brief Scrambled Sobol low-discrepancy sequence (up to 1024 dimensions).
   *
   * - Direction numbers: Joe-Kuo "new-joe-kuo-6" (good 2-D projections).
   * - Gray-code generation: one XOR per dimension per point.
   * - Randomization: digital shift (per-dimension XOR mask drawn from a
   *   PCG32 stream). Running B independent shifts gives unbiased RQMC
   *   error bars: each shifted-sequence mean is one i.i.d. sample.
   *
   * One point = one Monte-Carlo path; the point's coordinates drive the
   * path's factors (e.g. one per time step, ordered by a Brownian bridge).
   */
  class SobolSequence
  {
  public:
    /**
     * @param dimension  coordinates per point (1..kMaxDimension)
     * @param scramble_seed seed of the digital-shift masks. Two sequences
     *        with different seeds are independent RQMC replicates.
     */
    explicit SobolSequence(int dimension, uint64_t scramble_seed = 0)
        : dim_(dimension), v_(static_cast<size_t>(dimension) * kBits),
          x_(static_cast<size_t>(dimension), 0u),
          shift_(static_cast<size_t>(dimension), 0u)
    {
      if (dimension < 1 || dimension > sobol_detail::kMaxDimension)
        throw InvalidInput("SobolSequence: dimension out of range [1, " +
                           std::to_string(sobol_detail::kMaxDimension) + "]");

      for (int d = 0; d < dim_; ++d)
        init_directions(d, &v_[static_cast<size_t>(d) * kBits]);

      Pcg32 rng(scramble_seed, /*stream_id=*/0x50b01u);
      for (int d = 0; d < dim_; ++d)
        shift_[static_cast<size_t>(d)] = rng();
    }

    int dimension() const { return dim_; }

    /**
     * @brief Write the next point's coordinates (in (0,1)) into out.
     *
     * The digital shift is always applied; the raw first point (0,...,0)
     * therefore maps to the shift itself. Coordinates are offset by half
     * an ulp of the 32-bit grid so they never hit exactly 0 or 1 (safe
     * for the inverse normal CDF).
     */
    void next_uniform(std::span<double> out)
    {
      constexpr double inv = 1.0 / 4294967296.0; // 2^-32
      if (index_ > 0)
      {
        const int c = std::countr_zero(index_);
        for (int d = 0; d < dim_; ++d)
          x_[static_cast<size_t>(d)] ^= v_[static_cast<size_t>(d) * kBits + static_cast<size_t>(c)];
      }
      ++index_;
      for (int d = 0; d < dim_; ++d)
      {
        const uint32_t u = x_[static_cast<size_t>(d)] ^ shift_[static_cast<size_t>(d)];
        out[static_cast<size_t>(d)] = (static_cast<double>(u) + 0.5) * inv;
      }
    }

    /// Next point mapped through the inverse normal CDF (i.i.d. N(0,1)
    /// marginals per coordinate).
    void next_gaussian(std::span<double> out)
    {
      next_uniform(out);
      for (int d = 0; d < dim_; ++d)
        out[static_cast<size_t>(d)] = inverse_normal_cdf(out[static_cast<size_t>(d)]);
    }

  private:
    static constexpr int kBits = 32;

    /// Expand the direction integers V_1..V_32 of dimension index d (0-based).
    static void init_directions(int d, uint32_t *QM_RESTRICT V)
    {
      if (d == 0)
      {
        // First dimension: van der Corput in base 2.
        for (int k = 0; k < kBits; ++k)
          V[k] = 1u << (31 - k);
        return;
      }
      using namespace sobol_detail;
      const int s = kS[d - 1];
      const uint32_t a = kA[d - 1];
      const uint32_t *m = &kM[kMOffset[d - 1]];

      for (int k = 0; k < s && k < kBits; ++k)
        V[k] = m[k] << (31 - k);
      for (int k = s; k < kBits; ++k)
      {
        V[k] = V[k - s] ^ (V[k - s] >> s);
        for (int j = 1; j < s; ++j)
          if ((a >> (s - 1 - j)) & 1u)
            V[k] ^= V[k - j];
      }
    }

    int dim_;
    std::vector<uint32_t> v_;     // direction integers, v_[d * 32 + k]
    std::vector<uint32_t> x_;     // current Gray-code state per dimension
    std::vector<uint32_t> shift_; // digital-shift masks
    uint32_t index_ = 0;          // points generated so far
  };

  /**
   * @brief GaussianSource adapter over a Sobol sequence.
   *
   * Feeds MC kernels one coordinate at a time: a path consuming `dim`
   * gaussians reads the `dim` coordinates of one Sobol point in order.
   * The caller must size `dimension` to the exact number of draws per
   * path; kernels templated on GaussianSource then work unchanged.
   */
  class SobolGaussianSource
  {
  public:
    SobolGaussianSource(int dimension, uint64_t scramble_seed)
        : seq_(dimension, scramble_seed),
          buf_(static_cast<size_t>(dimension)),
          pos_(dimension) // force a refill on first call
    {
    }

    double next()
    {
      if (pos_ == seq_.dimension())
      {
        seq_.next_gaussian(buf_);
        pos_ = 0;
      }
      return buf_[static_cast<size_t>(pos_++)];
    }

  private:
    SobolSequence seq_;
    std::vector<double> buf_;
    int pos_;
  };

} // namespace quantModeling

#endif // UTILS_SOBOL_HPP
