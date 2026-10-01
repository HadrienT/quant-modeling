#include <gtest/gtest.h>

#include "quantModeling/risk/regulatory/sa_ccr.hpp"

#include <cmath>
#include <vector>

// The worked examples of BCBS 279 (March 2014), Annex 4a and 4b — reproduced
// in the Basel framework as CRE99.20 onwards. Amounts are in thousands; the
// text carries unrounded intermediate values and prints rounded ones, hence
// the half-unit tolerances.

namespace quantModeling::sa_ccr
{
    namespace
    {
        Trade rate_swap(const std::string &currency, Real notional, Time maturity, bool payer,
                        Real market_value)
        {
            Trade t;
            t.subclass = SubClass::InterestRate;
            t.hedging_set = currency;
            t.notional = notional;
            // Paying fixed is being long the floating rate.
            t.long_primary_risk_factor = payer;
            t.maturity = maturity;
            t.end = maturity;
            t.market_value = market_value;
            return t;
        }

        Trade cds(const std::string &entity, SubClass rating, Real notional, Time maturity,
                  bool protection_buyer, Real market_value)
        {
            Trade t;
            t.subclass = rating;
            t.reference = entity;
            t.notional = notional;
            t.long_primary_risk_factor = protection_buyer;
            t.maturity = maturity;
            t.end = maturity;
            t.market_value = market_value;
            return t;
        }

        Trade commodity_forward(const std::string &hedging_set, const std::string &type,
                                SubClass subclass, Real notional, Time maturity, bool is_long,
                                Real market_value)
        {
            Trade t;
            t.subclass = subclass;
            t.hedging_set = hedging_set;
            t.reference = type;
            t.notional = notional;
            t.long_primary_risk_factor = is_long;
            t.maturity = maturity;
            t.market_value = market_value;
            return t;
        }

        Trade fx_forward(const std::string &pair, Real notional, Time maturity, bool is_long)
        {
            Trade t;
            t.subclass = SubClass::ForeignExchange;
            t.hedging_set = pair;
            t.notional = notional;
            t.long_primary_risk_factor = is_long;
            t.maturity = maturity;
            return t;
        }

        /// Example 1: two USD swaps and a bought EUR 1Y-into-10Y swaption to
        /// receive fixed.
        std::vector<Trade> example1_trades()
        {
            Trade swaption;
            swaption.subclass = SubClass::InterestRate;
            swaption.hedging_set = "EUR";
            swaption.notional = 5000.0;
            // Physically settled: active until the underlying swap ends.
            swaption.maturity = 11.0;
            swaption.start = 1.0;
            swaption.end = 11.0;
            swaption.market_value = 50.0;
            // Receiving fixed is short the rate: a bought put on the swap
            // rate, forward 6 %, strike 5 %.
            swaption.option =
                OptionTerms{OptionType::Put, OptionSide::Bought, 0.06, 0.05, 1.0, 0.0};
            return {rate_swap("USD", 10000.0, 10.0, true, 30.0),
                    rate_swap("USD", 10000.0, 4.0, false, -20.0), swaption};
        }

        /// Example 2: three credit default swaps.
        std::vector<Trade> example2_trades()
        {
            return {cds("Firm A", SubClass::CreditAA, 10000.0, 3.0, true, 20.0),
                    cds("Firm B", SubClass::CreditBBB, 10000.0, 6.0, false, -40.0),
                    cds("CDX.IG 5y", SubClass::CreditIndexIG, 10000.0, 5.0, true, 0.0)};
        }

        /// Example 3: three commodity forwards.
        std::vector<Trade> example3_trades()
        {
            return {commodity_forward("Energy", "Crude Oil", SubClass::CommodityOilGas, 10000.0,
                                      0.75, true, -50.0),
                    commodity_forward("Energy", "Crude Oil", SubClass::CommodityOilGas, 20000.0,
                                      2.0, false, -30.0),
                    commodity_forward("Metals", "Silver", SubClass::CommodityMetals, 10000.0, 5.0,
                                      true, 100.0)};
        }

