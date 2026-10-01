#include <gtest/gtest.h>

#include "quantModeling/risk/regulatory/ba_cva.hpp"
#include "quantModeling/risk/regulatory/im_schedule.hpp"
#include "quantModeling/risk/regulatory/irb.hpp"

#include <cmath>
#include <utility>
#include <vector>

namespace quantModeling
{

    // ── IRB risk-weight function (CRE31) ─────────────────────────────────────

    TEST(Irb, ReproducesTheIllustrativeRiskWeightsOfTheBaselFramework)
    {
        // CRE99 Table 1, corporate exposures, LGD 40 %, maturity 2.5 years:
        // {PD, risk weight}, both in percent. Risk weight = 12.5 × K.
        const std::vector<std::pair<Real, Real>> table = {
            {0.05, 17.47},
            {0.10, 26.36},
            {0.25, 43.97},
            {0.40, 55.75},
            {0.50, 61.88},
            {0.75, 73.58},
            {1.00, 82.06},
            {1.30, 89.73},
            {1.50, 93.86},
            {2.00, 102.09},
            {2.50, 108.58},
            {3.00, 114.17},
            {4.00, 124.07},
            {5.00, 133.20},
            {6.00, 141.88},
            {10.00, 171.63},
            {15.00, 196.92},
            {20.00, 211.76},
        };
        for (const auto &[pd_percent, risk_weight_percent] : table)
        {
            const Real k = irb::capital_requirement(pd_percent / 100.0, 0.40, 2.5);
            EXPECT_NEAR(100.0 * irb::risk_weighted_assets(k, 1.0), risk_weight_percent, 0.005)
                << "PD=" << pd_percent << "%";
        }
    }

    TEST(Irb, AssetCorrelationRunsFrom24To12Percent)
    {
        EXPECT_NEAR(irb::asset_correlation(1e-9), 0.24, 1e-7);
        EXPECT_NEAR(irb::asset_correlation(0.999), 0.12, 1e-12);
        Real previous = irb::asset_correlation(1e-6);
        for (const Real pd : {0.001, 0.01, 0.05, 0.2, 0.5})
        {
            // Riskier firms default for more idiosyncratic reasons.
            const Real r = irb::asset_correlation(pd);
            EXPECT_LT(r, previous);
            previous = r;
        }
        // Large and unregulated financials: × 1.25.
        EXPECT_NEAR(irb::asset_correlation(0.01, true), 1.25 * irb::asset_correlation(0.01), 1e-15);
        EXPECT_GT(irb::capital_requirement(0.01, 0.45, 2.5, true),
                  irb::capital_requirement(0.01, 0.45, 2.5, false));
    }

    TEST(Irb, MaturityAdjustmentIsNeutralAtOneYearAndLinear)
    {
        const Real pd = 0.01, lgd = 0.45;
        const Real r = irb::asset_correlation(pd);
        // At M = 1 the factor (1 + (M - 2.5) b) / (1 - 1.5 b) is exactly 1:
        // K is the plain one-year unexpected loss of the Vasicek model.
        const Real stressed = 0.5 * std::erfc(-(-2.3263478740408408 + std::sqrt(r) * 3.090232306167813) /
                                              std::sqrt(1.0 - r) / std::sqrt(2.0));
        EXPECT_NEAR(irb::capital_requirement(pd, lgd, 1.0), lgd * (stressed - pd), 1e-9);
        // Linear in M.
        const Real k1 = irb::capital_requirement(pd, lgd, 1.0);
        const Real k3 = irb::capital_requirement(pd, lgd, 3.0);
        const Real k5 = irb::capital_requirement(pd, lgd, 5.0);
        EXPECT_NEAR(k5 - k3, k3 - k1, 1e-15);
        EXPECT_GT(k5, k3);
        // Linear in LGD.
        EXPECT_NEAR(irb::capital_requirement(pd, 0.9, 2.5), 2.0 * irb::capital_requirement(pd, lgd, 2.5),
                    1e-15);
    }

