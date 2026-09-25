#include "quantModeling/market/heston_calibration.hpp"

#include "quantModeling/models/equity/sabr.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <map>

namespace quantModeling
{

    namespace
    {
        /// A vega this small (relative to the forward) means the quote says
        /// nothing about volatility; the floor keeps its residual finite.
        constexpr Real kRelativeVegaFloor = 1e-8;
    } // namespace

    HestonSurfaceObjective::HestonSurfaceObjective(std::vector<HestonCalibrationQuote> quotes,
                                                   Real spot, Real rate, Real dividend,
                                                   const HestonCOSSettings &cos)
        : quotes_(std::move(quotes)), n_quotes_(quotes_.size()), cos_(cos)
    {
        if (quotes_.empty())
            throw InvalidInput("calibrate_heston: no quotes to fit");
        if (!(spot > 0.0))
            throw InvalidInput("calibrate_heston: spot must be > 0");

        std::map<Real, std::size_t> slice_of_ttm;
        for (std::size_t i = 0; i < quotes_.size(); ++i)
        {
            const HestonCalibrationQuote &q = quotes_[i];
            if (!(q.strike > 0.0) || !(q.ttm > 0.0) || !(q.implied_vol > 0.0))
                throw InvalidInput("calibrate_heston: every quote needs strike, ttm and implied_vol > 0");
            auto [it, inserted] = slice_of_ttm.emplace(q.ttm, slices_.size());
            if (inserted)
                slices_.push_back(Slice{q.ttm, spot * std::exp((rate - dividend) * q.ttm), std::exp(-rate * q.ttm), {}, {}});
            Slice &s = slices_[it->second];
            s.index.push_back(i);
            s.strikes.push_back(q.strike);
        }

        market_price_.resize(n_quotes_);
        market_vega_.resize(n_quotes_);
        is_call_.resize(n_quotes_);
        for (const Slice &s : slices_)
            for (std::size_t j = 0; j < s.index.size(); ++j)
            {
                const std::size_t i = s.index[j];
                const HestonCalibrationQuote &q = quotes_[i];
                const Real call = black76_call_price(s.forward, q.strike, s.ttm, q.implied_vol, s.discount);
                is_call_[i] = q.strike >= s.forward;
                market_price_[i] = is_call_[i] ? call : call - s.discount * (s.forward - q.strike);
                market_vega_[i] = std::max(black76_vega(s.forward, q.strike, s.ttm, q.implied_vol, s.discount),
                                           kRelativeVegaFloor * s.forward);
            }
    }

    std::vector<Real> HestonSurfaceObjective::model_prices(const HestonParams &p, bool calls_only) const
    {
        std::vector<Real> out(n_quotes_);
        for (const Slice &s : slices_)
        {
            const std::vector<Real> calls =
                heston_cos_prices(s.forward, s.strikes, s.ttm, s.discount, p, true, cos_);
            for (std::size_t j = 0; j < s.index.size(); ++j)
            {
                const std::size_t i = s.index[j];
                out[i] = (calls_only || is_call_[i])
                             ? calls[j]
                             : calls[j] - s.discount * (s.forward - s.strikes[j]);
            }
        }
        return out;
    }

    std::vector<Real> HestonSurfaceObjective::residuals(const std::vector<Real> &params) const
    {
        const std::vector<Real> model = model_prices(unpack(params), false);
        std::vector<Real> r(n_quotes_);
        for (std::size_t i = 0; i < n_quotes_; ++i)
            r[i] = (model[i] - market_price_[i]) / market_vega_[i];
        return r;
    }

    std::vector<Real> HestonSurfaceObjective::weights() const
    {
        std::vector<Real> w(n_quotes_);
        for (std::size_t i = 0; i < n_quotes_; ++i)
            w[i] = quotes_[i].weight;
        return w;
    }

    // Bounds wide enough for any listed equity underlying (instantaneous and
    // long-run vol up to ~140 %), tight enough to keep the characteristic
    // function well conditioned: kappa and xi bounded away from 0.
    std::vector<Real> HestonSurfaceObjective::lower_bounds() const
    {
        return {1e-4, 0.05, 1e-4, 0.05, -0.99};
    }