        std::vector<Trade> concatenate(std::vector<Trade> a, const std::vector<Trade> &b)
        {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        }

        /// A netting set worth `value`, to test the replacement cost alone.
        NettingSet valued(Real value, Real collateral, const MarginAgreement &margin)
        {
            NettingSet ns;
            ns.trades = {rate_swap("EUR", 1000.0, 5.0, true, value)};
            ns.collateral = collateral;
            ns.margin = margin;
            return ns;
        }
    } // namespace

    // ── BCBS 279, Annex 4a ───────────────────────────────────────────────────

    TEST(SaCcrExamples, Example1InterestRates)
    {
        const std::vector<Trade> trades = example1_trades();
        EXPECT_NEAR(supervisory_duration(0.0, 10.0), 7.87, 0.005);
        EXPECT_NEAR(supervisory_duration(0.0, 4.0), 3.63, 0.005);
        EXPECT_NEAR(supervisory_duration(1.0, 11.0), 7.49, 0.005);
        EXPECT_NEAR(adjusted_notional(trades[0]), 78694.0, 0.5);
        EXPECT_NEAR(adjusted_notional(trades[1]), 36254.0, 0.5);
        EXPECT_NEAR(adjusted_notional(trades[2]), 37428.0, 0.5);
        EXPECT_DOUBLE_EQ(supervisory_delta(trades[0]), 1.0);
        EXPECT_DOUBLE_EQ(supervisory_delta(trades[1]), -1.0);
        EXPECT_NEAR(supervisory_delta(trades[2]), -0.2694, 5e-5); // CRE99: -0.2694

        NettingSet ns;
        ns.trades = trades;
        const Exposure e = exposure_at_default(ns);
        EXPECT_DOUBLE_EQ(e.replacement_cost, 60.0);
        EXPECT_DOUBLE_EQ(e.multiplier, 1.0);
        // 0.5 % × (59,270 + 10,083).
        EXPECT_NEAR(e.add_ons.interest_rate / 0.005, 59270.0 + 10083.0, 1.0);
        EXPECT_NEAR(e.add_ons.interest_rate, 347.0, 0.5);
        EXPECT_DOUBLE_EQ(e.add_ons.aggregate(), e.add_ons.interest_rate);
        EXPECT_NEAR(e.ead, 569.0, 0.5);
        EXPECT_FALSE(e.capped_at_unmargined);
    }

    TEST(SaCcrExamples, Example2Credit)
    {
        const std::vector<Trade> trades = example2_trades();
        EXPECT_NEAR(adjusted_notional(trades[0]), 27858.0, 0.5);
        EXPECT_NEAR(adjusted_notional(trades[1]), 51836.0, 0.5);
        EXPECT_NEAR(adjusted_notional(trades[2]), 44240.0, 0.5);

        NettingSet ns;
        ns.trades = trades;
        const Exposure e = exposure_at_default(ns);
        EXPECT_DOUBLE_EQ(e.replacement_cost, 0.0); // V = -20
        // sqrt(47.5² + 77,344).
        EXPECT_NEAR(e.add_ons.credit, 282.0, 0.5);
        // Out of the money: the multiplier is activated.
        EXPECT_NEAR(e.multiplier, 0.965, 5e-4);
        EXPECT_NEAR(e.ead, 381.0, 0.5);
    }

    TEST(SaCcrExamples, Example3Commodities)
    {
        NettingSet ns;
        ns.trades = example3_trades();
        const Exposure e = exposure_at_default(ns);
        EXPECT_DOUBLE_EQ(e.replacement_cost, 20.0);
        EXPECT_DOUBLE_EQ(e.multiplier, 1.0);
        // Energy 2,041 (WTI and Brent net inside "Crude Oil", the nine-month
        // forward scaled by sqrt(9/12)) plus Metals 1,800.
        EXPECT_NEAR(e.add_ons.commodity, 3841.0, 0.5);
        EXPECT_NEAR(e.ead, 5406.0, 0.5);
    }

