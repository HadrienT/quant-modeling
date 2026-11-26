// The variance decay of the multilevel corrections, V_l = Var[P_l - P_{l-1}],
// for the payoffs Giles & Szpruch (2012, §3.2-3.5) treat with a conditional
// expectation: a digital, a barrier, a vanilla for reference. Each is written
// twice, with the hard payoff and with the library's conditional primitive
// (lastStepDigital, lastStepCall, survivalBelow, knockOutAbove) -- the latter run with each
// level on its own steps, then with the coarse path conditioned on the fine
// one (Giles & Szpruch's variants, WP 26 §26) -- and priced on fixed levels
// 0..L with the same number of paths each, so that every level's V_l and
// |E Y_l| are measured, not just the adaptive run's regression (issue #363).
//
//   qm_mlmc_conditional_bench [paths per level] [finest level]
//
// Local vol on a skewed 5 x 3 surface (tests/testMlmc.cpp's), level 0 at
// monthly steps (daily where the product observes daily), log-Euler, PCG32.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

namespace qm = quantModeling;

namespace
{
    qm::scripting::ScriptModelSpec skewed_local_vol()
    {
        qm::scripting::ScriptModelSpec s;
        s.model = "local_vol";
        s.spot = 100.0;
        s.rate = 0.05;
        s.dividend = 0.02;
        s.K_grid = {50.0, 80.0, 100.0, 120.0, 200.0};
        s.T_grid = {0.0, 0.5, 2.0};
        s.sigma_loc_flat = {0.80, 0.70, 0.60, 0.45, 0.40, 0.35, 0.25, 0.24, 0.22,
                            0.16, 0.15, 0.14, 0.10, 0.10, 0.10};
        s.max_dt = 1.0 / 12.0;
        // QM_BENCH_GRID (#371): a market grid instead, from a text file --
        // the dividend yield, nK nT, the strikes (scaled to a spot of 100:
        // local vol is homogeneous), the maturities, sigma_loc strike-major.
        if (const char *path = std::getenv("QM_BENCH_GRID"))
        {
            std::ifstream in(path);
            std::size_t nK = 0, nT = 0;
            in >> s.dividend >> nK >> nT;
            s.rate = 0.04;
            s.K_grid.resize(nK), s.T_grid.resize(nT), s.sigma_loc_flat.resize(nK * nT);
            for (double &k : s.K_grid)
                in >> k;
            for (double &t : s.T_grid)
                in >> t;
            for (double &v : s.sigma_loc_flat)
                in >> v;
            if (!in)
                throw std::runtime_error(std::string("QM_BENCH_GRID: cannot read ") + path);
        }
        return s;
    }

    struct Case
    {
        const char *product, *estimator, *script;
        int finest_cap; ///< daily observations make level 0 daily: fewer levels
    };

    // log2 slope of y over levels 1..L (the corrections), least squares.
    double slope(const std::vector<double> &y)
    {
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (std::size_t l = 1; l < y.size(); ++l)
        {
            if (!(y[l] > 0.0))
                continue;
            const double x = static_cast<double>(l), v = std::log2(y[l]);
            n += 1, sx += x, sy += v, sxx += x * x, sxy += x * v;
        }
        return n < 2 ? 0.0 : (n * sxy - sx * sy) / (n * sxx - sx * sx);
    }
} // namespace

