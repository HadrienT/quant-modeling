#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/cds.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/xva_market_risks.hpp"
#include "quantModeling/instruments/credit/cds.hpp"
#include "quantModeling/market/credit_bootstrap.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/risk/regulatory/sa_cva.hpp"

#include <cmath>
#include <vector>

// Lot X8 of blueprint/wp/23-xva.md (§14.13): from the model's inputs to the
// market's quotes, and SA-CVA from the sensitivities. The oracle of the map
// is what a desk would do by hand: move a quote, rebuild the curve, calibrate
// again, price again.

namespace quantModeling
{
    namespace
    {
        const std::vector<Time> kSpreadTenors(sa_cva::credit_spread_tenors.begin(),
                                              sa_cva::credit_spread_tenors.end());

        /// The quotes a run starts from.
        struct Quotes
        {
            std::vector<std::pair<Time, Real>> swaps{{1.0, 0.0455}, {2.0, 0.0473}, {3.0, 0.0477}, {5.0, 0.0478}, {7.0, 0.0481}, {10.0, 0.0486}, {15.0, 0.0500}};
            std::vector<SwaptionVolQuote> vols{{0.5, 10.0, 0.0100}, {1.0, 2.0, 0.0117}, {1.0, 5.0, 0.0111}, {1.0, 10.0, 0.0103}, {2.0, 5.0, 0.0114}, {2.0, 10.0, 0.0107}};
            /// The counterparty's credit spread at the five tenors of MAR50.65.
            std::vector<Real> spreads{0.0080, 0.0090, 0.0105, 0.0115, 0.0130};
            Real own_hazard = 0.008;
        };

        DiscountCurve curve(const Quotes &q)
        {
            std::vector<ParRateQuote> par;
            for (const auto &[tenor, rate] : q.swaps)
                par.push_back(make_ois_quote(tenor, rate));
            return bootstrap_curve({}, par);
        }

        CreditCurve counterparty(const Quotes &q, const DiscountCurve &c)
        {
            std::vector<CdsQuote> cds;
            for (std::size_t k = 0; k < kSpreadTenors.size(); ++k)
                cds.push_back({kSpreadTenors[k], q.spreads[k]});
            return bootstrap_credit_curve(cds, c, 0.4);
        }

        void book(HullWhiteExposureEngine &engine)
        {
            engine.add(make_swap(0.0, 10.0, 0.0486, 1, 1, 1e7, true));
            engine.add(make_swap(0.0, 5.0, 0.0470, 1, 1, 6e6, false));
            engine.add(Swaption{make_swap(2.0, 5.0, 0.0490, 1, 1, 5e6, true), 2.0});
        }

        ExposureSimulationSettings settings()
        {
            ExposureSimulationSettings s;
            s.paths = 128;
            s.seed = 20261002;
            return s;
        }

        /// What a set of quotes prices to: everything rebuilt from them.
        struct Run
        {
            DiscountCurve curve;
            HullWhiteCalibration calibration;
            CreditCurve counterparty, own;
            XvaRiskInputs inputs;
        };

        Run build(const Quotes &q)
        {
            const DiscountCurve c = curve(q);
            Run r{c, calibrate_hull_white(c, c, q.vols, 1, 1), counterparty(q, c), CreditCurve(q.own_hazard), {}};
            r.inputs.counterparty = r.counterparty;
            r.inputs.own = r.own;
            r.inputs.borrowing_spread = 0.004;
            r.inputs.lending_spread = 0.001;
            return r;
        }

        XvaMarketQuotes market(const Quotes &q)
        {
            XvaMarketQuotes m;
            m.swap_rates = q.swaps;
            m.swaption_vols = q.vols;
            m.counterparty_spread_tenors = kSpreadTenors;
            return m;
        }

        XvaValues values(const Quotes &q, const std::vector<Real> &widths)
        {
            Run r = build(q);
            const HullWhiteCurveModel model(r.calibration.mean_reversion, r.calibration.sigma, r.curve);
            HullWhiteExposureEngine engine(model);
            book(engine);
            r.inputs.smoothing_widths = widths;
            return engine.xva_values(settings(), r.inputs);
        }

        struct Base
        {
            Quotes quotes;
            Run run = build(quotes);
            HullWhiteCurveModel model{run.calibration.mean_reversion, run.calibration.sigma, run.curve};
            XvaRisks risks;
            XvaMarketRisks market_risks;