    TEST(SaCcrExamples, Example4TwoAssetClassesAddUp)
    {
        NettingSet ns;
        ns.trades = concatenate(example1_trades(), example2_trades());
        const Exposure e = exposure_at_default(ns);
        EXPECT_DOUBLE_EQ(e.replacement_cost, 40.0);
        EXPECT_NEAR(e.add_ons.aggregate(), 629.0, 0.5); // 347 + 282, no diversification
        EXPECT_DOUBLE_EQ(e.multiplier, 1.0);
        EXPECT_NEAR(e.ead, 936.0, 0.5);
    }

    TEST(SaCcrExamples, Example5MarginedNettingSet)
    {
        NettingSet ns;
        ns.trades = concatenate(example1_trades(), example3_trades());
        ns.collateral = 200.0; // 50 of variation margin and 150 of independent amount
        MarginAgreement margin;
        margin.threshold = 0.0;
        margin.minimum_transfer_amount = 5.0;
        margin.nica = 150.0;
        // Weekly re-margining: 10 + 5 - 1 = 14 business days.
        margin.margin_period_of_risk = margin_period_of_risk_floor(5);
        ns.margin = margin;
        EXPECT_NEAR(margin.margin_period_of_risk, 14.0 / 250.0, 1e-15);

        const Exposure e = exposure_at_default(ns);
        // max(80 - 200, 0 + 5 - 150, 0).
        EXPECT_DOUBLE_EQ(e.replacement_cost, 0.0);
        EXPECT_NEAR(e.add_ons.interest_rate, 123.0, 0.5);
        EXPECT_NEAR(e.add_ons.commodity, 1278.0, 0.5);
        EXPECT_NEAR(e.add_ons.aggregate(), 1401.0, 0.5);
        EXPECT_NEAR(e.multiplier, 0.958, 5e-4);
        EXPECT_NEAR(e.ead, 1879.0, 0.5);
        EXPECT_FALSE(e.capped_at_unmargined);
    }

    // ── BCBS 279, Annex 4b: the margined replacement cost ────────────────────

    TEST(SaCcrReplacementCost, Annex4bExamples)
    {
        MarginAgreement m;
        // 1: value 80 fully covered by VM, plus an independent amount of 10.
        m = {0.0, 1.0, 10.0};
        EXPECT_DOUBLE_EQ(replacement_cost(valued(80.0, 90.0, m)), 0.0);
        // 2: VM of 79.5 held; 10 of independent collateral held and 10 posted
        // unsegregated net to zero. The MTA of 1 is what can build up unmargined.
        m = {0.0, 1.0, 0.0};
        EXPECT_DOUBLE_EQ(replacement_cost(valued(80.0, 79.5, m)), 1.0);
        // 3: clearing member, VM posted, IM held bankruptcy-remote by the CCP.
        m = {0.0, 0.0, 0.0};
        EXPECT_DOUBLE_EQ(replacement_cost(valued(-50.0, -50.0, m)), 0.0);
        // 4: same, but the IM of 10 is not bankruptcy-remote: it is at risk.
        m = {0.0, 0.0, -10.0};
        EXPECT_DOUBLE_EQ(replacement_cost(valued(-50.0, -60.0, m)), 10.0);
        // 5: maintenance margin of 140 % of a value of 50; 80 posted.
        m = {0.0, 0.0, 20.0};
        EXPECT_DOUBLE_EQ(replacement_cost(valued(50.0, 80.0, m)), 0.0);
    }

