#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"

#include <Eigen/Core>
#include <cmath>
#include <span>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using aad::Number;
        using aad::Tape;

        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

        /// An equally-weighted basket call on n assets, one event date.
        template <class T>
        struct BasketCallT final : ISimulatableProduct<T>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};
            std::size_t n_;

            BasketCallT(Real k, Real maturity, std::size_t n)
                : K(k), tl_{maturity}, dl_(1), n_(n)
            {
            }
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return n_; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::max;
                T avg = p[0].spots[0];
                for (std::size_t i = 1; i < n_; ++i)
                    avg = avg + p[0].spots[i];
                avg = avg / static_cast<double>(n_);
                out.assign(1, max(avg - K, 0.0) / p[0].numeraire);
            }
        };

        Eigen::MatrixXd corr2(double rho)
        {
            Eigen::MatrixXd c(2, 2);
            c << 1.0, rho, rho, 1.0;
            return c;
        }
    } // namespace

    // ── construction validates its inputs ───────────────────────────────────

    TEST(MultiAssetBSSimModel, RejectsMismatchedVectorLengths)
    {
        EXPECT_THROW((MultiAssetBSSimModel<Real>{
                         {100.0, 100.0}, 0.03, {0.0}, {0.2, 0.2}, corr2(0.0)}),
                    InvalidInput);
    }

    TEST(MultiAssetBSSimModel, RejectsEmptyAssetList)
    {
        EXPECT_THROW((MultiAssetBSSimModel<Real>{{}, 0.03, {}, {}, Eigen::MatrixXd(0, 0)}),
                    InvalidInput);
    }

    TEST(MultiAssetBSSimModel, RejectsCorrelationSizeMismatch)
    {
        EXPECT_THROW((MultiAssetBSSimModel<Real>{
                         {100.0, 100.0}, 0.03, {0.0, 0.0}, {0.2, 0.2}, corr2(0.0).topLeftCorner(1, 1)}),
                    InvalidInput);
    }

    // ── correlation actually does something: two identical assets with
    // rho = 1 must move in lockstep, exactly, on every path ────────────────

    TEST(MultiAssetBSSimModel, PerfectCorrelationMovesTwoIdenticalAssetsInLockstep)
    {
        MultiAssetBSSimModel<Real> model{{100.0, 100.0}, 0.03, {0.0, 0.0},
                                         {0.2, 0.25}, corr2(1.0)};
        TimeLine tl{0.5, 1.0};
        std::vector<SampleDef> dl(2);
        model.init(tl, dl);

        Scenario<Real> path;
        allocate_scenario(path, dl, 2);
        const std::vector<double> gauss = {0.3, -0.7, 1.1, 0.2};
        model.generate_path(std::span<const double>(gauss), path);

        // rho = 1 with a *shared* mixing matrix means both assets draw the
        // same underlying gaussian z: the two paths differ only through
        // their own vol, not through independent randomness.
        for (const Sample<Real> &s : path)
            EXPECT_NE(s.spots[0], s.spots[1]); // different vols -> different levels
        // but a sanity check that they are NOT independent: both must be on
        // the same side of their respective starting points every time
        // (impossible to guarantee with independent draws at these z's).
        for (const Sample<Real> &s : path)
            EXPECT_EQ(s.spots[0] > 100.0, s.spots[1] > 100.0);
    }

    TEST(MultiAssetBSSimModel, PerfectCorrelationAndEqualVolsGivesIdenticalPaths)
    {
        MultiAssetBSSimModel<Real> model{{100.0, 100.0}, 0.03, {0.0, 0.0},
                                         {0.2, 0.2}, corr2(1.0)};
        TimeLine tl{0.5, 1.0};
        std::vector<SampleDef> dl(2);
        model.init(tl, dl);

        Scenario<Real> path;
        allocate_scenario(path, dl, 2);
        const std::vector<double> gauss = {0.3, -0.7, 1.1, 0.2};
        model.generate_path(std::span<const double>(gauss), path);

        for (const Sample<Real> &s : path)
            EXPECT_DOUBLE_EQ(s.spots[0], s.spots[1]);
    }

    // ── the AAD trap, closed: a clone's parameters() point at the clone's
    // own storage, not the original's ───────────────────────────────────────

    TEST(MultiAssetBSSimModel, CloneKeepsItsOwnParameterPointers)
    {
        MultiAssetBSSimModel<Real> model{{100.0, 90.0}, 0.03, {0.0, 0.01},
                                         {0.2, 0.25}, corr2(0.3)};
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── AAD delta of a two-asset basket call matches a common-random-number
    // bump on the same path ─────────────────────────────────────────────────

    TEST(MultiAssetBSSimModel, BasketDeltaMatchesCommonRandomNumberBump)
    {
        const Real r = 0.03, q0 = 0.0, q1 = 0.01, sigma0 = 0.2, sigma1 = 0.25,
                   K = 100.0, Tm = 1.0, rho = 0.4;
        const std::vector<double> z = {0.4, -0.6};
        const double h = 1e-4;

        BasketCallT<Number> product(K, Tm, 2);

        auto price_for_spot0 = [&](double s0_value)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            MultiAssetBSSimModel<Number> m{
                {Number(s0_value), Number(100.0)},
                Number(r),
                {Number(q0), Number(q1)},
                {Number(sigma0), Number(sigma1)},
                corr2(rho)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump_delta =
            (price_for_spot0(100.0 + h) - price_for_spot0(100.0 - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number s0_0(100.0);
        MultiAssetBSSimModel<Number> model{
            {s0_0, Number(100.0)},
            Number(r),
            {Number(q0), Number(q1)},
            {Number(sigma0), Number(sigma1)},
            corr2(rho)};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(s0_0.adjoint(), bump_delta, 1e-6);
    }

    // ── the full engine: simulate_aad over many paths gives sensible,
    // low-noise deltas for both assets and a positive vega for both vols ────

    TEST(MultiAssetBSSimModel, SimulateAadGivesSensibleRisksForABasketCall)
    {
        Tape tape;
        TapeSwitch guard(tape);

        BasketCallT<Number> product(100.0, 1.0, 2);
        MultiAssetBSSimModel<Number> model{
            {Number(100.0), Number(100.0)},
            Number(0.03),
            {Number(0.0), Number(0.0)},
            {Number(0.2), Number(0.2)},
            corr2(0.3)};

        const AADSimulResults res = simulate_aad(product, model, 100000, 11);

        ASSERT_EQ(res.risk_labels.size(), 7u);
        EXPECT_EQ(res.risk_labels[0], "rate");
        EXPECT_EQ(res.risk_labels[1], "spot[0]");
        EXPECT_EQ(res.risk_labels[2], "spot[1]");
        EXPECT_EQ(res.risk_labels[3], "div[0]");
        EXPECT_EQ(res.risk_labels[4], "div[1]");
        EXPECT_EQ(res.risk_labels[5], "vol[0]");
        EXPECT_EQ(res.risk_labels[6], "vol[1]");

        // deltas: positive, roughly equal by symmetry (identical assets)
        EXPECT_GT(res.risks[1], 0.0);
        EXPECT_GT(res.risks[2], 0.0);
        EXPECT_NEAR(res.risks[1], res.risks[2], 8.0 * (res.risk_std_errors[1] +
                                                       res.risk_std_errors[2]));
        // vega: positive for both assets
        EXPECT_GT(res.risks[5], 0.0);
        EXPECT_GT(res.risks[6], 0.0);
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
