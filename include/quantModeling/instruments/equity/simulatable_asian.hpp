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
            // Unqualified on purpose (blueprint §5.5): with T = aad::Number,
            // argument-dependent lookup must find aad::log/exp/max instead of
            // the double-only std:: overloads, which do not compile for
            // Number at all (its conversion to double is explicit) -- the
            // safe failure mode of getting this wrong. For T = Real these
            // still resolve to std:: via the using-declarations.
            using std::exp;
            using std::log;
            using std::max;

            const double n = static_cast<double>(path.size());
            // The accumulator starts from the path's own first term rather
            // than a synthetic T(0): that would put a wasted leaf node on
            // the tape every path for an additive identity that is not a
            // real quantity.
            T avg = geometric_ ? log(path.front().spots[0]) : path.front().spots[0];
            for (std::size_t i = 1; i < path.size(); ++i)
                avg += geometric_ ? log(path[i].spots[0]) : path[i].spots[0];
            avg = geometric_ ? exp(avg / n) : avg / n;

            // Mixed (T minus double) rather than building a T(strike_) leaf
            // for the constant first -- see blueprint §5.2 on why a mixed
            // operation records one node instead of wasting one on a leaf.
            const T intrinsic = is_call_ ? max(avg - strike_, 0.0)
                                         : max(strike_ - avg, 0.0);
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
