#ifndef QM_INSTRUMENTS_EQUITY_SIMULATABLE_ASIAN_HPP
#define QM_INSTRUMENTS_EQUITY_SIMULATABLE_ASIAN_HPP

#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Arithmetic- or geometric-average Asian call/put for the timeline
     *        Monte-Carlo engine.
     *
     * Fixing times are year-fractions from the valuation date — the caller
     * resolves them from calendar Dates through a ValuationContext, so the
     * calendar / day-count layer is exercised end to end. Settlement is the
     * last fixing; the payoff is deflated by that date's numeraire.
     */
    template <class T = Real>
    class SimulatableAsian final : public ISimulatableProduct<T>
    {
      public:
        SimulatableAsian(std::vector<Time> fixing_times, Real strike, bool is_call,
                         bool geometric)
            : strike_(strike), is_call_(is_call), geometric_(geometric)
        {
            if (fixing_times.empty())
                throw std::invalid_argument("SimulatableAsian: need at least one fixing");
            timeline_ = canonical_timeline(std::move(fixing_times));
            if (timeline_.front() <= 0.0)
                throw std::invalid_argument(
                    "SimulatableAsian: every fixing must fall after the valuation date");

            defline_.resize(timeline_.size());
            defline_.back().numeraire = true;
        }

        const TimeLine &timeline() const override { return timeline_; }
        const std::vector<SampleDef> &defline() const override { return defline_; }
        const std::vector<std::string> &payoff_labels() const override
        {
            return labels_;
        }
        std::size_t n_underlyings() const override { return 1; }

        void payoffs(const Scenario<T> &path, std::vector<T> &out) const override
        {
            const T n = static_cast<T>(path.size());
            T avg;
            if (geometric_)
            {
                T acc(0);
                for (const Sample<T> &s : path)
                    acc += std::log(s.spots[0]);
                avg = std::exp(acc / n);
            }
            else
            {
                T acc(0);
                for (const Sample<T> &s : path)
                    acc += s.spots[0];
                avg = acc / n;
            }

            const T k = T(strike_);
            const T intrinsic = is_call_ ? std::max(avg - k, T(0))
                                         : std::max(k - avg, T(0));
            out.assign(1, intrinsic / path.back().numeraire);
        }

      private:
        Real strike_;
        bool is_call_;
        bool geometric_;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<std::string> labels_{"price"};
    };

} // namespace quantModeling

#endif // QM_INSTRUMENTS_EQUITY_SIMULATABLE_ASIAN_HPP
