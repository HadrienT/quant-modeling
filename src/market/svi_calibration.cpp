#include "quantModeling/market/svi_calibration.hpp"

#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{

    namespace
    {

        /// Black-Scholes vega under the forward measure: F * phi(d1) * sqrt(T).
        /// The discount factor is dropped -- it is a common scale factor across
        /// every quote in one slice (same T), so it cannot change how they are
        /// weighted relative to each other.
        Real bs_vega_forward(Real forward, Real strike, Real ttm, Real sigma) noexcept
        {
            if (sigma <= 0.0 || ttm <= 0.0 || forward <= 0.0 || strike <= 0.0)
                return 0.0;
            const Real vol_sqrt_t = sigma * std::sqrt(ttm);
            const Real d1 = (std::log(forward / strike) + 0.5 * sigma * sigma * ttm) / vol_sqrt_t;
            return forward * norm_pdf(d1) * std::sqrt(ttm);
        }

    } // namespace

    SVISliceObjective::SVISliceObjective(std::vector<SVISliceQuote> quotes, Real ttm)
        : quotes_(std::move(quotes)), ttm_(ttm)
    {
        Real k_min = 0.0, k_max = 0.0, w_max = 0.01;
        if (!quotes_.empty())
        {
            k_min = k_max = quotes_.front().log_moneyness;
            for (const auto &q : quotes_)
            {
                k_min = std::min(k_min, q.log_moneyness);
                k_max = std::max(k_max, q.log_moneyness);
                w_max = std::max(w_max, q.market_iv * q.market_iv * ttm_);
            }
        }
        const Real k_span = std::max(k_max - k_min, Real(0.1));

        // sigma's lower bound and b's upper bound both matter more than they
        // look: found against a real GOOGL chain, where 1e-4 and
        // 20*w_max/k_span (unbounded above) let several slices collapse to
        // sigma ~ 0.0001 with b in the 3-17 range and rho pinned at +-0.999
        // -- a genuine local optimum on sparse or narrow-k-range slices, not
        // a starting-point problem (multi-start converges to the same
        // degenerate point from every tested start). A sigma this far below
        // any real curvature scale, paired with a b this large, is not a
        // smile any real market produces; bounding both away from that
        // region is cheap insurance against it, not a guess.
        constexpr Real sigma_floor = 0.02;
        constexpr Real b_ceiling = 5.0;

        lower_ = {-10.0 * w_max, 0.0, -0.999, k_min - k_span, sigma_floor};
        upper_ = {10.0 * w_max, std::min(20.0 * w_max / k_span, b_ceiling), 0.999, k_max + k_span, 5.0 * k_span};
    }

    std::vector<Real> SVISliceObjective::residuals(const std::vector<Real> &params) const
    {
        const SVIParams p = unpack(params);
        std::vector<Real> r(quotes_.size());
        for (std::size_t i = 0; i < quotes_.size(); ++i)
            r[i] = svi_implied_vol(quotes_[i].log_moneyness, ttm_, p) - quotes_[i].market_iv;
        return r;
    }

    std::vector<Real> SVISliceObjective::weights() const
    {
        std::vector<Real> w(quotes_.size());
        for (std::size_t i = 0; i < quotes_.size(); ++i)
            w[i] = quotes_[i].weight;
        return w;
    }

    SVIParams SVISliceObjective::unpack(const std::vector<Real> &params) noexcept
    {
        SVIParams p;
        p.a = params[0];
        p.b = params[1];
        p.rho = params[2];
        p.m = params[3];
        p.sigma = params[4];
        return p;
    }

    std::vector<Real> SVISliceObjective::pack(const SVIParams &p)
    {
        return {p.a, p.b, p.rho, p.m, p.sigma};
    }

    std::vector<Real> SVISliceObjective::initial_guess() const
    {
        if (quotes_.empty())
            return {0.04, 0.1, 0.0, 0.0, 0.1};

        Real k_sum = 0.0;
        Real k_min = quotes_.front().log_moneyness, k_max = k_min;
        Real w_min = quotes_.front().market_iv * quotes_.front().market_iv * ttm_;
        for (const auto &q : quotes_)
        {
            k_sum += q.log_moneyness;
            k_min = std::min(k_min, q.log_moneyness);
            k_max = std::max(k_max, q.log_moneyness);
            w_min = std::min(w_min, q.market_iv * q.market_iv * ttm_);
        }

        const Real m0 = k_sum / static_cast<Real>(quotes_.size());
        const Real k_span = std::max(k_max - k_min, Real(0.1));
        const Real sigma0 = std::max(0.1 * k_span, Real(0.02)); // matches the constructor's sigma floor
        const Real b0 = std::max(0.1 * w_min / std::max(k_span, Real(1e-3)), Real(1e-3));
        constexpr Real rho0 = 0.0;
        // At rho = 0, the slice minimum is a + b*sigma -- pick a0 so that
        // minimum roughly matches the smallest observed total variance.
        const Real a0 = w_min - b0 * sigma0;

        return {a0, b0, rho0, m0, sigma0};
    }

    std::vector<std::vector<Real>> SVISliceObjective::initial_guess_candidates() const
    {
        const std::vector<Real> base = initial_guess();
        if (base.size() != 5)
            return {base};

        const Real a0 = base[0], b0 = base[1], m0 = base[3], sigma0 = base[4];

        std::vector<std::vector<Real>> candidates;
        for (const Real rho0 : {0.0, -0.6, 0.6, -0.3, 0.3})
            for (const Real scale : {1.0, 0.4, 2.5})
                candidates.push_back({a0, b0, rho0, m0, std::max(sigma0 * scale, Real(1e-4))});
        return candidates;
    }

    SVISliceCalibration calibrate_svi_slice(
        std::vector<SVISliceQuote> quotes, Real ttm,
        const calibration::LevenbergMarquardtSettings &settings)
    {
        SVISliceCalibration result;
        result.ttm = ttm;

        if (quotes.empty())
        {
            result.params = SVIParams{};
            result.butterfly_arbitrage_free = false;
            return result;
        }

        Real k_min = quotes.front().log_moneyness, k_max = k_min;
        for (const auto &q : quotes)
        {
            k_min = std::min(k_min, q.log_moneyness);
            k_max = std::max(k_max, q.log_moneyness);
        }

        SVISliceObjective objective(quotes, ttm);

        bool have_candidate = false;
        Real best_score = std::numeric_limits<Real>::infinity();

        for (const auto &start : objective.initial_guess_candidates())
        {
            calibration::CalibrationReport candidate_report =
                calibration::levenberg_marquardt(objective, start, settings);
            const SVIParams candidate_params = SVISliceObjective::unpack(candidate_report.params);
            const bool candidate_arb_free =
                svi_is_butterfly_arbitrage_free(candidate_params, k_min, k_max);

            // RMSE is the primary signal; a fit outside the arbitrage-free
            // region is penalised so a slightly worse but arbitrage-free
            // candidate wins over a marginally tighter one that isn't.
            const Real score = candidate_report.rmse + (candidate_arb_free ? 0.0 : 1.0);

            if (!have_candidate || score < best_score)
            {
                have_candidate = true;
                best_score = score;
                result.report = candidate_report;
                result.params = candidate_params;
                result.butterfly_arbitrage_free = candidate_arb_free;
            }
        }
        return result;
    }

    std::vector<SVISliceQuote> svi_quotes_from_raw_surface(
        const RawVolSurface &surface, Real ttm, Real ttm_tolerance)
    {
        std::vector<SVISliceQuote> out;
        const Real forward = surface.forward(ttm);

        for (const auto &q : surface.quotes())
        {
            if (!q.has_iv || std::abs(q.ttm - ttm) > ttm_tolerance)
                continue;

            SVISliceQuote sq;
            sq.log_moneyness = surface.log_moneyness(q.strike, ttm);
            sq.market_iv = q.implied_vol;
            // Call and put vega coincide under Black-Scholes at the same
            // (S, K, T, sigma), so is_call does not affect the weight.
            sq.weight = std::max(bs_vega_forward(forward, q.strike, ttm, q.implied_vol), Real(1e-8));
            out.push_back(sq);
        }
        return out;
    }

    bool svi_slices_are_calendar_arbitrage_free(
        const SVISliceCalibration &shorter, const SVISliceCalibration &longer,
        Real k_min, Real k_max, std::size_t n_grid)
    {
        if (!(shorter.ttm < longer.ttm))
            return false; // caller must pass the pair in maturity order
        if (n_grid < 2 || !(k_max > k_min))
            return false;

        for (std::size_t i = 0; i < n_grid; ++i)
        {
            const Real t = static_cast<Real>(i) / static_cast<Real>(n_grid - 1);
            const Real k = k_min + t * (k_max - k_min);
            const Real w_short = svi_total_variance(k, shorter.params);
            const Real w_long = svi_total_variance(k, longer.params);
            if (w_long < w_short)
                return false;
        }
        return true;
    }

} // namespace quantModeling
