#ifndef QM_ENGINES_MC_LSM_HPP
#define QM_ENGINES_MC_LSM_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/scripting/exercise.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    struct LsmSettings
    {
        /// Paths of the pilot that fits the exercise rule; 0 picks
        /// max(20 000, a quarter of the pricing's paths), capped at 200 000.
        int pilot_paths = 0;
        /// Total degree of the polynomial basis; 0 picks 3 for one or two
        /// regressors, 2 beyond.
        int degree = 0;
    };

    struct LsmReport
    {
        int pilot_paths = 0;
        int degree = 0;
        std::size_t regressors = 0;
        /// Per exercise date (timeline index): pilot paths that reached it,
        /// and the fraction of those the fitted rule exercises there.
        struct Date
        {
            std::size_t event;
            int reached;
            double exercised;
        };
        std::vector<Date> dates;

        std::string note() const
        {
            return "LSMC exercise rule: " + std::to_string(dates.size()) + " dates, " +
                   std::to_string(pilot_paths) + " independent pilot paths, degree-" + std::to_string(degree) +
                   " polynomial in " + std::to_string(regressors) +
                   " regressor(s) (a lower bound: the rule is estimated)";
        }
    };

    /**
     * @brief Fit the exercise rule of a script with exercise() / call() by
     *        Longstaff-Schwartz regression, on a pilot independent of the
     *        pricing paths (blueprint/wp/16-scripting.md §14).
     *
     * 1. Pilot: `pilot_paths` paths of the model (Philox, a seed apart from
     *    the pricing's). On each, the script runs once without exercising and
     *    once per exercise date it reaches, exercised there
     *    (ScriptedProduct::exercise_pilot): the value of every branch.
     * 2. Backward induction over the exercise dates, last first: Y_p starts
     *    at the never-exercised value; at date e, regress the gain
     *    V_ex(e) − Y_p on the regressors of the paths that reached e, and on
     *    those the rule exercises (gain > 0 for the holder, < 0 for the
     *    issuer) set Y_p = V_ex(e).
     * 3. The rule is set on `product`; the pricing that follows (the generic
     *    engine, any sampler) applies it to fresh paths, so the price is a
     *    lower bound for the holder, free of the foresight bias of reusing
     *    the regression's own paths.
     *
     * The values carry everything the script pays on the path, before and
     * after the date: the flows before e are the same in every branch that
     * has not exercised yet, so they cancel in the gain.
     */
    inline LsmReport fit_exercise_policy(ScriptedProduct<Real> &product, ISimulationModel<Real> &model,
                                         int pricing_paths, std::uint64_t seed, const LsmSettings &settings = {})
    {
        LsmReport report;
        if (!product.has_exercise())
            return report;
        const int n_pilot = settings.pilot_paths > 0
                                ? settings.pilot_paths
                                : std::min(200000, std::max(20000, pricing_paths / 4));
        report.pilot_paths = n_pilot;

        model.init(product.timeline(), product.defline());
        const std::size_t dim = model.sim_dim();
        Scenario<Real> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<double> gauss(std::max<std::size_t>(dim, 1));
        // A stream the pricing never draws: the seed's complement.
        PhiloxGaussianSource source(~seed);

        std::vector<ExercisePilotPath> pilot;
        pilot.reserve(static_cast<std::size_t>(n_pilot));
        for (int p = 0; p < n_pilot; ++p)
        {
            source.set_path(static_cast<std::uint64_t>(p));
            for (std::size_t d = 0; d < dim; ++d)
                gauss[d] = source.next();
            model.generate_path(std::span<const double>(gauss.data(), dim), path);
            pilot.push_back(product.exercise_pilot(path));
        }

        // Paths by exercise date: (path, opportunity) pairs.
        std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> by_event;
        for (std::size_t p = 0; p < pilot.size(); ++p)
            for (std::size_t k = 0; k < pilot[p].opportunities.size(); ++k)
                by_event[pilot[p].opportunities[k].event].push_back({p, k});
        if (by_event.empty())
            return report; // no pilot path ever reached an exercise date

        report.regressors = pilot[by_event.begin()->second.front().first]
                                .opportunities[by_event.begin()->second.front().second]
                                .regressors.size();
        const int degree = settings.degree > 0 ? settings.degree : (report.regressors <= 2 ? 3 : 2);
        report.degree = degree;

        scripting::ExercisePolicy policy;
        policy.issuer = product.issuer_exercise();
        std::vector<double> Y(pilot.size());
        for (std::size_t p = 0; p < pilot.size(); ++p)
            Y[p] = pilot[p].never;

        const std::size_t n_basis = scripting::monomials(report.regressors, degree).size();
        for (auto it = by_event.rbegin(); it != by_event.rend(); ++it)
        {
            const auto &members = it->second;
            // The holder's "in the money" (Longstaff & Schwartz), for any
            // script: exercising now changes the path's value. Where it does
            // not, the gain is never - Y <= 0 (continuing under the best rule
            // is worth at least never exercising): those paths do not
            // exercise, and fitting them only bends the polynomial where the
            // decision is made (issue #228). Too few such paths for the
            // basis: all of them, as before. The issuer fits every path.
            const auto changes = [&](std::size_t m)
            {
                const auto &[p, k] = members[m];
                const double ex = pilot[p].opportunities[k].exercised, nv = pilot[p].never;
                return std::abs(ex - nv) > 1e-12 * std::max({1.0, std::abs(ex), std::abs(nv)});
            };
            std::size_t in_money = 0;
            if (!policy.issuer)
                for (std::size_t m = 0; m < members.size(); ++m)
                    in_money += changes(m) ? 1 : 0;
            const bool restrict = !policy.issuer && in_money >= 2 * n_basis && in_money < members.size();
            std::vector<std::vector<double>> rows, fit_rows;
            std::vector<double> fit_gains;
            rows.reserve(members.size());
            for (std::size_t m = 0; m < members.size(); ++m)
            {
                const auto &[p, k] = members[m];
                const auto &opp = pilot[p].opportunities[k];
                rows.push_back(opp.regressors);
                if (!restrict || changes(m))
                {
                    fit_rows.push_back(opp.regressors);
                    fit_gains.push_back(opp.exercised - Y[p]);
                }
            }
            scripting::ExerciseRegression reg = scripting::fit_exercise_regression(fit_rows, fit_gains, degree);
            if (restrict)
            {
                // The box the rule may exercise in: the in-the-money paths'
                // regressors, unbounded on a side where no other path lies.
                const std::size_t n = report.regressors;
                reg.lo.assign(n, std::numeric_limits<double>::infinity());
                reg.hi.assign(n, -std::numeric_limits<double>::infinity());
                for (const auto &row : fit_rows)
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        reg.lo[i] = std::min(reg.lo[i], row[i]);
                        reg.hi[i] = std::max(reg.hi[i], row[i]);
                    }
                std::vector<char> below(n, 0), above(n, 0);
                for (std::size_t m = 0; m < members.size(); ++m)
                    if (!changes(m))
                        for (std::size_t i = 0; i < n; ++i)
                        {
                            below[i] |= rows[m][i] < reg.lo[i] ? 1 : 0;
                            above[i] |= rows[m][i] > reg.hi[i] ? 1 : 0;
                        }
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (!below[i])
                        reg.lo[i] = -std::numeric_limits<double>::infinity();
                    if (!above[i])
                        reg.hi[i] = std::numeric_limits<double>::infinity();
                }
            }
            int exercised = 0;
            for (std::size_t m = 0; m < members.size(); ++m)
            {
                if (restrict && !changes(m))
                    continue; // gain <= 0: not exercised there
                const double g = reg.gain(rows[m]);
                if (policy.issuer ? g < 0.0 : g > 0.0)
                {
                    const auto &[p, k] = members[m];
                    Y[p] = pilot[p].opportunities[k].exercised;
                    ++exercised;
                }
            }
            policy.by_event[it->first] = reg;
            report.dates.push_back({it->first, static_cast<int>(members.size()),
                                    static_cast<double>(exercised) / static_cast<double>(members.size())});
        }
        std::reverse(report.dates.begin(), report.dates.end());
        product.set_exercise_policy(std::move(policy));
        return report;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_LSM_HPP