            Base()
            {
                HullWhiteExposureEngine engine(model);
                book(engine);
                risks = engine.xva_risks(settings(), run.inputs);
                market_risks = xva_market_risks(risks, model, run.counterparty, run.own, market(quotes));
            }
        };

        /// The change of each adjustment for a bumped set of quotes, rebuilt
        /// and recalibrated, per unit of the bump.
        std::array<Real, kXvaOutputs> rebuilt(const Base &base, const Quotes &up, const Quotes &down, Real h)
        {
            const XvaValues high = values(up, base.risks.smoothing_widths);
            const XvaValues low = values(down, base.risks.smoothing_widths);
            std::array<Real, kXvaOutputs> d{};
            for (std::size_t o = 0; o < kXvaOutputs; ++o)
                d[o] = (high.values[o].value - low.values[o].value) / (2.0 * h);
            return d;
        }

        /// `scale`: the largest sensitivity of the output among the quotes
        /// of the kind. The bump goes through an optimiser and a bisection,
        /// and through a path-by-path estimator with kinks: a few thousandths
        /// of it is their noise.
        void expect_close(const XvaQuoteRisk &risk, const std::array<Real, kXvaOutputs> &bump,
                          const std::array<Real, kXvaOutputs> &scale)
        {
            for (std::size_t o = 0; o < kXvaOutputs; ++o)
                EXPECT_NEAR(risk.risk[o].value, bump[o], 0.02 * std::abs(bump[o]) + 2e-3 * scale[o])
                    << risk.label << ", output " << o;
        }

        std::array<Real, kXvaOutputs> largest(const std::vector<XvaQuoteRisk> &risks)
        {
            std::array<Real, kXvaOutputs> scale{};
            for (const XvaQuoteRisk &q : risks)
                for (std::size_t o = 0; o < kXvaOutputs; ++o)
                    scale[o] = std::max(scale[o], std::abs(q.risk[o].value));
            return scale;
        }
    } // namespace

    TEST(XvaMarketRisks, ASwapRateRiskIsRebuildRecalibrateReprice)
    {
        const Base base;
        ASSERT_EQ(base.market_risks.swap_rates.size(), base.quotes.swaps.size());
        for (std::size_t i = 0; i < base.quotes.swaps.size(); ++i)
        {
            const Real h = 2e-6;
            Quotes up = base.quotes, down = base.quotes;
            up.swaps[i].second += h;
            down.swaps[i].second -= h;
            // The credit spreads are quotes too: they stay, and the hazard
            // rates follow the curve.
            expect_close(base.market_risks.swap_rates[i], rebuilt(base, up, down, h),
                         largest(base.market_risks.swap_rates));
            EXPECT_GT(base.market_risks.swap_rates[i].risk[0].error, 0.0);
        }
        EXPECT_EQ(base.market_risks.swap_rates[5].label, "swap rate 10Y");
        EXPECT_DOUBLE_EQ(base.market_risks.swap_rates[5].level, 0.0486);
    }

    TEST(XvaMarketRisks, ASwaptionVolRiskIsRecalibrateReprice)
    {
        const Base base;
        ASSERT_EQ(base.market_risks.swaption_vols.size(), base.quotes.vols.size());
        Real total = 0.0;
        for (std::size_t j = 0; j < base.quotes.vols.size(); ++j)
        {
            const Real h = 2e-5;
            Quotes up = base.quotes, down = base.quotes;
            up.vols[j].normal_vol += h;
            down.vols[j].normal_vol -= h;
            expect_close(base.market_risks.swaption_vols[j], rebuilt(base, up, down, h),
                         largest(base.market_risks.swaption_vols));
            total += base.market_risks.swaption_vols[j].risk[0].value;
        }
        // More volatility, more exposure: the CVA, a negative number, falls.
        EXPECT_LT(total, 0.0);
        EXPECT_EQ(base.market_risks.swaption_vols[0].label, "swaption vol 6M into 10Y");
    }

