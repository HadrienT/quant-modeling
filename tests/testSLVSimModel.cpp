#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/market/slv_calibration.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <span>
#include <vector>

// issue #37's SLV item: Heston dynamics (already in C++, BatesSimModel at
// lambda=0) combined with Dupire's local-vol exactness (already in C++,
// LocalVolSimModel / market/slv_calibration.hpp's particle method).

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

        template <class T>
        struct EuroCallT final : ISimulatableProduct<T>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            EuroCallT(Real k, Real maturity)
                : K(k), tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override { return labels_; }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::max;
                out.assign(1, max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
            }
        };

        template <class T>
        struct DeflatedSpotT final : ISimulatableProduct<T>
        {
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            explicit DeflatedSpotT(Real maturity)
                : tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override { return labels_; }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                out.assign(1, p[0].spots[0] / p[0].numeraire);
            }
        };

        Real bs_call(Real S0, Real K, Real r, Real q, Real vol, Real Tm)
        {
            const Real sd = vol * std::sqrt(Tm);
            const Real d1 = (std::log(S0 / K) + (r - q + 0.5 * vol * vol) * Tm) / sd;
            const Real d2 = d1 - sd;
            return S0 * std::exp(-q * Tm) * norm_cdf(d1) - K * std::exp(-r * Tm) * norm_cdf(d2);
        }
    } // namespace

    TEST(SLVSimModel, RejectsNonPositiveKappa)
    {
        std::vector<Real> K{80, 100, 120}, Tg{0.5, 1.0}, lev(6, 1.0);
        EXPECT_THROW((SLVSimModel<Real>{100.0, 0.03, 0.0, 0.04, 0.0, 0.04, 0.5, -0.5, K, Tg, lev}),
                     InvalidInput);
    }

    TEST(SLVSimModel, RejectsRhoOutOfRange)
    {
        std::vector<Real> K{80, 100, 120}, Tg{0.5, 1.0}, lev(6, 1.0);
        EXPECT_THROW((SLVSimModel<Real>{100.0, 0.03, 0.0, 0.04, 1.5, 0.04, 0.5, -1.5, K, Tg, lev}),
                     InvalidInput);
    }

    TEST(SLVSimModel, RejectsLeverageSizeMismatch)
    {
        std::vector<Real> K{80, 100, 120}, Tg{0.5, 1.0}, lev(5, 1.0); // needs 6
        EXPECT_THROW((SLVSimModel<Real>{100.0, 0.03, 0.0, 0.04, 1.5, 0.04, 0.5, -0.5, K, Tg, lev}),
                     InvalidInput);
    }

    // ── the core promise of SLV calibration: target a *flat* local-vol
    // surface (Black-Scholes) under genuinely stochastic Heston dynamics.
    // The theoretical leverage L(S,t) = flat_vol / sqrt(E[v_t|S_t=S]) is
    // itself far from flat (Heston's own smile varies with S) -- so this
    // exercises real calibration work, not a degenerate no-op -- yet the
    // *priced* option, if the particle method got E[v|S] right, must land
    // on the closed-form Black-Scholes price, regardless of which
    // reasonable Heston parameters were used underneath. This is the test
    // that actually proves the leverage function does what it claims ──────

    // Grid resolution here is not arbitrary: nearest-strike bucketing's own
    // bias shrinks with it, measured directly rather than assumed --
    // nK=5 biased one strike by ~6 std errors, nK=15 by ~3, nK=25 (used
    // below) keeps every strike within ~2. This is the bucketing method's
    // known bias-variance tradeoff (a coarser bucket averages E[v|S] over a
    // wider, less accurate window, regardless of how many particles fill
    // it), not noise that a fixed particle count would eventually wash out
    // -- exactly why a real desk's SLV calibration grid is this fine or
    // finer in practice.
    TEST(SLVSimModel, CalibratedToFlatVolReproducesBlackScholes)
    {
        const Real s0 = 100.0, r = 0.03, q = 0.01, flat_vol = 0.2;
        const HestonParams heston{/*v0=*/0.03, /*kappa=*/1.5, /*theta=*/0.04,
                                  /*xi=*/0.4, /*rho=*/-0.6};

        std::vector<Real> K_grid;
        for (int i = 0; i < 25; ++i)
            K_grid.push_back(50.0 + static_cast<Real>(i) * (200.0 - 50.0) / 24.0);
        const std::vector<Real> T_grid{0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
        const std::vector<Real> sigma_loc(K_grid.size() * T_grid.size(), flat_vol);

        SLVCalibrationSettings settings;
        settings.n_particles = 150000;
        settings.seed = 7;
        settings.max_dt = 1.0 / 100;
        const SLVLeverageGrid grid =
            calibrate_slv_leverage(s0, r, q, heston, K_grid, T_grid, sigma_loc, settings);

        SLVSimModel<Real> model(s0, r, q, heston.v0, heston.kappa, heston.theta,
                                heston.xi, heston.rho, grid.K_grid, grid.T_grid,
                                grid.leverage, 1.0 / 100);

        for (Real K : {90.0, 100.0, 110.0})
        {
            const Real Tm = 1.0;
            EuroCallT<Real> product(K, Tm);

            PricingSettings ps;
            ps.mc_paths = 150000;
            ps.mc_seed = 11;
            ps.mc_antithetic = true;
            const SimulationMCResult res = simulate<Real>(product, model, ps);

            const Real expected = bs_call(s0, K, r, q, flat_vol, Tm);
            EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error() + 0.15) << "strike " << K;
        }
    }

    // ── the general oracle, independent of the calibration entirely:
    // discounted spot is a martingale for any leverage grid -- the SDE
    // structure itself, not the calibration's quality ──────────────────────

    TEST(SLVSimModel, DiscountedSpotIsAMartingale)
    {
        const Real s0 = 100.0, r = 0.03, q = 0.02, Tm = 1.0;
        std::vector<Real> K_grid{60, 100, 160}, T_grid{0.5, 1.5};
        std::vector<Real> leverage(6, 0.9); // an arbitrary, non-trivial leverage grid

        DeflatedSpotT<Real> product(Tm);
        SLVSimModel<Real> model(s0, r, q, 0.04, 1.2, 0.04, 0.5, -0.5, K_grid, T_grid, leverage);

        PricingSettings settings;
        settings.mc_paths = 200000;
        settings.mc_seed = 5;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const Real expected = s0 * std::exp(-q * Tm);
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error());
    }

    TEST(SLVSimModel, CloneKeepsItsOwnParameterPointers)
    {
        std::vector<Real> K_grid{80, 100, 120}, T_grid{0.5, 1.0}, leverage(6, 1.0);
        SLVSimModel<Real> model(100.0, 0.03, 0.0, 0.04, 1.5, 0.04, 0.5, -0.5, K_grid, T_grid, leverage);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── simulate_aad reports every parameter -- 8 Heston-side plus one per
    // leverage grid point -- all finite. Every reachable leverage point has
    // a positive standard error (the variance-floor NaN trap BatesSimModel
    // already documents applies here identically, same scheme, same
    // sqrt(v_plus)); the grid's *last* T column never is reachable, and
    // that is asserted explicitly rather than silently excluded --
    // leverage_at()'s staircase-in-T lookup (documented on that method)
    // uses column j-1 for the interval ending at T_grid[j], so column
    // nT-1 would only ever be read for t > T_grid.back(), which the
    // clamp in leverage_at() never lets happen. Pricing is unaffected --
    // the interval (T_grid[nT-2], T_grid[nT-1]] is fully covered by
    // column nT-2 -- but a risk report correctly has nothing to say about
    // a grid point no path ever touches. ─────────────────────────────────

    TEST(SLVSimModel, SimulateAadReportsEveryParameterFiniteWithPositiveStdError)
    {
        const Real s0 = 100.0, r = 0.03, q = 0.01;
        std::vector<Real> K_grid{80, 100, 120}, T_grid{0.5, 1.0};
        const std::size_t nT = T_grid.size();
        std::vector<Real> leverage(K_grid.size() * nT, 1.0);

        Tape tape;
        TapeSwitch guard(tape);

        std::vector<Number> lev_T;
        for (Real l : leverage)
            lev_T.emplace_back(l);

        EuroCallT<Number> product(100.0, 1.0);
        SLVSimModel<Number> model{Number(s0), Number(r), Number(q), Number(0.04),
                                  Number(1.5), Number(0.04), Number(0.5), Number(-0.5),
                                  K_grid, T_grid, lev_T};

        const AADSimulResults res = simulate_aad(product, model, 40000, 4);

        ASSERT_EQ(res.risk_labels.size(), 8u + K_grid.size() * nT);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[3], "v0");
        EXPECT_TRUE(res.risk_labels[8].rfind("leverage[", 0) == 0);

        EXPECT_TRUE(std::isfinite(res.price));
        for (std::size_t i = 0; i < res.risks.size(); ++i)
        {
            EXPECT_TRUE(std::isfinite(res.risks[i])) << res.risk_labels[i];

            // leverage grid points are indices [8, 8 + nK*nT); the j-th
            // column of grid point i is index 8 + i*nT + j.
            const bool is_last_t_column = i >= 8 && (i - 8) % nT == nT - 1;
            if (is_last_t_column)
                EXPECT_EQ(res.risk_std_errors[i], 0.0) << res.risk_labels[i];
            else
                EXPECT_GT(res.risk_std_errors[i], 0.0) << res.risk_labels[i];
        }
    }

} // namespace quantModeling