int main(int argc, char **argv)
{
    const long long paths = argc > 1 ? std::atoll(argv[1]) : 100000;
    const int finest = argc > 2 ? std::atoi(argv[2]) : 6;
    // #371: the step between dates -- "euler" (log-Euler, the default),
    // "milstein" (every step), "before" (Milstein except the steps that
    // reach a product date, which stay lognormal) -- and a filter on the
    // products' names and estimators.
    const std::string scheme = argc > 3 ? argv[3] : "euler";
    const std::string only = argc > 4 ? argv[4] : "";
    const qm::ValuationContext ctx{qm::Date::from_iso("2025-12-31")};

    const Case cases[] = {
        {"vanilla call K=100", "hard", "2027-01-01\n    pays max(spot() - 100, 0)\n", 99},
        {"vanilla call K=100", "lastStepCall", "2027-01-01\n    pays lastStepCall(100)\n", 99},
        {"digital call K=100, pays 100", "hard", "2027-01-01\n    if spot() > 100 then pays 100 endIf\n", 99},
        {"digital call K=100, pays 100", "lastStepDigital", "2027-01-01\n    pays 100 * lastStepDigital(100)\n", 99},
        {"up-and-out call K=100 B=130, continuous", "survivalBelow (per step)",
         "2027-01-01\n    s = survivalBelow(130)\n    pays s * max(spot() - 100, 0)\n", 99},
        // fixed dates, as the library's (issue #363): a Lipschitz function
        // of the dates' spots, which both levels share
        {"Asian call K=100, 12 monthly fixings", "hard",
         "2026-01-30\n    acc = spot()\nschedule(2026-02-27, 2026-11-30, 1M, US, F)\n    acc = acc + spot()\n"
         "2027-01-01\n    acc = acc + spot()\n    pays max(acc / 12 - 100, 0)\n",
         99},
        {"fixed lookback call K=100, weekly closes", "hard",
         "2026-01-02\n    hi = spot()\nschedule(2026-01-09, 2026-12-25, 1W, US, F)\n    hi = max(hi, spot())\n"
         "2027-01-01\n    hi = max(hi, spot())\n    pays max(hi - 100, 0)\n",
         4},
        {"up-and-out call K=100 B=130, daily closes", "hard",
         "2026-01-01\n    alive = 1\nschedule(2026-01-02, 2026-12-31, 1D, US, F)\n"
         "    if spot() >= 130 then alive = 0 endIf\n2027-01-01\n"
         "    if spot() >= 130 then alive = 0 endIf\n    if alive = 1 then pays max(spot() - 100, 0) endIf\n",
         4},
        // the step into each close conditioned on survival (issue #369)
        {"up-and-out call K=100 B=130, daily closes", "knockOutAbove",
         "2026-01-01\n    k = 100\nschedule(2026-01-02, 2026-12-31, 1D, US, F)\n    knockOutAbove(130)\n"
         "2027-01-01\n    knockOutAbove(130)\n    pays max(spot() - k, 0)\n",
         4},
    };

    std::printf("Local vol (%s), %s, PCG32, %lld paths a level\n",
                std::getenv("QM_BENCH_GRID") ? std::getenv("QM_BENCH_GRID") : "skewed 5x3", scheme.c_str(), paths);
    for (const Case &c : cases)
        for (const bool conditioned : {false, true})
        {
            if (!only.empty() && (std::string(c.product) + " " + c.estimator).find(only) == std::string::npos)
                continue;
            // The coarse path conditioned on the fine one (WP 26 §26) changes
            // only the step quantities: the hard payoffs are run once.
            const bool hard = std::string(c.estimator) == "hard";
            if (hard && conditioned)
                continue;
            const int L = std::min(finest, c.finest_cap);
            const qm::ScriptedProduct<qm::Real> product(c.script, ctx);
            auto model = qm::scripting::make_script_model<qm::Real>(skewed_local_vol());
            auto *lv = dynamic_cast<qm::LocalVolSimModel<qm::Real> *>(model.get());
            lv->set_milstein(scheme == "milstein");
            lv->set_milstein_before_dates(scheme == "before");
            const std::vector<long long> n(static_cast<std::size_t>(L + 1), paths);
            qm::MlmcResult r;
            try
            {
                r = qm::simulate_mlmc_fixed(product, *model, n, 11, conditioned);
            }
            catch (const std::exception &e)
            {
                std::printf("\n%s -- %s: refused (%s)\n", c.product, c.estimator, e.what());
                continue;
            }
            std::vector<double> v, m;
            for (const qm::MlmcLevel &lv : r.levels)
                v.push_back(lv.variance()), m.push_back(std::fabs(lv.mean()));
            std::printf("\n%s -- %s%s\n  value %.6f +- %.6f   beta %.2f   alpha %.2f\n", c.product, c.estimator,
                        hard ? "" : (conditioned ? ", coarse conditioned on the fine path" : ", each level on its own steps"), r.value,
                        r.std_error, -slope(v), -slope(m));
            std::printf("  %-5s %12s %12s %12s\n", "level", "V_l", "|E Y_l|", "cost/path");
            for (std::size_t l = 0; l < r.levels.size(); ++l)
                std::printf("  %-5zu %12.4e %12.4e %12.0f\n", l, v[l], m[l], r.levels[l].cost);
        }
    return 0;
}