    TEST(XvaMarketRisks, TheCalibrationMovesAsTheImplicitFunctionTheoremSays)
    {
        const Base base;
        const auto recalibrated = [](const Quotes &q)
        {
            const DiscountCurve c = curve(q);
            const HullWhiteCalibration k = calibrate_hull_white(c, c, q.vols, 1, 1);
            return std::array<Real, 2>{k.mean_reversion, k.sigma};
        };
        const auto check = [&](const std::array<Real, 2> &predicted, const Quotes &up, const Quotes &down, Real h,
                               const char *what)
        {
            const std::array<Real, 2> high = recalibrated(up), low = recalibrated(down);
            for (std::size_t p = 0; p < 2; ++p)
            {
                const Real moved = (high[p] - low[p]) / (2.0 * h);
                // The Gauss-Newton Hessian drops the second derivatives of
                // residuals of a few basis points: a percent.
                EXPECT_NEAR(predicted[p], moved, 0.03 * std::abs(moved) + (p == 0 ? 2e-3 : 2e-4)) << what << p;
            }
        };
        for (std::size_t j = 0; j < base.quotes.vols.size(); ++j)
        {
            const Real h = 1e-5;
            Quotes up = base.quotes, down = base.quotes;
            up.vols[j].normal_vol += h;
            down.vols[j].normal_vol -= h;
            check(base.market_risks.calibration[j], up, down, h, "vol ");
        }
        for (std::size_t i = 0; i < base.quotes.swaps.size(); ++i)
        {
            const Real h = 1e-5;
            Quotes up = base.quotes, down = base.quotes;
            up.swaps[i].second += h;
            down.swaps[i].second -= h;
            check(base.market_risks.calibration_to_swap_rates[i], up, down, h, "swap ");
        }
        // A higher quoted vol asks for a higher σ.
        Real sigma_to_vols = 0.0;
        for (const auto &d : base.market_risks.calibration)
            sigma_to_vols += d[1];
        EXPECT_GT(sigma_to_vols, 0.5);
        EXPECT_LT(sigma_to_vols, 2.0);
    }

    TEST(XvaMarketRisks, ACreditSpreadRiskIsRebootstrapReprice)
    {
        const Base base;
        ASSERT_EQ(base.market_risks.counterparty_spreads.size(), 5u);
        EXPECT_TRUE(base.market_risks.counterparty_in_spreads);
        for (std::size_t k = 0; k < 5; ++k)
        {
            const XvaQuoteRisk &risk = base.market_risks.counterparty_spreads[k];
            // The par spread read back off the hazard curve is the quote.
            EXPECT_NEAR(risk.level, base.quotes.spreads[k], 1e-9);
            const Real h = 1e-6;
            Quotes up = base.quotes, down = base.quotes;
            up.spreads[k] += h;
            down.spreads[k] -= h;
            const auto bump = rebuilt(base, up, down, h);
            for (std::size_t o = 0; o < kXvaOutputs; ++o)
                EXPECT_NEAR(risk.risk[o].value, bump[o], 1e-4 * std::abs(bump[o]) + 1e-6 * std::abs(risk.risk[o].value))
                    << risk.label << ", output " << o;
        }
        // The CVA falls (grows as a cost) when the counterparty's spread
        // widens, wherever on the curve.
        Real parallel = 0.0;
        for (const XvaQuoteRisk &risk : base.market_risks.counterparty_spreads)
            parallel += risk.risk[0].value;
        EXPECT_LT(parallel, 0.0);
        EXPECT_EQ(base.market_risks.counterparty_spreads[0].label, "counterparty credit spread 6M");
        // The bank's own curve was given without quotes: hazard rates.
        ASSERT_EQ(base.market_risks.own_spreads.size(), 1u);
        EXPECT_EQ(base.market_risks.own_spreads[0].label, "own hazard rate");
        // Funding spreads and losses given default pass through.
        ASSERT_EQ(base.market_risks.others.size(), 4u);
        EXPECT_NEAR(base.market_risks.others[2].risk[2].value, base.risks[XvaOutput::Fca].value / 0.004,
                    1e-9 * std::abs(base.risks[XvaOutput::Fca].value / 0.004));
    }