    TEST(Irb, RejectsInvalidInputs)
    {
        EXPECT_THROW(irb::capital_requirement(0.0, 0.45, 2.5), InvalidInput);
        EXPECT_THROW(irb::capital_requirement(1.0, 0.45, 2.5), InvalidInput);
        EXPECT_THROW(irb::capital_requirement(0.01, 1.5, 2.5), InvalidInput);
        EXPECT_THROW(irb::capital_requirement(0.01, 0.45, 0.0), InvalidInput);
        EXPECT_THROW(irb::risk_weighted_assets(-0.1, 100.0), InvalidInput);
    }

    // ── BA-CVA (MAR50.13 - MAR50.26) ─────────────────────────────────────────

    namespace
    {
        ba_cva::Counterparty counterparty(ba_cva::Sector sector, ba_cva::CreditQuality quality,
                                          Real ead, Time maturity)
        {
            ba_cva::Counterparty c;
            c.sector = sector;
            c.quality = quality;
            c.netting_sets = {{ead, maturity, false}};
            return c;
        }

        const ba_cva::CreditQuality ig = ba_cva::CreditQuality::InvestmentGrade;
        const ba_cva::CreditQuality hy = ba_cva::CreditQuality::HighYieldOrNotRated;
    } // namespace

    TEST(BaCva, RiskWeightsOfTable1)
    {
        using ba_cva::Sector;
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::Sovereign, ig), 0.005);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::Sovereign, hy), 0.02);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::LocalGovernment, ig), 0.01);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::LocalGovernment, hy), 0.04);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::Financial, ig), 0.05);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::Financial, hy), 0.12);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::BasicMaterialsEnergyIndustrials, ig), 0.03);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::BasicMaterialsEnergyIndustrials, hy), 0.07);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::ConsumerTransportAdministrative, ig), 0.03);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::ConsumerTransportAdministrative, hy), 0.085);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::TechnologyTelecommunications, ig), 0.02);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::TechnologyTelecommunications, hy), 0.055);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::HealthCareUtilitiesProfessional, ig), 0.015);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::HealthCareUtilitiesProfessional, hy), 0.05);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::Other, ig), 0.05);
        EXPECT_DOUBLE_EQ(ba_cva::risk_weight(Sector::Other, hy), 0.12);
    }

    TEST(BaCva, SupervisoryDiscountFactor)
    {
        EXPECT_DOUBLE_EQ(ba_cva::supervisory_discount_factor(0.0), 1.0);
        EXPECT_NEAR(ba_cva::supervisory_discount_factor(1e-9), 1.0, 1e-10);
        EXPECT_NEAR(ba_cva::supervisory_discount_factor(2.0), (1.0 - std::exp(-0.1)) / 0.1, 1e-15);
        // The average of exp(-0.05 t) over [0, M]: above the end-point value.
        EXPECT_GT(ba_cva::supervisory_discount_factor(10.0), std::exp(-0.5));
        EXPECT_LT(ba_cva::supervisory_discount_factor(10.0), 1.0);
    }

    TEST(BaCva, StandAloneCapitalByHand)
    {
        // An investment-grade bank: RW 5 %. EAD 100 over two years, SA-CCR:
        // (1 / 1.4) × 0.05 × 2 × 100 × 0.951626 = 6.7973.
        const ba_cva::Counterparty c = counterparty(ba_cva::Sector::Financial, ig, 100.0, 2.0);
        const Real expected = 0.05 * 2.0 * 100.0 * (1.0 - std::exp(-0.1)) / 0.1 / 1.4;
        EXPECT_NEAR(ba_cva::stand_alone_cva(c), expected, 1e-14);
        EXPECT_NEAR(expected, 6.7973, 5e-5);
        // One counterparty: rho² + (1 - rho²) = 1, K_reduced is its own charge.
        EXPECT_NEAR(ba_cva::k_reduced({c}), expected, 1e-14);
        EXPECT_NEAR(ba_cva::capital_reduced({c}), 0.65 * expected, 1e-14);

        // An IMM exposure is already discounted.
        ba_cva::Counterparty imm = c;
        imm.netting_sets[0].imm = true;
        EXPECT_NEAR(ba_cva::stand_alone_cva(imm), 0.05 * 2.0 * 100.0 / 1.4, 1e-14);

        // Netting sets of one counterparty add up.
        ba_cva::Counterparty two = c;
        two.netting_sets.push_back({100.0, 2.0, false});
        EXPECT_NEAR(ba_cva::stand_alone_cva(two), 2.0 * expected, 1e-14);
    }

    TEST(BaCva, DiversificationAcrossCounterparties)
    {
        const ba_cva::Counterparty c = counterparty(ba_cva::Sector::Financial, ig, 100.0, 2.0);
        const Real one = ba_cva::k_reduced({c});
        for (const int n : {2, 5, 20, 100})
        {
            // n identical counterparties: sqrt(rho² n² + (1 - rho²) n) times one.
            const std::vector<ba_cva::Counterparty> book(static_cast<std::size_t>(n), c);
            const Real k = ba_cva::k_reduced(book);
            EXPECT_NEAR(k, one * std::sqrt(0.25 * n * n + 0.75 * n), 1e-10);
            EXPECT_LT(k, n * one);       // less than the sum of the parts...
            EXPECT_GT(k, 0.5 * n * one); // ...but the systematic half never diversifies
        }
        EXPECT_DOUBLE_EQ(ba_cva::capital_reduced({}), 0.0);
    }

    TEST(BaCva, FullVersionWithoutHedgesIsTheReducedVersion)
    {
        const std::vector<ba_cva::Counterparty> book = {
            counterparty(ba_cva::Sector::Financial, ig, 100.0, 2.0),
            counterparty(ba_cva::Sector::TechnologyTelecommunications, hy, 40.0, 5.0),
            counterparty(ba_cva::Sector::Sovereign, ig, 300.0, 7.5),
        };
        EXPECT_NEAR(ba_cva::k_hedged(book, {}), ba_cva::k_reduced(book), 1e-14);
        EXPECT_NEAR(ba_cva::capital_full(book, {}), ba_cva::capital_reduced(book), 1e-14);
    }

    TEST(BaCva, HedgesReduceCapitalButNeverBelowTheFloor)
    {
        ba_cva::Counterparty c = counterparty(ba_cva::Sector::Financial, ig, 100.0, 2.0);
        const Real reduced = ba_cva::capital_reduced({c});

        // A direct CDS sized to cancel SCVA exactly: RW M B DF = SCVA needs
        // B = EAD / alpha on the same name and maturity.
        ba_cva::SingleNameHedge direct;
        direct.notional = 100.0 / ba_cva::alpha;
        direct.remaining_maturity = 2.0;
        direct.sector = ba_cva::Sector::Financial;
        direct.quality = ig;
        direct.relation = ba_cva::HedgeRelation::Direct;
        c.hedges = {direct};
        EXPECT_NEAR(ba_cva::k_hedged({c}, {}), 0.0, 1e-12);
        // Even a perfect hedge leaves beta = 25 % of the unhedged charge.
        EXPECT_NEAR(ba_cva::capital_full({c}, {}), 0.25 * reduced, 1e-12);

        // The same hedge on a merely comparable name (r = 50 %) only removes
        // half of SCVA and adds a misalignment term (1 - r²) SCVA²:
        //   K² = (rho SCVA/2)² + (1 - rho²)(SCVA/2)² + 0.75 SCVA² = SCVA².
        // A proxy hedge of that quality earns nothing: K_hedged cannot reach zero.
        c.hedges[0].relation = ba_cva::HedgeRelation::SameSectorAndRegion;
        const Real scva = ba_cva::stand_alone_cva(c);
        const Real indirect = ba_cva::k_hedged({c}, {});
        EXPECT_NEAR(indirect, scva * std::sqrt(0.0625 + 0.1875 + 0.75), 1e-12);
        EXPECT_NEAR(ba_cva::capital_full({c}, {}), reduced, 1e-12);

        c.hedges[0].relation = ba_cva::HedgeRelation::LegallyRelated;
        const Real related = ba_cva::k_hedged({c}, {});
        EXPECT_LT(related, indirect);
        EXPECT_GT(related, 0.0);
    }

    TEST(BaCva, IndexHedgeOffsetsOnlyTheSystematicPart)
    {
        const std::vector<ba_cva::Counterparty> book(
            10, counterparty(ba_cva::Sector::Financial, ig, 100.0, 2.0));
        const Real scva = ba_cva::stand_alone_cva(book[0]);

        // Sized so that IH = rho Σ SCVA: the systematic term vanishes and the
        // idiosyncratic one, sqrt((1 - rho²) n) SCVA, is all that is left.
        ba_cva::IndexHedge index;
        index.remaining_maturity = 5.0;
        index.constituent_risk_weight = 0.05;
        index.notional = 0.5 * 10.0 * scva /
                         (0.7 * 0.05 * 5.0 * ba_cva::supervisory_discount_factor(5.0));
        EXPECT_NEAR(ba_cva::k_hedged(book, {index}), std::sqrt(0.75 * 10.0) * scva, 1e-10);
        EXPECT_LT(ba_cva::capital_full(book, {index}), ba_cva::capital_reduced(book));
        EXPECT_NEAR(ba_cva::hedge_correlation(ba_cva::HedgeRelation::LegallyRelated), 0.8, 1e-15);
    }

    // ── Standardised initial margin schedule (BCBS-IOSCO) ────────────────────

    TEST(ImSchedule, AppendixARates)
    {
        using im_schedule::AssetClass;
        using im_schedule::margin_rate;
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::Credit, 1.0), 0.02);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::Credit, 3.0), 0.05);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::Credit, 7.0), 0.10);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::InterestRate, 1.0), 0.01);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::InterestRate, 3.0), 0.02);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::InterestRate, 7.0), 0.04);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::ForeignExchange, 7.0), 0.06);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::Commodity, 0.5), 0.15);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::Equity, 0.5), 0.15);
        EXPECT_DOUBLE_EQ(margin_rate(AssetClass::Other, 0.5), 0.15);
        EXPECT_THROW(margin_rate(AssetClass::Credit, -1.0), InvalidInput);
    }

    TEST(ImSchedule, NetMarginIsBetween40And100PercentOfGross)
    {
        using im_schedule::AssetClass;
        // A 10-year payer and a 10-year receiver swap of 100 each, worth +5
        // and -3: gross IM 8, net replacement cost 2 over gross 5.
        const std::vector<im_schedule::Trade> trades = {
            {AssetClass::InterestRate, 100.0, 10.0, 5.0},
            {AssetClass::InterestRate, 100.0, 10.0, -3.0},
        };
        EXPECT_NEAR(im_schedule::gross_initial_margin(trades), 8.0, 1e-15);
        EXPECT_NEAR(im_schedule::net_to_gross_ratio(trades), 0.4, 1e-15);
        EXPECT_NEAR(im_schedule::net_initial_margin(trades), (0.4 + 0.6 * 0.4) * 8.0, 1e-15);

        // The schedule ignores direction: two offsetting swaps still pay at
        // least 40 % of the gross amount. That is its conservatism.
        EXPECT_NEAR(im_schedule::net_initial_margin(8.0, 0.0), 3.2, 1e-15);
        EXPECT_NEAR(im_schedule::net_initial_margin(8.0, 1.0), 8.0, 1e-15);
        for (Real ngr = 0.0; ngr <= 1.0; ngr += 0.125)
        {
            const Real net = im_schedule::net_initial_margin(8.0, ngr);
            EXPECT_GE(net, 0.4 * 8.0);
            EXPECT_LE(net, 8.0);
        }
        EXPECT_THROW(im_schedule::net_initial_margin(8.0, 1.5), InvalidInput);
    }

    TEST(ImSchedule, NetToGrossRatioEdgeCases)
    {
        using im_schedule::AssetClass;
        // Net value negative: the ratio is floored at zero.
        EXPECT_DOUBLE_EQ(im_schedule::net_to_gross_ratio({{AssetClass::Equity, 10.0, 1.0, 2.0},
                                                          {AssetClass::Equity, 10.0, 1.0, -9.0}}),
                         0.0);
        // No positive value at all: 0/0, taken as 1 (no netting benefit).
        EXPECT_DOUBLE_EQ(im_schedule::net_to_gross_ratio({{AssetClass::Equity, 10.0, 1.0, -2.0}}),
                         1.0);
        EXPECT_DOUBLE_EQ(im_schedule::net_to_gross_ratio({}), 1.0);
        // A single trade in the money: no netting to recognise.
        EXPECT_DOUBLE_EQ(im_schedule::net_to_gross_ratio({{AssetClass::Equity, 10.0, 1.0, 2.0}}),
                         1.0);
    }

} // namespace quantModeling
