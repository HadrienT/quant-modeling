#include "quantModeling/market/sabr_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{

    SABRSliceObjective::SABRSliceObjective(std::vector<SABRSliceQuote> quotes, Real forward, Real ttm, Real beta)
        : quotes_(std::move(quotes)), forward_(forward), ttm_(ttm), beta_(beta)
    {
        Real iv_max = 0.01;
        for (const auto &q : quotes_)
            iv_max = std::max(iv_max, q.market_iv);

        // alpha has units of vol * F^(1-beta): a generous multiple of the
        // largest observed IV (scaled to that convention) comfortably bounds
        // any real market smile without hand-tuning per ticker.
        const Real alpha_upper = 5.0 * iv_max * std::pow(forward_, 1.0 - beta_);

        lower_ = {1e-6, -0.999, 0.0};
        upper_ = {std::max(alpha_upper, Real(1e-3)), 0.999, 5.0};
    }

    std::vector<Real> SABRSliceObjective::residuals(const std::vector<Real> &params) const
    {
        const SABRParams p = unpack(params, beta_);
        std::vector<Real> r(quotes_.size());
        for (std::size_t i = 0; i < quotes_.size(); ++i)
            r[i] = sabr_implied_vol(forward_, quotes_[i].strike, ttm_, p) - quotes_[i].market_iv;
        return r;
    }

    std::vector<Real> SABRSliceObjective::weights() const
    {
        std::vector<Real> w(quotes_.size());
        for (std::size_t i = 0; i < quotes_.size(); ++i)
            w[i] = quotes_[i].weight;
        return w;
    }

    SABRParams SABRSliceObjective::unpack(const std::vector<Real> &params, Real beta) noexcept
    {
        SABRParams p;
        p.alpha = params[0];
        p.beta = beta;
        p.rho = params[1];
        p.nu = params[2];
        return p;
    }

    std::vector<Real> SABRSliceObjective::pack(const SABRParams &p)
    {
        return {p.alpha, p.rho, p.nu};
    }

    std::vector<Real> SABRSliceObjective::initial_guess() const
    {
        if (quotes_.empty())
            return {0.2 * std::pow(forward_, 1.0 - beta_), 0.0, 0.3};

        const SABRSliceQuote *closest = &quotes_.front();
        for (const auto &q : quotes_)
            if (std::fabs(q.strike - forward_) < std::fabs(closest->strike - forward_))
                closest = &q;

        // ATM implied vol ~ alpha / F^(1-beta) to leading order (the series
        // and time corrections are both 1 + O(small) there).
        const Real alpha0 = std::max(closest->market_iv * std::pow(forward_, 1.0 - beta_), Real(1e-4));
        constexpr Real rho0 = 0.0;
        constexpr Real nu0 = 0.3;

        return {alpha0, rho0, nu0};
    }

    std::vector<std::vector<Real>> SABRSliceObjective::initial_guess_candidates() const
    {
        const std::vector<Real> base = initial_guess();
        if (base.size() != 3)
            return {base};

        const Real alpha0 = base[0];

        std::vector<std::vector<Real>> candidates;
        for (const Real rho0 : {0.0, -0.5, 0.5, -0.8, 0.8})
            for (const Real nu0 : {0.1, 0.3, 0.8, 1.5})
                candidates.push_back({alpha0, rho0, nu0});
        return candidates;
    }

    SABRSliceCalibration calibrate_sabr_slice(
        std::vector<SABRSliceQuote> quotes, Real forward, Real ttm, Real beta,
        const calibration::LevenbergMarquardtSettings &settings)
    {
        SABRSliceCalibration result;
        result.ttm = ttm;

        if (quotes.empty())
        {
            result.params = SABRParams{};
            result.params.beta = beta;
            return result;
        }

        SABRSliceObjective objective(quotes, forward, ttm, beta);

        bool have_candidate = false;
        Real best_rmse = std::numeric_limits<Real>::infinity();

        for (const auto &start : objective.initial_guess_candidates())
        {
            calibration::CalibrationReport candidate_report =
                calibration::levenberg_marquardt(objective, start, settings);

            if (!have_candidate || candidate_report.rmse < best_rmse)
            {
                have_candidate = true;
                best_rmse = candidate_report.rmse;
                result.report = candidate_report;
                result.params = SABRSliceObjective::unpack(candidate_report.params, beta);
            }
        }
        return result;
    }

} // namespace quantModeling