    TEST(XvaMarketRisks, WhatDoesNotBelongTogetherIsRefused)
    {
        const Base base;
        XvaMarketQuotes other = market(base.quotes);
        other.swap_rates[2].second += 0.001; // not the curve the model has
        EXPECT_THROW(xva_market_risks(base.risks, base.model, base.run.counterparty, base.run.own, other),
                     InvalidInput);
        other = market(base.quotes);
        other.swaption_vols.clear();
        EXPECT_THROW(xva_market_risks(base.risks, base.model, base.run.counterparty, base.run.own, other),
                     InvalidInput);
        other = market(base.quotes);
        other.counterparty_spread_tenors = {1.0, 5.0};
        EXPECT_THROW(xva_market_risks(base.risks, base.model, base.run.counterparty, base.run.own, other),
                     InvalidInput);
        // Hazard rates are not credit spreads: SA-CVA wants the spreads.
        other = market(base.quotes);
        other.counterparty_spread_tenors.clear();
        const XvaMarketRisks in_hazards =
            xva_market_risks(base.risks, base.model, base.run.counterparty, base.run.own, other);
        EXPECT_FALSE(in_hazards.counterparty_in_spreads);
        EXPECT_THROW(sa_cva_sensitivities(in_hazards, ba_cva::Sector::Financial,
                                          ba_cva::CreditQuality::InvestmentGrade),
                     InvalidInput);
    }

    // ── SA-CVA (MAR50) ───────────────────────────────────────────────────────

    TEST(SaCva, TheCapitalOfAToyPortfolioIsTheOneWorkedByHand)
    {
        sa_cva::Sensitivities s;
        s.sector = ba_cva::Sector::Financial;
        s.quality = ba_cva::CreditQuality::InvestmentGrade;
        // One tenor of each kind: risk weight × sensitivity.
        s.interest_rate_delta[2] = 1e6; // 5 years
        s.interest_rate_vega = 5e4;
        s.credit_spread_delta[3] = -2e6; // 5 years
        sa_cva::Capital k = sa_cva::capital(s);
        EXPECT_DOUBLE_EQ(k.interest_rate_delta, 0.0074 * 1e6);
        EXPECT_DOUBLE_EQ(k.interest_rate_vega, 1.0 * 5e4);
        EXPECT_DOUBLE_EQ(k.credit_spread_delta, 0.05 * 2e6);
        EXPECT_DOUBLE_EQ(k.total(), 7400.0 + 50000.0 + 100000.0);

        // Two tenors: sqrt(a² + b² + 2 ρ a b), ρ from Table 4 (2Y and 5Y:
        // 87 %) and from MAR50.65 (two tenors of one name: 90 %).
        s.interest_rate_delta[1] = -5e5; // 2 years
        s.credit_spread_delta[4] = -1e6; // 10 years
        k = sa_cva::capital(s);
        const Real a = 0.0074 * 1e6, b = 0.0093 * -5e5;
        EXPECT_NEAR(k.interest_rate_delta, std::sqrt(a * a + b * b + 2.0 * 0.87 * a * b), 1e-9);
        const Real c = 0.05 * -2e6, d = 0.05 * -1e6;
        EXPECT_NEAR(k.credit_spread_delta, std::sqrt(c * c + d * d + 2.0 * 0.90 * c * d), 1e-9);
        // Opposite sensitivities offset, to the correlation.
        EXPECT_LT(k.interest_rate_delta, a);

        // The multiplier scales; a high-yield name weighs more.
        EXPECT_NEAR(sa_cva::capital(s, 1.25).total(), 1.25 * k.total(), 1e-9);
        s.quality = ba_cva::CreditQuality::HighYieldOrNotRated;
        EXPECT_NEAR(sa_cva::capital(s).credit_spread_delta, 12.0 / 5.0 * k.credit_spread_delta, 1e-9);
        EXPECT_THROW(sa_cva::capital(s, 0.5), InvalidInput);
    }

    TEST(SaCva, TheTablesAreThoseOfTheText)
    {
        using ba_cva::CreditQuality;
        using ba_cva::Sector;
        // Table 7 of MAR50.65.
        EXPECT_DOUBLE_EQ(sa_cva::credit_spread_risk_weight(Sector::Sovereign, CreditQuality::InvestmentGrade), 0.005);
        EXPECT_DOUBLE_EQ(sa_cva::credit_spread_risk_weight(Sector::LocalGovernment, CreditQuality::InvestmentGrade), 0.01);
        EXPECT_DOUBLE_EQ(sa_cva::credit_spread_risk_weight(Sector::ConsumerTransportAdministrative,
                                                           CreditQuality::HighYieldOrNotRated),
                         0.085);
        EXPECT_DOUBLE_EQ(sa_cva::credit_spread_risk_weight(Sector::HealthCareUtilitiesProfessional,
                                                           CreditQuality::InvestmentGrade),
                         0.015);
        EXPECT_DOUBLE_EQ(sa_cva::credit_spread_risk_weight(Sector::Other, CreditQuality::HighYieldOrNotRated), 0.12);
        // Table 4 of MAR50.56: symmetric, ones on the diagonal, falling with
        // the distance between tenors.
        for (std::size_t i = 0; i < 5; ++i)
        {
            EXPECT_EQ(sa_cva::interest_rate_correlations[i][i], 1.0);
            for (std::size_t j = 0; j < 5; ++j)
                EXPECT_EQ(sa_cva::interest_rate_correlations[i][j], sa_cva::interest_rate_correlations[j][i]);
            for (std::size_t j = i + 2; j < 5; ++j)
                EXPECT_LT(sa_cva::interest_rate_correlations[i][j], sa_cva::interest_rate_correlations[i][j - 1]);
        }
        EXPECT_DOUBLE_EQ(sa_cva::interest_rate_correlations[0][4], 0.31);
        EXPECT_DOUBLE_EQ(sa_cva::interest_rate_risk_weights[0], 0.0111);
    }