    TEST(SaCcrReplacementCost, UnmarginedIsCurrentExposure)
    {
        NettingSet ns;
        ns.trades = {rate_swap("EUR", 1000.0, 5.0, true, 30.0)};
        EXPECT_DOUBLE_EQ(replacement_cost(ns), 30.0);
        ns.collateral = 10.0;
        EXPECT_DOUBLE_EQ(replacement_cost(ns), 20.0);
        ns.collateral = 45.0; // excess collateral cannot make it negative
        EXPECT_DOUBLE_EQ(replacement_cost(ns), 0.0);
        ns.collateral = -15.0; // collateral posted by the bank is at risk too
        EXPECT_DOUBLE_EQ(replacement_cost(ns), 45.0);
    }

    // ── Table 2 and the building blocks ──────────────────────────────────────

    TEST(SaCcrParameters, Table2)
    {
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::InterestRate).factor, 0.005);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::InterestRate).option_volatility, 0.5);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::ForeignExchange).factor, 0.04);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::ForeignExchange).option_volatility, 0.15);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditAAA).factor, 0.0038);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditA).factor, 0.0042);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditBBB).factor, 0.0054);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditBB).factor, 0.0106);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditB).factor, 0.016);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditCCC).factor, 0.06);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditCCC).correlation, 0.5);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditIndexSG).factor, 0.0106);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CreditIndexSG).correlation, 0.8);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::EquitySingleName).factor, 0.32);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::EquitySingleName).option_volatility, 1.2);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::EquityIndex).factor, 0.20);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::EquityIndex).correlation, 0.8);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CommodityElectricity).factor, 0.40);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CommodityElectricity).option_volatility,
                         1.5);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CommodityOther).factor, 0.18);
        EXPECT_DOUBLE_EQ(supervisory_parameters(SubClass::CommodityMetals).correlation, 0.4);
        EXPECT_EQ(asset_class(SubClass::CreditIndexIG), AssetClass::Credit);
        EXPECT_EQ(asset_class(SubClass::CommodityAgricultural), AssetClass::Commodity);
    }

    TEST(SaCcrBuildingBlocks, SupervisoryDuration)
    {
        // Close to E - S for short periods, below it for long ones (5 % discounting).
        EXPECT_NEAR(supervisory_duration(0.0, 1.0), 20.0 * (1.0 - std::exp(-0.05)), 1e-15);
        EXPECT_LT(supervisory_duration(0.0, 30.0), 30.0);
        EXPECT_LT(supervisory_duration(5.0, 15.0), supervisory_duration(0.0, 10.0));
        // Floored at ten business days.
        EXPECT_DOUBLE_EQ(supervisory_duration(0.0, 0.001), 10.0 / 250.0);
        EXPECT_THROW(supervisory_duration(2.0, 1.0), InvalidInput);
    }

    TEST(SaCcrBuildingBlocks, MaturityFactors)
    {
        EXPECT_DOUBLE_EQ(maturity_factor_unmargined(3.0), 1.0); // capped at one year
        EXPECT_DOUBLE_EQ(maturity_factor_unmargined(0.25), 0.5);
        EXPECT_DOUBLE_EQ(maturity_factor_unmargined(0.0), std::sqrt(10.0 / 250.0)); // floor
        // Daily margining, ten business days: 1.5 × sqrt(10/250) = 0.3.
        EXPECT_NEAR(maturity_factor_margined(margin_period_of_risk_floor(1)), 0.3, 1e-15);
        EXPECT_NEAR(margin_period_of_risk_floor(1, 20), 20.0 / 250.0, 1e-15);
        EXPECT_THROW(maturity_factor_margined(0.0), InvalidInput);
    }

    TEST(SaCcrBuildingBlocks, OptionDeltasObeyPutCallParity)
    {
        Trade t = rate_swap("USD", 1000.0, 6.0, true, 0.0);
        const auto delta = [&](OptionType type, OptionSide side, Real price, Real strike)
        {
            t.option = OptionTerms{type, side, price, strike, 2.0, 0.0};
            return supervisory_delta(t);
        };
        for (const Real strike : {0.01, 0.03, 0.05})
        {
            const Real call = delta(OptionType::Call, OptionSide::Bought, 0.03, strike);
            const Real put = delta(OptionType::Put, OptionSide::Bought, 0.03, strike);
            // Bought call + sold put is a forward: deltas add up to one.
            EXPECT_NEAR(call - put, 1.0, 1e-15);
            EXPECT_GT(call, 0.0);
            EXPECT_LT(put, 0.0);
            EXPECT_DOUBLE_EQ(delta(OptionType::Call, OptionSide::Sold, 0.03, strike), -call);
            EXPECT_DOUBLE_EQ(delta(OptionType::Put, OptionSide::Sold, 0.03, strike), -put);
        }
        // Deep in the money a call behaves like the underlying.
        EXPECT_NEAR(delta(OptionType::Call, OptionSide::Bought, 0.03, 0.00001), 1.0, 1e-9);
        // Negative rates need the shift of the CRE52.40 FAQ.
        t.option = OptionTerms{OptionType::Call, OptionSide::Bought, -0.002, 0.001, 2.0, 0.0};
        EXPECT_THROW(supervisory_delta(t), InvalidInput);
        t.option->shift = 0.01;
        EXPECT_GT(supervisory_delta(t), 0.0);
        EXPECT_LT(supervisory_delta(t), 1.0);
    }

    TEST(SaCcrBuildingBlocks, Multiplier)
    {
        // Under-collateralised: the whole add-on counts.
        EXPECT_DOUBLE_EQ(multiplier(0.0, 100.0), 1.0);
        EXPECT_DOUBLE_EQ(multiplier(50.0, 100.0), 1.0);
        // Over-collateralised: decreasing, never below the 5 % floor.
        Real previous = 1.0;
        for (const Real excess : {10.0, 50.0, 100.0, 500.0, 5000.0})
        {
            const Real m = multiplier(-excess, 100.0);
            EXPECT_LT(m, previous);
            EXPECT_GT(m, multiplier_floor);
            previous = m;
        }
        EXPECT_NEAR(multiplier(-1e9, 100.0), multiplier_floor, 1e-12);
        EXPECT_DOUBLE_EQ(multiplier(-10.0, 0.0), 1.0);
        EXPECT_THROW(multiplier(0.0, -1.0), InvalidInput);
    }

    // ── Offsetting rules ─────────────────────────────────────────────────────

    TEST(SaCcrHedgingSets, InterestRateOffsetsInsideABucketAndPartlyAcross)
    {
        // Payer and receiver of the same size in the same bucket: nothing left.
        EXPECT_NEAR(add_ons({rate_swap("USD", 1000.0, 10.0, true, 0.0),
                             rate_swap("USD", 1000.0, 10.0, false, 0.0)})
                        .interest_rate,
                    0.0, 1e-12);

        const Real long_10y = add_ons({rate_swap("USD", 1000.0, 10.0, true, 0.0)}).interest_rate;
        const Real short_4y = add_ons({rate_swap("USD", 1000.0, 4.0, false, 0.0)}).interest_rate;
        const Real both = add_ons({rate_swap("USD", 1000.0, 10.0, true, 0.0),
                                   rate_swap("USD", 1000.0, 4.0, false, 0.0)})
                              .interest_rate;
        // Different buckets: a partial offset (70 % correlation between
        // adjacent buckets).
        EXPECT_LT(both, long_10y + short_4y);
        EXPECT_GT(both, long_10y - short_4y);
        EXPECT_NEAR(both * both,
                    long_10y * long_10y + short_4y * short_4y - 1.4 * long_10y * short_4y, 1e-9);

        // Different currencies: no offset at all.
        const Real two_currencies = add_ons({rate_swap("USD", 1000.0, 10.0, true, 0.0),
                                             rate_swap("EUR", 1000.0, 10.0, false, 0.0)})
                                        .interest_rate;
        EXPECT_NEAR(two_currencies, 2.0 * long_10y, 1e-12);
    }

    TEST(SaCcrHedgingSets, MaturityBucketsAreBelowOneOneToFiveAndAboveFive)
    {
        // D1 = 1 and D3 = -1 only offset at 30 %: sqrt(1 + 1 - 0.6).
        Trade short_end = rate_swap("USD", 1.0, 0.9, true, 0.0);
        Trade long_end = rate_swap("USD", 1.0, 5.5, false, 0.0);
        const Real d1 = adjusted_notional(short_end) * maturity_factor_unmargined(0.9);
        const Real d3 = adjusted_notional(long_end);
        EXPECT_NEAR(add_ons({short_end, long_end}).interest_rate,
                    0.005 * std::sqrt(d1 * d1 + d3 * d3 - 0.6 * d1 * d3), 1e-15);
        // Exactly five years is still the middle bucket.
        Trade five = rate_swap("USD", 1.0, 5.0, false, 0.0);
        const Real d2 = adjusted_notional(five);
        EXPECT_NEAR(add_ons({short_end, five}).interest_rate,
                    0.005 * std::sqrt(d1 * d1 + d2 * d2 - 1.4 * d1 * d2), 1e-15);
    }

    TEST(SaCcrHedgingSets, ForeignExchangeNetsInsideACurrencyPair)
    {
        const Real one = add_ons({fx_forward("EURUSD", 1000.0, 2.0, true)}).foreign_exchange;
        EXPECT_NEAR(one, 0.04 * 1000.0, 1e-12);
        EXPECT_NEAR(add_ons({fx_forward("EURUSD", 1000.0, 2.0, true),
                             fx_forward("EURUSD", 400.0, 3.0, false)})
                        .foreign_exchange,
                    0.04 * 600.0, 1e-12);
        EXPECT_NEAR(add_ons({fx_forward("EURUSD", 1000.0, 2.0, true),
                             fx_forward("USDJPY", 400.0, 3.0, false)})
                        .foreign_exchange,
                    0.04 * 1400.0, 1e-12);
    }

    TEST(SaCcrHedgingSets, SingleFactorAggregationRewardsHedgesAndDiversification)
    {
        const auto equity = [](const std::string &name, Real notional, bool is_long)
        {
            Trade t;
            t.subclass = SubClass::EquitySingleName;
            t.reference = name;
            t.notional = notional;
            t.long_primary_risk_factor = is_long;
            t.maturity = 2.0;
            return t;
        };
        const Real one = add_ons({equity("A", 100.0, true)}).equity;
        EXPECT_NEAR(one, 0.32 * 100.0, 1e-12);
        // Same name, opposite directions: full offset.
        EXPECT_NEAR(add_ons({equity("A", 100.0, true), equity("A", 100.0, false)}).equity, 0.0,
                    1e-12);
        // Two names, same direction: rho = 50 % each, so the pair correlation
        // is 25 %: sqrt(2 + 2 × 0.25) = 1.58 times one, not 2.
        EXPECT_NEAR(add_ons({equity("A", 100.0, true), equity("B", 100.0, true)}).equity,
                    one * std::sqrt(2.5), 1e-12);
        // Two names, opposite directions: an imperfect hedge, sqrt(2 - 0.5).
        EXPECT_NEAR(add_ons({equity("A", 100.0, true), equity("B", 100.0, false)}).equity,
                    one * std::sqrt(1.5), 1e-12);

        Trade inconsistent = equity("A", 100.0, true);
        inconsistent.subclass = SubClass::EquityIndex;
        EXPECT_THROW(add_ons({equity("A", 100.0, true), inconsistent}), InvalidInput);
    }

    TEST(SaCcrHedgingSets, BasisAndVolatilityTransactionsAreSetApart)
    {
        const Trade regular = rate_swap("USD", 1000.0, 10.0, true, 0.0);
        Trade basis = rate_swap("USD", 1000.0, 10.0, false, 0.0);
        basis.kind = HedgingSetKind::Basis;
        Trade volatility = basis;
        volatility.kind = HedgingSetKind::Volatility;

        const Real base = add_ons({regular}).interest_rate;
        EXPECT_NEAR(add_ons({basis}).interest_rate, 0.5 * base, 1e-12);
        EXPECT_NEAR(add_ons({volatility}).interest_rate, 5.0 * base, 1e-12);
        // No offset against the regular hedging set of the same currency.
        EXPECT_NEAR(add_ons({regular, basis}).interest_rate, 1.5 * base, 1e-12);
    }

    // ── The whole calculation ────────────────────────────────────────────────

    TEST(SaCcrExposure, MarginedExposureIsCappedAtTheUnmarginedOne)
    {
        // A threshold so large that it would never be hit: the margined
        // formula would report it as replacement cost.
        NettingSet ns;
        ns.trades = example1_trades();
        const Exposure unmargined = exposure_at_default(ns);

        MarginAgreement margin;
        margin.threshold = 1.0e6;
        ns.margin = margin;
        EXPECT_DOUBLE_EQ(replacement_cost(ns), 1.0e6);
        const Exposure capped = exposure_at_default(ns);
        EXPECT_TRUE(capped.capped_at_unmargined);
        EXPECT_DOUBLE_EQ(capped.ead, unmargined.ead);
        EXPECT_DOUBLE_EQ(capped.replacement_cost, unmargined.replacement_cost);

        // With a zero threshold the margin agreement does reduce the exposure.
        ns.margin->threshold = 0.0;
        ns.collateral = 60.0; // variation margin received in full
        const Exposure margined = exposure_at_default(ns);
        EXPECT_FALSE(margined.capped_at_unmargined);
        EXPECT_LT(margined.ead, unmargined.ead);
    }

    TEST(SaCcrExposure, IsHomogeneousInTheSizeOfTheBook)
    {
        NettingSet ns;
        ns.trades = concatenate(example1_trades(), example2_trades());
        ns.collateral = 15.0;
        NettingSet scaled = ns;
        for (Trade &t : scaled.trades)
        {
            t.notional *= 3.0;
            t.market_value *= 3.0;
        }
        scaled.collateral *= 3.0;
        const Exposure a = exposure_at_default(ns);
        const Exposure b = exposure_at_default(scaled);
        EXPECT_NEAR(b.ead, 3.0 * a.ead, 1e-9);
        EXPECT_NEAR(b.multiplier, a.multiplier, 1e-14);
    }

    TEST(SaCcrExposure, EmptyNettingSetHasNoExposure)
    {
        const Exposure e = exposure_at_default(NettingSet{});
        EXPECT_DOUBLE_EQ(e.ead, 0.0);
        EXPECT_DOUBLE_EQ(e.pfe, 0.0);
    }

    TEST(SaCcrExposure, RejectsMalformedTrades)
    {
        NettingSet ns;
        ns.trades = {rate_swap("USD", 0.0, 5.0, true, 0.0)};
        EXPECT_THROW(exposure_at_default(ns), InvalidInput);
        ns.trades = {rate_swap("", 100.0, 5.0, true, 0.0)};
        EXPECT_THROW(exposure_at_default(ns), InvalidInput);
        ns.trades = {cds("", SubClass::CreditA, 100.0, 5.0, true, 0.0)};
        EXPECT_THROW(exposure_at_default(ns), InvalidInput);
        Trade backwards = rate_swap("USD", 100.0, 5.0, true, 0.0);
        backwards.start = 6.0;
        ns.trades = {backwards};
        EXPECT_THROW(exposure_at_default(ns), InvalidInput);
    }

} // namespace quantModeling::sa_ccr
