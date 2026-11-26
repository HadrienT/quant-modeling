#ifndef MARKET_RAW_VOL_SURFACE_HPP
#define MARKET_RAW_VOL_SURFACE_HPP

#include "quantModeling/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace quantModeling
{

    /**
     * One option quote as reported by the data source — the C++ counterpart of
     * a row in data-ingest's options.chain_snapshot table. No derived fields:
     * mid() is computed on demand, never stored, so there is exactly one place
     * that decides how a mid is built from bid/ask/last.
     */
    struct RawOptionQuote
    {
        Real strike = 0.0;
        Real ttm = 0.0; ///< time to maturity in years, > 0
        bool is_call = true;
        Real bid = 0.0;
        Real ask = 0.0;
        Real last = 0.0;
        std::int64_t volume = 0;
        std::int64_t open_interest = 0;
        Real implied_vol = 0.0; ///< meaningless unless has_iv is true
        bool has_iv = false;    ///< false when the source had no usable implied vol
        /// The expiry's forward and discount factor as data-ingest stored them
        /// with the quote (options.chain_iv, WP 31): from parity on a European
        /// chain, from carry on an American one. 0 when the source has none;
        /// the surface then uses S exp((r - q) T) and exp(-r T).
        Real forward = 0.0;
        Real discount = 0.0;

        /// (bid+ask)/2 when both quoted; falls back to the last trade otherwise.
        Real mid() const noexcept;
    };

    struct CleaningStats
    {
        std::size_t raw_count = 0;
        std::size_t after_liquidity = 0;
        std::size_t after_moneyness = 0;
        std::size_t after_calendar_arbitrage = 0;
        std::size_t after_butterfly_arbitrage = 0;
        std::size_t final_count = 0;
    };

    struct CleaningParams
    {
        std::int64_t min_open_interest = 10;
        Real min_bid = 0.05;
        Real max_spread_ratio = 0.50; ///< (ask - bid) / mid must not exceed this
        Real min_moneyness = 0.70;    ///< strike / spot lower bound
        Real max_moneyness = 1.40;    ///< strike / spot upper bound
        /// Below this fraction of quotes reporting open_interest > 0, treat OI
        /// as unpopulated and filter on volume instead (yfinance sometimes
        /// reports open interest as all zero for an entire chain).
        Real oi_coverage_threshold = 0.05;
        /// A quoted implied vol outside [min_plausible_iv, max_plausible_iv]
        /// is rejected outright, liquidity notwithstanding. Real quotes this
        /// far out are rare enough that generous bounds (2%-300%) never
        /// touch a legitimate equity smile; they exist because yfinance's
        /// impliedVolatility is sometimes backed out from a stale last-trade
        /// price rather than a live bid/ask, and a near-expiry contract that
        /// hasn't traded in a while can report something like 450% or
        /// 0.001% -- found by testing against a real AAPL chain, not
        /// hypothesised: high open interest does not protect against this,
        /// so it is checked separately from the liquidity filter.
        Real min_plausible_iv = 0.02;
        Real max_plausible_iv = 3.00;
        /// The quotes were cleaned at ingestion (data-ingest's options.chain_iv,
        /// WP 31 §6.1): our own implied vols, OTM side only, every quote with
        /// its expiry's forward and discount, data-quality flags already
        /// applied by the reader. What is left here is what is not data
        /// quality:
        /// - liquidity: no open-interest, volume or min_bid filter (the reader
        ///   applied the two-tick floor and the zero bids); the IV range and
        ///   max_spread_ratio still hold;
        /// - moneyness: |ln(K / F)| <= max_abs_log_moneyness instead of K / S;
        /// - calendar: no per-quote check (it holds at fixed forward
        ///   moneyness, and the fitted slices are checked for it);
        /// - butterfly: on the European call equivalents of every quote (calls
        ///   and puts), a violation counted only beyond the three quotes'
        ///   half-spreads, with unequal strike spacing.
        /// The slice fit then weights each quote by its spread
        /// (svi_quotes_from_raw_surface). Every quote must carry forward and
        /// discount.
        bool ingestion_cleaned = false;
        Real max_abs_log_moneyness = 0.6;
    };

    /**
     * Cleaned option quotes in (log-moneyness, TTM) space, built from raw
     * quotes by a four-stage filter — liquidity, moneyness, calendar
     * arbitrage, butterfly arbitrage — ported from the project's previous
     * Python pipeline (api/app/local_vol/{fetcher,cleaner}.py). No smoothing
     * or interpolation happens here: this stage only removes quotes that
     * would contaminate a smile fit, it never adjusts one. SVI calibration is
     * the next stage that consumes RawVolSurface::quotes().
     */
    class RawVolSurface
    {
      public:
        RawVolSurface(std::vector<RawOptionQuote> raw_quotes,
                      Real spot, Real rate, Real dividend,
                      const CleaningParams &params = {});

        /// Quotes surviving all four cleaning stages.
        const std::vector<RawOptionQuote> &quotes() const noexcept { return quotes_; }
        const CleaningStats &stats() const noexcept { return stats_; }

        Real spot() const noexcept { return spot_; }
        Real rate() const noexcept { return rate_; }
        Real dividend() const noexcept { return dividend_; }
        bool ingestion_cleaned() const noexcept { return ingestion_cleaned_; }

        /// The forward of the maturity: the one its quotes carry when they
        /// carry one (RawOptionQuote::forward, kept even if every quote of the
        /// maturity was cleaned out), else F_T = S * exp((r - q) * T).
        Real forward(Real ttm) const noexcept;

        /// The discount factor, by the same rule: stored, else exp(-r * T).
        Real discount(Real ttm) const noexcept;

        /// Log-moneyness y = ln(K / F_T), the coordinate SVI fits in.
        Real log_moneyness(Real strike, Real ttm) const noexcept;

      private:
        std::vector<RawOptionQuote> quotes_;
        CleaningStats stats_;
        Real spot_;
        Real rate_;
        Real dividend_;
        bool ingestion_cleaned_ = false;
        /// ttm -> (forward, discount), from the raw quotes that carry them.
        std::map<Real, std::pair<Real, Real>> stored_;
    };

} // namespace quantModeling

#endif