    std::vector<Real> HestonSurfaceObjective::upper_bounds() const
    {
        return {2.0, 15.0, 2.0, 4.0, 0.99};
    }

    HestonParams HestonSurfaceObjective::unpack(const std::vector<Real> &params) noexcept
    {
        HestonParams p;
        p.v0 = params[0];
        p.kappa = params[1];
        p.theta = params[2];
        p.xi = params[3];
        p.rho = params[4];
        return p;
    }

    std::vector<Real> HestonSurfaceObjective::pack(const HestonParams &p)
    {
        return {p.v0, p.kappa, p.theta, p.xi, p.rho};
    }

    std::vector<std::vector<Real>> HestonSurfaceObjective::initial_guess_candidates() const
    {
        const Slice &shortest = *std::min_element(
            slices_.begin(), slices_.end(), [](const Slice &a, const Slice &b)
            { return a.ttm < b.ttm; });
        std::size_t atm = shortest.index.front();
        for (const std::size_t i : shortest.index)
            if (std::fabs(std::log(quotes_[i].strike / shortest.forward)) <
                std::fabs(std::log(quotes_[atm].strike / shortest.forward)))
                atm = i;
        const Real var0 = quotes_[atm].implied_vol * quotes_[atm].implied_vol;

        std::vector<std::vector<Real>> candidates;
        for (const Real kappa0 : {1.0, 4.0})
            for (const Real xi0 : {0.3, 1.0})
                for (const Real rho0 : {-0.7, -0.3})
                    candidates.push_back({var0, kappa0, var0, xi0, rho0});
        return candidates;
    }

    std::vector<Real> HestonSurfaceObjective::implied_vol_errors(const HestonParams &p) const
    {
        const std::vector<Real> calls = model_prices(p, true);
        std::vector<Real> err(n_quotes_, std::numeric_limits<Real>::quiet_NaN());
        for (const Slice &s : slices_)
            for (std::size_t j = 0; j < s.index.size(); ++j)
            {
                const std::size_t i = s.index[j];
                const Real iv = black76_implied_vol(calls[i], s.forward, s.strikes[j], s.ttm, s.discount);
                if (std::isfinite(iv))
                    err[i] = iv - quotes_[i].implied_vol;
            }
        return err;
    }

    HestonCalibration calibrate_heston(
        std::vector<HestonCalibrationQuote> quotes, Real spot, Real rate, Real dividend,
        const calibration::LevenbergMarquardtSettings &settings,
        const HestonCOSSettings &cos)
    {
        const HestonSurfaceObjective objective(std::move(quotes), spot, rate, dividend, cos);
        const auto candidates = objective.initial_guess_candidates();

        HestonCalibration result;
        result.n_quotes = objective.num_residuals();
        result.n_starts = candidates.size();

        // The starts are independent and the objective is read-only, so they
        // run on one thread each: a handful of threads, a few KB apiece.
        std::vector<std::future<calibration::CalibrationReport>> runs;
        runs.reserve(candidates.size());
        for (const auto &start : candidates)
            runs.push_back(std::async(std::launch::async, [&objective, &settings, start]
                                      { return calibration::levenberg_marquardt(objective, start, settings); }));

        Real best = std::numeric_limits<Real>::infinity();
        for (auto &run : runs)
        {
            calibration::CalibrationReport report = run.get();
            if (report.rmse < best)
            {
                best = report.rmse;
                result.report = std::move(report);
            }
        }
        result.params = HestonSurfaceObjective::unpack(result.report.params);
        result.feller = feller_condition_satisfied(result.params);

        result.n_maturities = objective.num_maturities();

        // A quote whose model price has no Black-76 implied vol is counted in
        // n_unpriced, never folded into the RMSE as a silent zero.
        Real sq = 0.0;
        std::size_t n = 0;
        for (const Real e : objective.implied_vol_errors(result.params))
        {
            if (!std::isfinite(e))
            {
                ++result.n_unpriced;
                continue;
            }
            result.iv_worst = std::max(result.iv_worst, std::fabs(e));
            sq += e * e;
            ++n;
        }
        result.iv_rmse = n > 0 ? std::sqrt(sq / static_cast<Real>(n))
                               : std::numeric_limits<Real>::quiet_NaN();
        return result;
    }

} // namespace quantModeling