    TEST(SaCva, ASensitivityBetweenTwoTenorsIsSharedBetweenThem)
    {
        const std::vector<Time> tenors{0.5, 1.0, 3.0, 7.0, 15.0, 40.0};
        const std::vector<Real> s{1.0, 2.0, 3.0, 5.0, 4.0, 6.0};
        const auto out = sa_cva::to_tenors(sa_cva::interest_rate_tenors, tenors.data(), s.data(), s.size());
        // 6 months and 1 year go to 1Y; 3Y is a third of the way from 2Y to
        // 5Y; 7Y two fifths from 5Y to 10Y; 15Y a quarter from 10Y to 30Y;
        // 40Y goes to 30Y.
        EXPECT_NEAR(out[0], 1.0 + 2.0, 1e-12);
        EXPECT_NEAR(out[1], 3.0 * 2.0 / 3.0, 1e-12);
        EXPECT_NEAR(out[2], 3.0 / 3.0 + 5.0 * 0.6, 1e-12);
        EXPECT_NEAR(out[3], 5.0 * 0.4 + 4.0 * 0.75, 1e-12);
        EXPECT_NEAR(out[4], 4.0 * 0.25 + 6.0, 1e-12);
        // Nothing is lost: a parallel shift is a parallel shift.
        EXPECT_NEAR(out[0] + out[1] + out[2] + out[3] + out[4], 21.0, 1e-12);
    }

    TEST(SaCva, FromTheAdjointRisksOfANettingSet)
    {
        const Base base;
        const sa_cva::Sensitivities s = sa_cva_sensitivities(base.market_risks, ba_cva::Sector::Financial,
                                                             ba_cva::CreditQuality::InvestmentGrade);
        const std::size_t regulatory = static_cast<std::size_t>(XvaOutput::CvaUnilateral);
        // A parallel shift of the swap rates is a parallel shift of the
        // prescribed tenors; the vega is a relative shift of every vol.
        Real parallel = 0.0, vega = 0.0, spreads = 0.0;
        for (const XvaQuoteRisk &q : base.market_risks.swap_rates)
            parallel += q.risk[regulatory].value;
        for (const XvaQuoteRisk &q : base.market_risks.swaption_vols)
            vega += q.level * q.risk[regulatory].value;
        for (const XvaQuoteRisk &q : base.market_risks.counterparty_spreads)
            spreads += q.risk[regulatory].value;
        Real sum = 0.0;
        for (const Real x : s.interest_rate_delta)
            sum += x;
        EXPECT_NEAR(sum, parallel, 1e-9 * std::abs(parallel));
        EXPECT_NEAR(s.interest_rate_vega, vega, 1e-12 * std::abs(vega));
        sum = 0.0;
        for (const Real x : s.credit_spread_delta)
            sum += x;
        EXPECT_NEAR(sum, spreads, 1e-9 * std::abs(spreads));
        EXPECT_EQ(s.interest_rate_delta[4], base.market_risks.swap_rates[6].risk[regulatory].value * 0.25);

        const sa_cva::Capital k = sa_cva::capital(s);
        EXPECT_GT(k.interest_rate_delta, 0.0);
        EXPECT_GT(k.interest_rate_vega, 0.0);
        EXPECT_GT(k.credit_spread_delta, 0.0);
        // The credit spread of the counterparty is what a CVA is most
        // exposed to.
        EXPECT_GT(k.credit_spread_delta, k.interest_rate_delta);
    }

} // namespace quantModeling
