#include "quantModeling/engines/xva/xva_market_risks.hpp"

#include "quantModeling/engines/analytic/cds.hpp"
#include "quantModeling/instruments/credit/cds.hpp"
#include "quantModeling/market/credit_bootstrap.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/utils/accumulators.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-9;
        constexpr int kCdsFrequency = 4;
        /// Bounds of the Hull-White calibration (market/hull_white_calibration.cpp).
        constexpr Real kMinA = 1e-4, kMaxA = 1.0;

        std::string years(Time t)
        {
            char buffer[32];
            if (std::abs(t - std::round(t)) < 1e-9)
                std::snprintf(buffer, sizeof buffer, "%.0fY", t);
            else if (t < 1.0 && std::abs(12.0 * t - std::round(12.0 * t)) < 1e-9)
                std::snprintf(buffer, sizeof buffer, "%.0fM", 12.0 * t);
            else
                std::snprintf(buffer, sizeof buffer, "%.2fY", t);
            return buffer;
        }

        std::vector<Real> zero_rates(const DiscountCurve &curve)
        {
            std::vector<Real> z;
            for (std::size_t k = 0; k < curve.pillar_times().size(); ++k)
                z.push_back(-std::log(curve.pillar_discount_factors()[k]) / curve.pillar_times()[k]);
            return z;
        }

        DiscountCurve bootstrap(const std::vector<std::pair<Time, Real>> &rates)
        {
            std::vector<ParRateQuote> quotes;
            for (const auto &[tenor, rate] : rates)
                quotes.push_back(make_ois_quote(tenor, rate));
            return bootstrap_curve({}, quotes);
        }

        DiscountCurve from_zero_rates(const std::vector<Time> &times, const std::vector<Real> &zero,
                                      CurveExtrapolation extrapolation)
        {
            std::vector<Real> dfs;
            for (std::size_t k = 0; k < times.size(); ++k)
                dfs.push_back(std::exp(-zero[k] * times[k]));
            return DiscountCurve(times, dfs, extrapolation);
        }

        /// Solves the 1 × 1 or 2 × 2 system A x = b.
        std::array<Real, 2> solve(const std::array<std::array<Real, 2>, 2> &A, const std::array<Real, 2> &b,
                                  std::size_t n)
        {
            if (n == 1)
                return {b[0] / A[0][0], 0.0};
            const Real det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
            if (!(std::abs(det) > 0.0))
                throw InvalidInput("xVA market risks: the swaption volatilities do not determine a and "
                                   "sigma separately; fix the mean reversion");
            return {(b[0] * A[1][1] - b[1] * A[0][1]) / det, (A[0][0] * b[1] - A[1][0] * b[0]) / det};
        }

        /// The par spread of a CDS of each tenor on a hazard curve, and the
        /// derivative of the hazard rates to each spread: dh[j][k] = dh_j/ds_k.
        struct SpreadMap
        {
            std::vector<Real> spreads;
            std::vector<std::vector<Real>> dh;
            /// dh_dzero[j][k] = dh_j/dz_k at fixed spreads: a credit default
            /// swap is discounted on the curve, so the hazard rates that
            /// reprice the same spreads move with it.
            std::vector<std::vector<Real>> dh_dzero;
        };

        SpreadMap spread_map(const CreditCurve &credit, const std::vector<Time> &tenors,
                             const DiscountCurve &discount, Real recovery)
        {
            if (credit.times().size() != tenors.size() || credit.hazards().size() != tenors.size())
                throw InvalidInput("xVA market risks: the hazard curve needs one period per spread tenor");
            for (std::size_t k = 0; k < tenors.size(); ++k)
                if (std::abs(credit.times()[k] - tenors[k]) > kTimeEps)
                    throw InvalidInput("xVA market risks: the hazard curve's periods must end at the "
                                       "spread tenors");
            SpreadMap map;
            for (const Time t : tenors)
                map.spreads.push_back(
                    cds_legs(make_cds(t, 0.0, kCdsFrequency), discount, credit, recovery).par_spread());
            const auto hazards_on = [&](const std::vector<Real> &spreads, const DiscountCurve &c)
            {
                std::vector<CdsQuote> quotes;
                for (std::size_t k = 0; k < tenors.size(); ++k)
                    quotes.push_back({tenors[k], spreads[k]});
                return bootstrap_credit_curve(quotes, c, recovery, kCdsFrequency).hazards();
            };
            const auto hazards = [&](const std::vector<Real> &spreads)
            { return hazards_on(spreads, discount); };
            map.dh.assign(tenors.size(), std::vector<Real>(tenors.size(), 0.0));
            for (std::size_t k = 0; k < tenors.size(); ++k)
            {
                const Real h = 1e-6 * std::max(map.spreads[k], 1e-4);
                std::vector<Real> up = map.spreads, down = map.spreads;
                up[k] += h;
                down[k] -= h;
                const std::vector<Real> high = hazards(up), low = hazards(down);
                for (std::size_t j = 0; j < tenors.size(); ++j)
                    map.dh[j][k] = (high[j] - low[j]) / (2.0 * h);
            }
            const std::vector<Time> &pillars = discount.pillar_times();
            const std::vector<Real> zero = zero_rates(discount);
            map.dh_dzero.assign(tenors.size(), std::vector<Real>(pillars.size(), 0.0));
            for (std::size_t k = 0; k < pillars.size(); ++k)
            {
                constexpr Real h = 1e-5;
                std::vector<Real> up = zero, down = zero;
                up[k] += h;
                down[k] -= h;
                const std::vector<Real> high =
                    hazards_on(map.spreads, from_zero_rates(pillars, up, discount.extrapolation()));
                const std::vector<Real> low =
                    hazards_on(map.spreads, from_zero_rates(pillars, down, discount.extrapolation()));
                for (std::size_t j = 0; j < tenors.size(); ++j)
                    map.dh_dzero[j][k] = (high[j] - low[j]) / (2.0 * h);
            }
            return map;
        }
    } // namespace

    XvaMarketRisks xva_market_risks(const XvaRisks &risks, const HullWhiteCurveModel &model,
                                    const CreditCurve &counterparty, const CreditCurve &own,
                                    const XvaMarketQuotes &quotes)
    {
        const std::size_t P = risks.factors.size();
        if (quotes.swaption_vols.empty())
            throw InvalidInput("xVA market risks: need the swaption volatilities the model was "
                               "calibrated to");
        const DiscountCurve &curve = model.discount();
        const std::vector<Time> &pillars = curve.pillar_times();
        const std::size_t K = pillars.size();

        // Where each input sits among the factors.
        std::vector<std::size_t> zero_at, hazard_c_at, hazard_i_at;
        std::size_t a_at = P, sigma_at = P;
        for (std::size_t j = 0; j < P; ++j)
            switch (risks.factors[j])
            {
                case XvaRiskFactor::ZeroRate:
                    zero_at.push_back(j);
                    break;
                case XvaRiskFactor::MeanReversion:
                    a_at = j;
                    break;
                case XvaRiskFactor::Sigma:
                    sigma_at = j;
                    break;
                case XvaRiskFactor::CounterpartyHazard:
                    hazard_c_at.push_back(j);
                    break;
                case XvaRiskFactor::OwnHazard:
                    hazard_i_at.push_back(j);
                    break;
                default:
                    break;
            }
        if (zero_at.size() != K || K == 0 || a_at == P || sigma_at == P)
            throw InvalidInput("xVA market risks: these risks are not those of this model's curve");

        // ── The curve: dz/dq, by bumping the bootstrap ──────────────────────
        const std::size_t Q = quotes.swap_rates.size();
        const DiscountCurve rebuilt = bootstrap(quotes.swap_rates);
        if (rebuilt.pillar_times().size() != K)
            throw InvalidInput("xVA market risks: the model's curve is not the bootstrap of these "
                               "swap rates");
        const std::vector<Real> zero = zero_rates(curve), zero_rebuilt = zero_rates(rebuilt);
        for (std::size_t k = 0; k < K; ++k)
            if (std::abs(rebuilt.pillar_times()[k] - pillars[k]) > kTimeEps ||
                std::abs(zero_rebuilt[k] - zero[k]) > 1e-9)
                throw InvalidInput("xVA market risks: the model's curve is not the bootstrap of these "
                                   "swap rates");
        // dz[k][i] = d zero_k / d quote_i.
        std::vector<std::vector<Real>> dz(K, std::vector<Real>(Q, 0.0));
        for (std::size_t i = 0; i < Q; ++i)
        {
            constexpr Real h = 1e-6;
            auto up = quotes.swap_rates, down = quotes.swap_rates;
            up[i].second += h;
            down[i].second -= h;
            const std::vector<Real> high = zero_rates(bootstrap(up)), low = zero_rates(bootstrap(down));
            for (std::size_t k = 0; k < K; ++k)
                dz[k][i] = (high[k] - low[k]) / (2.0 * h);
        }

        // ── The calibration: implicit function theorem at the optimum ───────
        const Real a = model.mean_reversion(), sigma = model.sigma();
        const bool a_free = !quotes.fixed_mean_reversion && a > kMinA * (1.0 + 1e-6) && a < kMaxA * (1.0 - 1e-6);
        const std::size_t n_theta = a_free ? 2 : 1;
        const std::size_t n = quotes.swaption_vols.size();
        const auto model_vols = [&](Real a_, Real sigma_, const DiscountCurve &c)
        {
            const HullWhiteCurveModel m(a_, sigma_, c);
            std::vector<Real> v;
            for (const SwaptionVolQuote &q : quotes.swaption_vols)
                v.push_back(hull_white_atm_normal_vol(m, q.expiry, q.tenor, quotes.fixed_frequency,
                                                      quotes.float_frequency));
            return v;
        };
        const auto difference = [](const std::vector<Real> &high, const std::vector<Real> &low, Real h)
        {
            std::vector<Real> d(high.size());
            for (std::size_t j = 0; j < high.size(); ++j)
                d[j] = (high[j] - low[j]) / (2.0 * h);
            return d;
        };
        // The calibration is at its optimum where the gradient of the cost
        // vanishes: F(θ; z, m) = J(θ, z)ᵀ W (v(θ, z) - m) = 0, v the model's
        // vols and J = ∂v/∂θ. Hence dθ/dx = -(∂F/∂θ)⁻¹ ∂F/∂x. The residuals
        // of a two-parameter fit are a few basis points, not zero: ∂F/∂θ and
        // ∂F/∂z keep the second derivatives of v, which a Gauss-Newton
        // Hessian would drop (it is off by a quarter on some pillars here).
        const Real step_a = 1e-4, step_sigma = 1e-2 * sigma;
        const auto jacobian = [&](Real a_, Real sigma_, const DiscountCurve &c)
        {
            // J[theta][quote]: σ always, a when it was fitted and is inside
            // its bounds.
            std::vector<std::vector<Real>> J;
            if (a_free)
                J.push_back(difference(model_vols(a_ + step_a, sigma_, c), model_vols(a_ - step_a, sigma_, c),
                                       step_a));
            J.push_back(difference(model_vols(a_, sigma_ + step_sigma, c), model_vols(a_, sigma_ - step_sigma, c),
                                   step_sigma));
            return J;
        };
        const auto gradient = [&](Real a_, Real sigma_, const DiscountCurve &c)
        {
            const std::vector<std::vector<Real>> J = jacobian(a_, sigma_, c);
            const std::vector<Real> v = model_vols(a_, sigma_, c);
            std::array<Real, 2> F{};
            for (std::size_t p = 0; p < n_theta; ++p)
                for (std::size_t j = 0; j < n; ++j)
                    F[p] += J[p][j] * quotes.swaption_vols[j].weight * (v[j] - quotes.swaption_vols[j].normal_vol);
            return F;
        };
        const auto slope = [n_theta](const std::array<Real, 2> &high, const std::array<Real, 2> &low, Real h)
        {
            std::array<Real, 2> d{};
            for (std::size_t p = 0; p < n_theta; ++p)
                d[p] = (high[p] - low[p]) / (2.0 * h);
            return d;
        };
        // H[p][q] = ∂F_p/∂θ_q.
        std::array<std::array<Real, 2>, 2> H{};
        {
            std::size_t q = 0;
            if (a_free)
            {
                const Real h = 10.0 * step_a;
                const std::array<Real, 2> d = slope(gradient(a + h, sigma, curve), gradient(a - h, sigma, curve), h);
                for (std::size_t p = 0; p < n_theta; ++p)
                    H[p][q] = d[p];
                ++q;
            }
            const Real h = 10.0 * step_sigma;
            const std::array<Real, 2> d = slope(gradient(a, sigma + h, curve), gradient(a, sigma - h, curve), h);
            for (std::size_t p = 0; p < n_theta; ++p)
                H[p][q] = d[p];
        }
        const std::vector<std::vector<Real>> J = jacobian(a, sigma, curve);
        // d theta / d vol_j = H⁻¹ J_j w_j (∂F/∂m_j = -J_j w_j), as (a, σ).
        std::vector<std::array<Real, 2>> dtheta_dvol(n);
        for (std::size_t j = 0; j < n; ++j)
        {
            std::array<Real, 2> b{};
            for (std::size_t p = 0; p < n_theta; ++p)
                b[p] = J[p][j] * quotes.swaption_vols[j].weight;
            const std::array<Real, 2> x = solve(H, b, n_theta);
            dtheta_dvol[j] = a_free ? x : std::array<Real, 2>{0.0, x[0]};
        }
        // d theta / d zero_k = -H⁻¹ ∂F/∂z_k.
        std::vector<std::array<Real, 2>> dtheta_dzero(K);
        for (std::size_t k = 0; k < K; ++k)
        {
            constexpr Real h = 1e-5;
            std::vector<Real> up = zero, down = zero;
            up[k] += h;
            down[k] -= h;
            const std::array<Real, 2> dF =
                slope(gradient(a, sigma, from_zero_rates(pillars, up, curve.extrapolation())),
                      gradient(a, sigma, from_zero_rates(pillars, down, curve.extrapolation())), h);
            const std::array<Real, 2> x = solve(H, {-dF[0], -dF[1]}, n_theta);
            dtheta_dzero[k] = a_free ? x : std::array<Real, 2>{0.0, x[0]};
        }

        // ── The linear map, quote by quote: a row of weights on the factors ─
        struct Row
        {
            XvaQuoteRisk *target;
            std::vector<Real> weights;
        };
        XvaMarketRisks out;
        std::vector<Row> rows;
        // Credit spreads are quotes: at fixed spreads the hazard rates follow
        // the curve.
        struct Followers
        {
            const std::vector<std::size_t> *at;
            SpreadMap map;
        };
        std::vector<Followers> followers;
        if (!quotes.counterparty_spread_tenors.empty())
            followers.push_back(
                {&hazard_c_at, spread_map(counterparty, quotes.counterparty_spread_tenors, curve, quotes.recovery)});
        if (!quotes.own_spread_tenors.empty())
            followers.push_back({&hazard_i_at, spread_map(own, quotes.own_spread_tenors, curve, quotes.recovery)});
        out.swap_rates.resize(Q);
        for (std::size_t i = 0; i < Q; ++i)
        {
            XvaQuoteRisk &q = out.swap_rates[i];
            q.tenor = quotes.swap_rates[i].first;
            q.level = quotes.swap_rates[i].second;
            q.label = "swap rate " + years(q.tenor);
            std::vector<Real> w(P, 0.0);
            for (std::size_t k = 0; k < K; ++k)
            {
                // The zero rate moves, and the calibration follows it.
                w[zero_at[k]] += dz[k][i];
                w[a_at] += dtheta_dzero[k][0] * dz[k][i];
                w[sigma_at] += dtheta_dzero[k][1] * dz[k][i];
                for (const Followers &f : followers)
                    for (std::size_t j = 0; j < f.at->size() && j < f.map.dh_dzero.size(); ++j)
                        w[(*f.at)[j]] += f.map.dh_dzero[j][k] * dz[k][i];
            }
            out.calibration_to_swap_rates.push_back({w[a_at], w[sigma_at]});
            rows.push_back({&q, std::move(w)});
        }
        out.swaption_vols.resize(n);
        for (std::size_t j = 0; j < n; ++j)
        {
            XvaQuoteRisk &q = out.swaption_vols[j];
            q.expiry = quotes.swaption_vols[j].expiry;
            q.tenor = quotes.swaption_vols[j].tenor;
            q.level = quotes.swaption_vols[j].normal_vol;
            q.label = "swaption vol " + years(q.expiry) + " into " + years(q.tenor);
            std::vector<Real> w(P, 0.0);
            w[a_at] = dtheta_dvol[j][0];
            w[sigma_at] = dtheta_dvol[j][1];
            rows.push_back({&q, std::move(w)});
        }
        out.calibration = dtheta_dvol;

        const auto credit_rows = [&](const CreditCurve &credit, const std::vector<Time> &tenors,
                                     const std::vector<std::size_t> &at, std::vector<XvaQuoteRisk> &target,
                                     const char *who)
        {
            if (tenors.empty())
            {
                // No quotes given: the hazard rates as they are.
                target.resize(at.size());
                for (std::size_t j = 0; j < at.size(); ++j)
                {
                    target[j].label = risks.labels[at[j]];
                    target[j].tenor = risks.tenors[at[j]];
                    target[j].level = risks.levels[at[j]];
                    std::vector<Real> w(P, 0.0);
                    w[at[j]] = 1.0;
                    rows.push_back({&target[j], std::move(w)});
                }
                return;
            }
            if (at.size() != tenors.size())
                throw InvalidInput("xVA market risks: the hazard curve needs one period per spread tenor");
            const SpreadMap map = spread_map(credit, tenors, curve, quotes.recovery);
            target.resize(tenors.size());
            for (std::size_t k = 0; k < tenors.size(); ++k)
            {
                target[k].label = std::string(who) + " credit spread " + years(tenors[k]);
                target[k].tenor = tenors[k];
                target[k].level = map.spreads[k];
                std::vector<Real> w(P, 0.0);
                for (std::size_t j = 0; j < tenors.size(); ++j)
                    w[at[j]] = map.dh[j][k];
                rows.push_back({&target[k], std::move(w)});
            }
        };
        credit_rows(counterparty, quotes.counterparty_spread_tenors, hazard_c_at, out.counterparty_spreads,
                    "counterparty");
        credit_rows(own, quotes.own_spread_tenors, hazard_i_at, out.own_spreads, "own");
        out.counterparty_in_spreads = !quotes.counterparty_spread_tenors.empty();

        for (std::size_t j = 0; j < P; ++j)
            if (risks.factors[j] == XvaRiskFactor::CounterpartyLgd || risks.factors[j] == XvaRiskFactor::OwnLgd ||
                risks.factors[j] == XvaRiskFactor::BorrowingSpread || risks.factors[j] == XvaRiskFactor::LendingSpread)
                out.others.emplace_back();
        std::size_t other = 0;
        for (std::size_t j = 0; j < P; ++j)
            if (risks.factors[j] == XvaRiskFactor::CounterpartyLgd || risks.factors[j] == XvaRiskFactor::OwnLgd ||
                risks.factors[j] == XvaRiskFactor::BorrowingSpread || risks.factors[j] == XvaRiskFactor::LendingSpread)
            {
                XvaQuoteRisk &q = out.others[other++];
                q.label = risks.labels[j];
                q.level = risks.levels[j];
                std::vector<Real> w(P, 0.0);
                w[j] = 1.0;
                rows.push_back({&q, std::move(w)});
            }

        // Applied batch by batch: the mean over the paths, and the dispersion
        // of the batches for the error.
        const std::size_t batches = risks.batches;
        constexpr std::size_t kBatch = 64;
        for (const Row &row : rows)
            for (std::size_t o = 0; o < kXvaOutputs; ++o)
            {
                WelfordAccumulator acc;
                Real sum = 0.0;
                for (std::size_t b = 0; b < batches; ++b)
                {
                    const Real *batch = risks.batch_risks.data() + (b * kXvaOutputs + o) * P;
                    Real x = 0.0;
                    for (std::size_t j = 0; j < P; ++j)
                        if (row.weights[j] != 0.0)
                            x += row.weights[j] * batch[j];
                    acc.add(x);
                    sum += x * static_cast<Real>(std::min(kBatch, risks.paths - b * kBatch));
                }
                row.target->risk[o] = {sum / static_cast<Real>(risks.paths), acc.std_error()};
            }
        return out;
    }

    sa_cva::Sensitivities sa_cva_sensitivities(const XvaMarketRisks &risks, ba_cva::Sector sector,
                                               ba_cva::CreditQuality quality)
    {
        if (!risks.counterparty_in_spreads)
            throw InvalidInput("SA-CVA: the counterparty's risks must be to its credit spreads; give "
                               "the spread tenors to xva_market_risks()");
        const std::size_t regulatory = static_cast<std::size_t>(XvaOutput::CvaUnilateral);
        sa_cva::Sensitivities s;
        s.sector = sector;
        s.quality = quality;
        const auto spread = [regulatory](const std::vector<XvaQuoteRisk> &quotes, const std::array<Time, 5> &to)
        {
            std::vector<Time> tenors;
            std::vector<Real> values;
            for (const XvaQuoteRisk &q : quotes)
            {
                tenors.push_back(q.tenor);
                values.push_back(q.risk[regulatory].value);
            }
            return sa_cva::to_tenors(to, tenors.data(), values.data(), tenors.size());
        };
        s.interest_rate_delta = spread(risks.swap_rates, sa_cva::interest_rate_tenors);
        s.credit_spread_delta = spread(risks.counterparty_spreads, sa_cva::credit_spread_tenors);
        // All volatilities up by the same relative amount: Σ vol dCVA/dvol.
        for (const XvaQuoteRisk &q : risks.swaption_vols)
            s.interest_rate_vega += q.level * q.risk[regulatory].value;
        return s;
    }

} // namespace quantModeling
