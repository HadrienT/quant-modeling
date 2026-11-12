#ifndef QM_PRICERS_SCRIPT_BUILDERS_HPP
#define QM_PRICERS_SCRIPT_BUILDERS_HPP

#include "quantModeling/core/date.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/equity/barrier.hpp"

#include <string>
#include <vector>

/**
 * @file script_builders.hpp
 * @brief Catalogue products written as scripts (WP 26 P3,
 *        blueprint/wp/26-performance.md §7): the terms of a product in, the
 *        script the generic engine prices out. One builder per product
 *        family; the adapters (pricers/adapters/) pair it with a model and
 *        price it with price_by_script().
 *
 * Dates count from the route's valuation date in calendar days (ACT/365F,
 * the scripts' day count): `days` is the maturity, as days_for() gives it.
 * Numbers are written with 17 significant digits, so the script prices the
 * terms it was given to the bit.
 */
namespace quantModeling::script_builders
{

    /// The valuation date every route's dates count from. Any date works:
    /// the scripts only see differences of dates.
    Date valuation();

    /// A number as the script parser reads it back to the bit.
    std::string number(double x);

    struct BarrierTerms
    {
        bool is_call = true;
        Real strike = 0.0;
        BarrierType type = BarrierType::DownAndOut;
        Real level = 0.0;
        Real rebate = 0.0; ///< paid at expiry in the "dead" state (knocked out, or never knocked in)
    };

    /**
     * Knock-in / knock-out vanilla. `observations` = 0: continuous
     * monitoring, the barrier read through survivalAbove / survivalBelow
     * (WP 26 P2) -- exact under Black-Scholes, per model step under local
     * vol. `observations` > 0: the spot checked on that many evenly spaced
     * dates up to the expiry (the hand-written engine's discrete mode).
     */
    std::string barrier(const BarrierTerms &t, int days, int observations = 0);

    /// `observations` calendar days evenly spread over (0, days], the last
    /// on `days`, one per day at most: the fixing dates of the routes whose
    /// hand-written engines used n evenly spaced steps.
    std::vector<int> evenly(int days, int observations);

    struct AsianTerms
    {
        bool is_call = true;
        Real strike = 0.0;
        bool geometric = false;
        /// Arithmetic only: declare the geometric Asian of the same fixings
        /// as an analytic control (controlPays, WP 26 P2).
        bool geometric_control = false;
    };

    /// Average-price Asian on `fixings` (days from the valuation date).
    std::string asian(const AsianTerms &t, const std::vector<int> &fixings);

    /// The geometric Asian on fixings at year fractions `times` under
    /// Black-Scholes: ln G is normal with mean ln S0 + (r - q - σ²/2)
    /// mean(t) and variance σ²/n² Σ_ij min(t_i, t_j) (Kemna & Vorst 1990,
    /// for any dates); paid at the last fixing.
    double geometric_asian_bs(double spot, double rate, double dividend, double vol, double strike, bool is_call,
                              const std::vector<double> &times);

    struct LookbackTerms
    {
        bool is_call = true;
        Real strike = 0.0;
        bool floating = false;  ///< floating strike: call S_T - min, put max - S_T
        bool on_maximum = true; ///< fixed strike: the extremum the payoff reads
    };

    /// Lookback on `fixings`; the extremum starts at the initial spot `s0`
    /// (the hand-written engines include it), written as a number.
    std::string lookback(const LookbackTerms &t, Real s0, const std::vector<int> &fixings);

    /// Worst-of (best-of) on n assets at expiry: notional * max(perf - K, 0)
    /// for a call, max(K - perf, 0) for a put, perf the lowest (highest) of
    /// spot(i) / s0[i].
    std::string rainbow(bool worst, bool is_call, Real strike, Real notional, const std::vector<Real> &s0, int days);

    /// Basket option at expiry: max(sum_i w_i spot(i) - K, 0) (call).
    std::string basket(bool is_call, Real strike, const std::vector<Real> &weights, int days);

    /// Himalaya (mountain): on each date (one per asset) the best return
    /// spot(j) / s0[j] - 1 among the assets still in the basket is locked in
    /// and the asset removed (the first one on a tie); at the last date,
    /// notional * max(average locked return - K, 0) (call).
    std::string mountain(bool is_call, Real strike, Real notional, const std::vector<Real> &s0,
                         const std::vector<int> &dates);

    struct AutocallTerms
    {
        Real s0 = 0.0;
        Real autocall_barrier = 1.0; ///< fractions of s0
        Real coupon_barrier = 0.0;
        Real put_barrier = 0.0;
        Real coupon_rate = 0.0; ///< per period, a fraction of the notional
        Real notional = 1000.0;
        bool memory_coupon = true;
        /// The knock-in checked on every observation date; otherwise on the
        /// last one only (the hand-written engine's two modes).
        bool ki_on_every_date = false;
    };

    /// Phoenix autocall on observation `dates`: called at par plus coupon(s)
    /// on the first date at or above the autocall barrier; a coupon (with
    /// the missed ones, memory) on a date at or above the coupon barrier;
    /// at the last date, par, or notional * spot() / s0 after a knock-in.
    std::string autocall(const AutocallTerms &t, const std::vector<int> &dates);

} // namespace quantModeling::script_builders

#endif // QM_PRICERS_SCRIPT_BUILDERS_HPP
