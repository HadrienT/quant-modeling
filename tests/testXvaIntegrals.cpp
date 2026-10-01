#include <gtest/gtest.h>

#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/risk/xva.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {
        std::vector<Time> uniform_grid(Time T, int n)
        {
            std::vector<Time> times;
            for (int i = 1; i <= n; ++i)
                times.push_back(T * i / n);
            return times;
        }

        /// The bell of an interest rate swap, EE(t) ∝ sqrt(t) (T - t), with a
        /// negative exposure of a different size so that nothing cancels by
        /// accident.
        ExposureProfile swap_like_profile(Time T, int n, Real ee_scale = 1.0, Real ene_scale = 0.6)
        {
            ExposureProfile p;
            p.times = uniform_grid(T, n);
            for (const Time t : p.times)
            {
                const Real shape = std::sqrt(t) * (T - t);
                p.discounted_ee.push_back(ee_scale * shape);
                p.discounted_ene.push_back(-ene_scale * shape);
            }
            return p;
        }

        ExposureProfile constant_profile(std::vector<Time> times, Real ee, Real ene)
        {
            ExposureProfile p;
            p.discounted_ee.assign(times.size(), ee);
            p.discounted_ene.assign(times.size(), ene);
            p.times = std::move(times);
            return p;
        }

        const CreditCurve no_default(0.0);
    } // namespace

    // ── CVA (blueprint §7.1 - §7.3) ──────────────────────────────────────────

    TEST(Cva, BoughtOptionIsLgdTimesValueTimesDefaultProbability)
    {
        // A bought option has EE*(t) = V_0 at every date (its discounted value
        // is a positive martingale): CVA = -LGD V_0 PD(0, T), whatever the
        // grid and whatever the shape of the credit curve.
        const Real v0 = 12.5, lgd = 0.6;
        const CreditCurve curve({1.0, 3.0, 5.0}, {0.01, 0.025, 0.04});
        const Real expected = -lgd * v0 * curve.default_probability(5.0);
        EXPECT_NEAR(cva_unilateral(constant_profile(uniform_grid(5.0, 60), v0, 0.0), curve, lgd),
                    expected, 1e-13);
        EXPECT_NEAR(
            cva_unilateral(constant_profile({0.3, 0.31, 2.0, 4.9, 5.0}, v0, 0.0), curve, lgd),
            expected, 1e-13);
        EXPECT_NEAR(cva_unilateral(constant_profile({5.0}, v0, 0.0), curve, lgd), expected, 1e-13);
    }

    TEST(Cva, SoldOptionHasNoCva)
    {
        // Premium received up front: the exposure is never positive.
        const ExposureProfile sold = constant_profile(uniform_grid(5.0, 20), 0.0, -8.0);
        EXPECT_DOUBLE_EQ(cva_unilateral(sold, CreditCurve(0.05), 0.6), 0.0);
        EXPECT_GT(dva(sold, no_default, CreditCurve(0.02), 0.6), 0.0);
    }

    TEST(Cva, IsACostZeroWithoutDefaultAndLinearInLgd)
    {
        const ExposureProfile p = swap_like_profile(5.0, 60);
        const CreditCurve curve(0.03);
        EXPECT_LT(cva_unilateral(p, curve, 0.6), 0.0);
        EXPECT_DOUBLE_EQ(cva_unilateral(p, no_default, 0.6), 0.0);
        EXPECT_DOUBLE_EQ(cva_unilateral(p, curve, 0.0), 0.0);
        EXPECT_NEAR(cva_unilateral(p, curve, 0.6), 2.0 * cva_unilateral(p, curve, 0.3), 1e-15);
    }

    TEST(Cva, GrowsWithTheHazardRate)
    {
        const ExposureProfile p = swap_like_profile(5.0, 60);
        Real previous = 0.0;
        for (const Real hazard : {0.005, 0.01, 0.02, 0.05, 0.10})
        {
            const Real cva = cva_unilateral(p, CreditCurve(hazard), 0.6);
            EXPECT_LT(cva, previous);
            previous = cva;
        }
    }

    TEST(Cva, ConvergesAtOrderOneToTheIntegral)
    {
        // A forward-like profile EE(t) = 1 + t against a flat hazard λ:
        //   CVA = -LGD ∫_0^T (1 + t) λ exp(-λ t) dt
        //       = -LGD [(1 - e^{-λT}) + (1 - e^{-λT} (1 + λT)) / λ].
        // The right-endpoint sum of Gregory's discrete formula is first
        // order: the slope in log-log is measured.
        const Real T = 5.0, lambda = 0.03, lgd = 0.6;
        const Real decay = std::exp(-lambda * T);
        const Real exact = -lgd * ((1.0 - decay) + (1.0 - decay * (1.0 + lambda * T)) / lambda);

        Real previous_error = 0.0;
        for (const int n : {25, 50, 100, 200, 400})
        {
            ExposureProfile p;
            p.times = uniform_grid(T, n);
            for (const Time t : p.times)
            {
                p.discounted_ee.push_back(1.0 + t);
                p.discounted_ene.push_back(0.0);
            }
            const Real cva = cva_unilateral(p, CreditCurve(lambda), lgd);
            // Sampling a rising exposure at the end of each period overstates it.
            EXPECT_LT(cva, exact);
            const Real error = std::abs(cva - exact);
            if (previous_error > 0.0)
            {
                EXPECT_NEAR(std::log2(previous_error / error), 1.0, 0.02) << "n=" << n;
            }
            previous_error = error;
        }
        EXPECT_LT(previous_error, 1e-2 * std::abs(exact));
    }

    TEST(Cva, SpreadApproximationHoldsWithinAFewPercent)
    {
        // Gregory's rule: CVA ≈ -spread × EPE × T. Swap-like profile over
        // five years against a 200 bp counterparty with 40 % recovery.
        const Real T = 5.0, spread = 0.02, lgd = 0.6;
        const ExposureProfile p = swap_like_profile(T, 600);
        const CreditCurve curve(hazard_from_spread(spread, lgd));
        const Real exact = cva_unilateral(p, curve, lgd);
        const Real epe = expected_positive_exposure(p.times, p.discounted_ee);
        const Real approximation = cva_spread_approximation(spread, epe, T);
        EXPECT_LT(approximation, 0.0);
        // The rule ignores the decay of the survival probability, so it
        // overstates the cost a little.
        EXPECT_LT(approximation, exact);
        EXPECT_NEAR(approximation / exact, 1.0, 0.08);
    }

    TEST(Cva, IsNearlyInsensitiveToRecoveryAtAGivenSpread)
    {
        // The credit triangle: at a fixed spread a higher recovery means a
        // higher hazard rate, and LGD × PD barely moves.
        const ExposureProfile p = swap_like_profile(5.0, 120);
        const Real spread = 0.02;
        const auto cva_at = [&](Real recovery)
        {
            const Real lgd = 1.0 - recovery;
            return cva_unilateral(p, CreditCurve(hazard_from_spread(spread, lgd)), lgd);
        };
        EXPECT_NEAR(cva_at(0.2) / cva_at(0.4), 1.0, 0.02);
        EXPECT_NEAR(cva_at(0.6) / cva_at(0.4), 1.0, 0.04);
        // Whereas at a fixed hazard rate it is exactly linear in LGD.
        EXPECT_NEAR(cva_unilateral(p, CreditCurve(0.03), 0.8) /
                        cva_unilateral(p, CreditCurve(0.03), 0.6),
                    0.8 / 0.6, 1e-13);
    }

    TEST(CreditTriangle, HazardIsSpreadOverLgd)
    {
        // Blueprint §6.2: 150 bp at 40 % recovery is a 2.5 % hazard, i.e. an
        // 11.8 % default probability over five years.
        const Real hazard = hazard_from_spread(0.015, 0.6);
        EXPECT_NEAR(hazard, 0.025, 1e-15);
        EXPECT_NEAR(CreditCurve(hazard).default_probability(5.0), 1.0 - std::exp(-0.125), 1e-15);
        EXPECT_NEAR(CreditCurve(hazard).default_probability(5.0), 0.118, 5e-4);
        EXPECT_THROW(hazard_from_spread(0.01, 0.0), InvalidInput);
        EXPECT_THROW(hazard_from_spread(-0.01, 0.6), InvalidInput);
    }

    // ── Bilateral CVA and DVA (blueprint §7.4) ───────────────────────────────

    TEST(BilateralCva, CvaOfOneSideIsDvaOfTheOther)
    {
        // Same netting set, same curves, seen from each side: the cost of
        // counterparty risk to the bank is the benefit of own credit to the
        // counterparty. That symmetry is what lets two parties agree on a price.
        const ExposureProfile bank_view = swap_like_profile(7.0, 84, 1.3, 0.4);
        const ExposureProfile counterparty_view = bank_view.seen_from_counterparty();
        const CreditCurve bank({2.0, 7.0}, {0.008, 0.015});
        const CreditCurve counterparty({1.0, 3.0, 7.0}, {0.02, 0.03, 0.05});
        const Real lgd_bank = 0.6, lgd_counterparty = 0.75;

        const Real cva_bank = cva_bilateral(bank_view, counterparty, bank, lgd_counterparty);
        const Real dva_bank = dva(bank_view, counterparty, bank, lgd_bank);
        const Real cva_cpty = cva_bilateral(counterparty_view, bank, counterparty, lgd_bank);
        const Real dva_cpty = dva(counterparty_view, bank, counterparty, lgd_counterparty);

        EXPECT_NEAR(cva_bank, -dva_cpty, 1e-14);
        EXPECT_NEAR(dva_bank, -cva_cpty, 1e-14);
        // Hence the bilateral adjustment is the same price for both, with
        // opposite signs.
        EXPECT_NEAR(cva_bank + dva_bank, -(cva_cpty + dva_cpty), 1e-14);
        EXPECT_LT(cva_bank, 0.0);
        EXPECT_GT(dva_bank, 0.0);
    }

    TEST(BilateralCva, FirstToDefaultShrinksTheUnilateralCva)
    {
        const ExposureProfile p = swap_like_profile(5.0, 60);
        const CreditCurve counterparty(0.04), bank(0.02);
        const Real unilateral = cva_unilateral(p, counterparty, 0.6);
        const Real bilateral = cva_bilateral(p, counterparty, bank, 0.6);
        // Scenarios where the bank defaults first no longer count.
        EXPECT_GT(bilateral, unilateral);
        EXPECT_LT(bilateral, 0.0);
        // A default-free bank gives the unilateral number back, bit for bit.
        EXPECT_DOUBLE_EQ(cva_bilateral(p, counterparty, no_default, 0.6), unilateral);
        // And a default-free bank has no DVA.
        EXPECT_DOUBLE_EQ(dva(p, counterparty, no_default, 0.6), 0.0);
    }

    // ── FVA (blueprint §9.2) ─────────────────────────────────────────────────

    TEST(Fva, SymmetricSpreadReducesToTheExpectedFutureValue)
    {
        // FS_B = FS_L = FS: FVA = -FS Σ EFV*(t_i) S_C S_I Δt_i.
        const ExposureProfile p = swap_like_profile(5.0, 60, 1.0, 0.35);
        const CreditCurve counterparty(0.03), bank(0.01);
        const Real fs = 0.012;
        const Real symmetric =
            running_cost_adjustment(p.times, p.discounted_efv(), counterparty, bank, fs);
        EXPECT_NEAR(fva(p, counterparty, bank, fs, fs), symmetric, 1e-15);

        // Without survival weighting it is literally -FS Σ EFV* Δt.
        Real sum = 0.0;
        Time previous = 0.0;
        const std::vector<Real> efv = p.discounted_efv();
        for (std::size_t i = 0; i < p.times.size(); ++i)
        {
            sum += efv[i] * (p.times[i] - previous);
            previous = p.times[i];
        }
        EXPECT_NEAR(fva(p, no_default, no_default, fs, fs), -fs * sum, 1e-15);
    }

    TEST(Fva, CostAndBenefitHaveOppositeSigns)
    {
        const ExposureProfile p = swap_like_profile(5.0, 60);
        const CreditCurve counterparty(0.03), bank(0.01);
        const Real cost = fca(p, counterparty, bank, 0.015);
        const Real benefit = fba(p, counterparty, bank, 0.005);
        EXPECT_LT(cost, 0.0);
        EXPECT_GT(benefit, 0.0);
        EXPECT_DOUBLE_EQ(fva(p, counterparty, bank, 0.015, 0.005), cost + benefit);
        // Linear in the spread; nothing to fund at a zero spread.
        EXPECT_NEAR(fca(p, counterparty, bank, 0.03), 2.0 * cost, 1e-15);
        EXPECT_DOUBLE_EQ(fca(p, counterparty, bank, 0.0), 0.0);
        // Funding stops at the first default: survival only shrinks it.
        EXPECT_LT(std::abs(cost), std::abs(fca(p, no_default, no_default, 0.015)));
    }

    TEST(Fva, FlatProfileHasAClosedForm)
    {
        // Constant funding need of 100 for two years at 1 %, no default: -2.
        const ExposureProfile p = constant_profile(uniform_grid(2.0, 8), 100.0, 0.0);
        EXPECT_NEAR(fca(p, no_default, no_default, 0.01), -2.0, 1e-13);
    }

    // ── MVA and KVA (blueprint §10, §11.7) ───────────────────────────────────

    TEST(MvaKva, AreCostsOfHoldingMarginAndCapital)
    {
        const std::vector<Time> times = uniform_grid(4.0, 16);
        const std::vector<Real> profile(times.size(), 50.0);
        const CreditCurve counterparty(0.03), bank(0.01);

        // Flat 50 for four years at 1.5 %, no default: -3.
        EXPECT_NEAR(mva(times, profile, no_default, no_default, 0.015), -3.0, 1e-13);
        EXPECT_NEAR(kva(times, profile, no_default, no_default, 0.10), -20.0, 1e-13);
        EXPECT_LT(mva(times, profile, counterparty, bank, 0.015), 0.0);
        EXPECT_GT(mva(times, profile, counterparty, bank, 0.015), -3.0);

        // An empty netting set consumes neither margin nor capital.
        const std::vector<Real> nothing(times.size(), 0.0);
        EXPECT_DOUBLE_EQ(mva(times, nothing, counterparty, bank, 0.015), 0.0);
        EXPECT_DOUBLE_EQ(kva(times, nothing, counterparty, bank, 0.10), 0.0);

        // A margin remunerated above the funding spread is a gain.
        EXPECT_GT(mva(times, profile, counterparty, bank, -0.002), 0.0);

        std::vector<Real> negative = profile;
        negative[3] = -1.0;
        EXPECT_THROW(mva(times, negative, counterparty, bank, 0.015), InvalidInput);
        EXPECT_THROW(kva(times, negative, counterparty, bank, 0.10), InvalidInput);
    }

    // ── Input validation ─────────────────────────────────────────────────────

    TEST(ExposureProfileValidation, RejectsWrongSignsAndBadGrids)
    {
        const CreditCurve curve(0.02);
        ExposureProfile p = swap_like_profile(5.0, 10);
        EXPECT_NO_THROW(p.validate());

        ExposureProfile positive_ene = p;
        positive_ene.discounted_ene[2] = 0.1;
        EXPECT_THROW(cva_unilateral(positive_ene, curve, 0.6), InvalidInput);

        ExposureProfile negative_ee = p;
        negative_ee.discounted_ee[2] = -0.1;
        EXPECT_THROW(cva_unilateral(negative_ee, curve, 0.6), InvalidInput);

        ExposureProfile short_ene = p;
        short_ene.discounted_ene.pop_back();
        EXPECT_THROW(dva(short_ene, curve, curve, 0.6), InvalidInput);

        ExposureProfile unsorted = p;
        unsorted.times[3] = unsorted.times[2];
        EXPECT_THROW(fca(unsorted, curve, curve, 0.01), InvalidInput);

        EXPECT_THROW(cva_unilateral(p, curve, 1.2), InvalidInput);
        EXPECT_THROW(cva_unilateral(ExposureProfile{}, curve, 0.6), InvalidInput);
    }

} // namespace quantModeling
