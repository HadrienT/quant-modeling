#ifndef UTILS_BROWNIAN_BRIDGE_HPP
#define UTILS_BROWNIAN_BRIDGE_HPP

#include <cmath>
#include <span>
#include <vector>

#include "quantModeling/core/types.hpp"

namespace quantModeling
{

  /**
   * @brief Brownian-bridge path construction (Jäckel's algorithm).
   *
   * Maps n i.i.d. N(0,1) variates z[0..n-1] to a discretely sampled
   * standard Brownian path W(t_1)..W(t_n). The first variate drives the
   * *terminal* value W(t_n); subsequent variates bisect the remaining
   * intervals, each conditioning on its already-fixed neighbours.
   *
   * Purpose (QMC): with Sobol points, the best-distributed leading
   * coordinates carry most of the path's variance (terminal value, then
   * midpoint, ...) — this reduces the effective dimension and restores
   * near-O(1/N) convergence for path-dependent payoffs. It also makes the
   * first coordinate the natural target for stratification.
   *
   * The linear map is precomputed once (indices + weights); transform()
   * is allocation-free and O(n) per path.
   */
  class BrownianBridge
  {
  public:
    /// @param times strictly increasing observation times, all > 0.
    explicit BrownianBridge(std::span<const Time> times)
        : n_(static_cast<int>(times.size())),
          t_(times.begin(), times.end()),
          bridge_index_(times.size()), left_index_(times.size()),
          right_index_(times.size()), left_weight_(times.size()),
          right_weight_(times.size()), std_dev_(times.size())
    {
      if (n_ == 0)
        throw InvalidInput("BrownianBridge: empty time grid");
      for (int i = 0; i < n_; ++i)
        if (t_[static_cast<size_t>(i)] <= (i == 0 ? 0.0 : t_[static_cast<size_t>(i) - 1]))
          throw InvalidInput("BrownianBridge: times must be strictly increasing and > 0");

      std::vector<int> map(static_cast<size_t>(n_), 0);
      // First variate: terminal point.
      map[static_cast<size_t>(n_) - 1] = 1;
      bridge_index_[0] = n_ - 1;
      std_dev_[0] = std::sqrt(t_[static_cast<size_t>(n_) - 1]);
      left_weight_[0] = right_weight_[0] = 0.0;
      left_index_[0] = right_index_[0] = 0;

      int j = 0;
      for (int i = 1; i < n_; ++i)
      {
        while (map[static_cast<size_t>(j)] != 0)
          ++j;
        int k = j;
        while (map[static_cast<size_t>(k)] == 0)
          ++k;
        const int l = j + ((k - 1 - j) >> 1);
        map[static_cast<size_t>(l)] = i;

        bridge_index_[static_cast<size_t>(i)] = l;
        left_index_[static_cast<size_t>(i)] = j;
        right_index_[static_cast<size_t>(i)] = k;

        const Real tl = t_[static_cast<size_t>(l)];
        const Real tk = t_[static_cast<size_t>(k)];
        const Real tj = (j != 0) ? t_[static_cast<size_t>(j) - 1] : 0.0;
        left_weight_[static_cast<size_t>(i)] = (tk - tl) / (tk - tj);
        right_weight_[static_cast<size_t>(i)] = (tl - tj) / (tk - tj);
        std_dev_[static_cast<size_t>(i)] =
            std::sqrt((tl - tj) * (tk - tl) / (tk - tj));

        j = k + 1;
        if (j >= n_)
          j = 0;
      }
    }

    int size() const { return n_; }

    /**
     * @brief Build W(t_1)..W(t_n) from n independent N(0,1) variates.
     *
     * z[0] must be the "best" coordinate of the point (first Sobol
     * dimension). May be called in-place (w == z) is NOT supported;
     * pass distinct buffers.
     */
    void transform(std::span<const Real> z, std::span<Real> w) const
    {
      w[static_cast<size_t>(n_) - 1] = std_dev_[0] * z[0];
      for (int i = 1; i < n_; ++i)
      {
        const int j = left_index_[static_cast<size_t>(i)];
        const int k = right_index_[static_cast<size_t>(i)];
        const int l = bridge_index_[static_cast<size_t>(i)];
        const Real wj = (j != 0) ? w[static_cast<size_t>(j) - 1] : 0.0;
        w[static_cast<size_t>(l)] =
            left_weight_[static_cast<size_t>(i)] * wj +
            right_weight_[static_cast<size_t>(i)] * w[static_cast<size_t>(k)] +
            std_dev_[static_cast<size_t>(i)] * z[static_cast<size_t>(i)];
      }
    }

  private:
    int n_;
    std::vector<Real> t_;
    std::vector<int> bridge_index_, left_index_, right_index_;
    std::vector<Real> left_weight_, right_weight_, std_dev_;
  };

} // namespace quantModeling

#endif // UTILS_BROWNIAN_BRIDGE_HPP
