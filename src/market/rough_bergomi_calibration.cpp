#include "quantModeling/market/rough_bergomi_calibration.hpp"

#include "quantModeling/market/calibration/objective_function.hpp"
#include "quantModeling/models/equity/sabr.hpp"

#include <cmath>
#include <limits>

namespace quantModeling
{
    namespace
    {
        /// Residual of a quote the model cannot price (no implied vol): large
        /// and finite, so the optimiser moves away instead of stopping.
        constexpr Real kUnpriced = 0.5;

        class RoughBergomiObjective final : public calibration::ObjectiveFunction
        {
          public:
            RoughBergomiObjective(const std::vector<RoughBergomiTarget> &targets, const ForwardVarianceCurve &xi,
                                  const RoughBergomiSurfaceSettings &mc)
                : targets_(targets), xi_(xi), mc_(mc)
            {
                for (const RoughBergomiTarget &t : targets_)
                    quotes_.push_back({t.ttm, t.k});
            }

            std::size_t num_params() const override { return 3; }
            std::size_t num_residuals() const override { return targets_.size(); }

            static RoughBergomiParams unpack(const std::vector<Real> &p) { return {p[0], p[1], p[2]}; }

            RoughBergomiSurface surface(const std::vector<Real> &p) const
            {
                return rough_bergomi_surface(unpack(p), xi_, quotes_, mc_);
            }

            std::vector<Real> residuals(const std::vector<Real> &p) const override
            {
                const RoughBergomiSurface s = surface(p);
                std::vector<Real> r(targets_.size());
                for (std::size_t i = 0; i < r.size(); ++i)
                    r[i] = std::isfinite(s.implied_vols[i]) ? s.implied_vols[i] - targets_[i].implied_vol : kUnpriced;
                return r;
            }

            std::vector<Real> lower_bounds() const override { return {0.02, 0.2, -0.99}; }
            std::vector<Real> upper_bounds() const override { return {0.45, 5.0, 0.2}; }

          private:
            const std::vector<RoughBergomiTarget> &targets_;
            const ForwardVarianceCurve &xi_;
            RoughBergomiSurfaceSettings mc_;
            std::vector<RoughBergomiQuote> quotes_;
        };
    } // namespace

    RoughBergomiCalibration calibrate_rough_bergomi(const std::vector<RoughBergomiTarget> &targets,
                                                    const std::vector<Time> &ttm,
                                                    const std::vector<Real> &total_variance,
                                                    const RoughBergomiSurfaceSettings &mc,
                                                    RoughBergomiParams initial)
    {
        if (targets.size() < 3)
            throw InvalidInput("calibrate_rough_bergomi: need at least three quotes for three parameters");
        RoughBergomiCalibration out;
        out.xi = ForwardVarianceCurve::from_total_variance(ttm, total_variance, &out.xi_floored);

        const RoughBergomiObjective objective(targets, out.xi, mc);
        calibration::LevenbergMarquardtSettings lm;
        // Each call prices a surface by Monte-Carlo: forward differences, and
        // stop at a relative cost change far below the Monte-Carlo error.
        lm.central_differences = false;
        lm.fd_step = 1e-4;
        lm.cost_tol = 1e-8;
        lm.step_tol = 1e-6;
        lm.gradient_tol = 1e-9;
        lm.max_iterations = 60;
        out.report = calibration::levenberg_marquardt(objective, {initial.H, initial.eta, initial.rho}, lm);
        out.params = RoughBergomiObjective::unpack(out.report.params);

        const RoughBergomiSurface s = objective.surface(out.report.params);
        Real sum2 = 0.0;
        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            const Real v = s.implied_vols[i];
            out.model_vols.push_back(v);
            Real err = std::numeric_limits<Real>::quiet_NaN();
            if (std::isfinite(v))
            {
                const Real K = std::exp(targets[i].k);
                const Real vega = black76_vega(1.0, K, targets[i].ttm, v, 1.0);
                if (vega > 0.0)
                    err = s.std_errors[i] / vega;
                const Real e = v - targets[i].implied_vol;
                sum2 += e * e;
                out.iv_worst = std::max(out.iv_worst, std::abs(e));
                ++out.n_quotes;
            }
            else
                ++out.n_unpriced;
            out.model_vol_errors.push_back(err);
        }
        out.iv_rmse = out.n_quotes > 0 ? std::sqrt(sum2 / static_cast<Real>(out.n_quotes)) : 0.0;
        return out;
    }

} // namespace quantModeling
