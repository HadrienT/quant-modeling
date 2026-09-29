#include "quantModeling/market/hull_white_calibration.hpp"

#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/market/calibration/objective_function.hpp"

#include <cmath>
#include <numeric>

namespace quantModeling
{
    namespace
    {
        constexpr Real kBp = 1e4;
        constexpr Real kMinA = 1e-4, kMaxA = 1.0, kMinSigma = 1e-5, kMaxSigma = 0.2;

        struct AtmSwaption
        {
            Swaption swaption;
            Real forward;
            Real annuity;
        };

        AtmSwaption atm_swaption(const DiscountCurve &discount, const DiscountCurve &projection, Time expiry,
                                 Time tenor, int fixed_frequency, int float_frequency)
        {
            Swaption s(make_swap(expiry, tenor, 0.0, fixed_frequency, float_frequency), expiry);
            const SwaptionForward f = swaption_forward(s, MultiCurve{discount, projection});
            s.swap.fixed_rate = f.forward;
            return {s, f.forward, f.annuity};
        }

        Real model_vol(const HullWhiteCurveModel &model, const AtmSwaption &atm)
        {
            const Real price = hull_white_european_swaption(atm.swaption, model);
            return bachelier_implied_vol(true, price, atm.forward, atm.forward, atm.swaption.expiry, atm.annuity);
        }

        class HullWhiteObjective final : public calibration::ObjectiveFunction
        {
          public:
            HullWhiteObjective(const DiscountCurve &discount, const DiscountCurve &projection,
                               std::vector<AtmSwaption> atms, std::vector<SwaptionVolQuote> quotes,
                               std::optional<Real> fixed_a)
                : discount_(discount), projection_(projection), atms_(std::move(atms)), quotes_(std::move(quotes)),
                  fixed_a_(fixed_a)
            {
            }

            std::size_t num_params() const override { return fixed_a_ ? 1 : 2; }
            std::size_t num_residuals() const override { return quotes_.size(); }

            HullWhiteCurveModel model(const std::vector<Real> &p) const
            {
                return fixed_a_ ? HullWhiteCurveModel(*fixed_a_, p[0], discount_, projection_)
                                : HullWhiteCurveModel(p[0], p[1], discount_, projection_);
            }

            std::vector<Real> residuals(const std::vector<Real> &p) const override
            {
                const HullWhiteCurveModel m = model(p);
                std::vector<Real> r(quotes_.size());
                for (std::size_t i = 0; i < quotes_.size(); ++i)
                {
                    const Real v = model_vol(m, atms_[i]);
                    // A price Bachelier cannot invert is a failed point, not a
                    // zero residual: a large finite penalty keeps LM away.
                    r[i] = std::isfinite(v) ? (v - quotes_[i].normal_vol) * kBp : 1e3;
                }
                return r;
            }

            std::vector<Real> weights() const override
            {
                std::vector<Real> w;
                for (const SwaptionVolQuote &q : quotes_)
                    w.push_back(q.weight);
                return w;
            }

            std::vector<Real> lower_bounds() const override
            {
                return fixed_a_ ? std::vector<Real>{kMinSigma} : std::vector<Real>{kMinA, kMinSigma};
            }
            std::vector<Real> upper_bounds() const override
            {
                return fixed_a_ ? std::vector<Real>{kMaxSigma} : std::vector<Real>{kMaxA, kMaxSigma};
            }

          private:
            const DiscountCurve &discount_;
            const DiscountCurve &projection_;
            std::vector<AtmSwaption> atms_;
            std::vector<SwaptionVolQuote> quotes_;
            std::optional<Real> fixed_a_;
        };
    } // namespace

    Real hull_white_atm_normal_vol(const HullWhiteCurveModel &model, Time expiry, Time tenor, int fixed_frequency,
                                   int float_frequency)
    {
        return model_vol(model, atm_swaption(model.discount(), model.projection(), expiry, tenor, fixed_frequency,
                                             float_frequency));
    }

    HullWhiteCalibration calibrate_hull_white(const DiscountCurve &discount, const DiscountCurve &projection,
                                              const std::vector<SwaptionVolQuote> &quotes, int fixed_frequency,
                                              int float_frequency, std::optional<Real> fixed_mean_reversion)
    {
        if (quotes.empty())
            throw InvalidInput("calibrate_hull_white: need at least one swaption quote");
        if (!fixed_mean_reversion && quotes.size() < 2)
            throw InvalidInput("calibrate_hull_white: fitting both a and sigma needs at least two quotes");
        std::vector<AtmSwaption> atms;
        Real mean_vol = 0.0;
        for (const SwaptionVolQuote &q : quotes)
        {
            if (!(q.expiry > 0.0) || !(q.tenor > 0.0) || !(q.normal_vol > 0.0))
                throw InvalidInput("calibrate_hull_white: expiry, tenor and vol must be > 0");
            atms.push_back(atm_swaption(discount, projection, q.expiry, q.tenor, fixed_frequency, float_frequency));
            mean_vol += q.normal_vol / static_cast<Real>(quotes.size());
        }
        if (fixed_mean_reversion && !(*fixed_mean_reversion >= kMinA && *fixed_mean_reversion <= kMaxA))
            throw InvalidInput("calibrate_hull_white: the fixed mean reversion must lie in [1e-4, 1]");

        const HullWhiteObjective objective(discount, projection, atms, quotes, fixed_mean_reversion);
        // σ of a short rate is close to the normal vol of short swap rates.
        std::vector<Real> guess = fixed_mean_reversion ? std::vector<Real>{mean_vol}
                                                       : std::vector<Real>{0.03, mean_vol};
        HullWhiteCalibration out;
        // Each residual inverts a price to a vol: the cost carries ~1e-12 of
        // noise, below which no step can decrease it. Stop on a relative
        // decrease of 1e-10 rather than exhaust λ at that noise floor.
        calibration::LevenbergMarquardtSettings settings;
        settings.cost_tol = 1e-10;
        settings.step_tol = 1e-10;
        out.report = calibration::levenberg_marquardt(objective, guess, settings);
        const std::vector<Real> &p = out.report.params;
        out.mean_reversion = fixed_mean_reversion ? *fixed_mean_reversion : p[0];
        out.sigma = fixed_mean_reversion ? p[0] : p[1];
        const HullWhiteCurveModel model = objective.model(p);
        for (std::size_t i = 0; i < quotes.size(); ++i)
        {
            out.strikes.push_back(atms[i].forward);
            out.market_vols.push_back(quotes[i].normal_vol);
            out.model_vols.push_back(model_vol(model, atms[i]));
        }
        return out;
    }

} // namespace quantModeling
