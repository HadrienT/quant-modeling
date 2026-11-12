// clone() of every simulation model is the model: same paths to the bit,
// and parameters that point into the clone, never into the original.
//
// Each model keeps non-owning pointers to its own parameters (for the
// adjoints), so a compiler-generated copy would leave the clone pointing into
// the original: every model writes its copy constructor by hand, listing its
// members, then calls set_param_pointers(). A member added later and left out
// of that list is default-constructed in the clone without any error, and
// the parallel engines price with clones. This test catches both mistakes,
// for a model cloned before and after init().

#include <gtest/gtest.h>

#include "quantModeling/core/sample.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/commodity/schwartz_smith_sim_model.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/kou_jump_diffusion_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/merton_jump_diffusion_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_local_vol_sim_model.hpp"
#include "quantModeling/models/equity/rough_bergomi_sim_model.hpp"
#include "quantModeling/models/equity/sabr_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/models/hybrid/hull_white_equity_sim_model.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"
#include "quantModeling/models/rates/vasicek_sim_model.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using Model = ISimulationModel<Real>;

        struct Case
        {
            std::string name;
            std::function<std::unique_ptr<Model>()> make;
            std::size_t size; // sizeof the concrete model
        };

        template <class M, class... Args>
        Case model_case(std::string name, Args... args)
        {
            return {std::move(name), [=]
                    { return std::unique_ptr<Model>(std::make_unique<M>(args...)); },
                    sizeof(M)};
        }

        const std::vector<Real> kK{80.0, 100.0, 120.0};
        const std::vector<Real> kT{0.5, 1.0, 2.0};

        // A surface that is not flat, so a grid the clone lost or mixed up
        // shows in the paths.
        std::vector<Real> surface(Real level)
        {
            std::vector<Real> s;
            for (std::size_t i = 0; i < kK.size() * kT.size(); ++i)
                s.push_back(level + 0.01 * static_cast<Real>(i % 4));
            return s;
        }

        Eigen::MatrixXd corr2(double rho)
        {
            Eigen::MatrixXd c(2, 2);
            c << 1.0, rho, rho, 1.0;
            return c;
        }

        std::vector<Case> cases()
        {
            using MALV = MultiAssetLocalVolSimModel<Real>;
            return {
                model_case<BlackScholesSimModel<Real>>("BlackScholes", 100.0, 0.03, 0.01, 0.2),
                model_case<BlackScholesSimModel<Real>>("BlackScholesOnCurve", 100.0, DiscountCurve(0.035),
                                                       0.01, 0.2),
                model_case<MultiAssetBSSimModel<Real>>("MultiAssetBS", std::vector<Real>{100.0, 90.0}, 0.03,
                                                       std::vector<Real>{0.01, 0.02},
                                                       std::vector<Real>{0.2, 0.25}, corr2(0.4)),
                model_case<LocalVolSimModel<Real>>("LocalVol", 100.0, 0.03, 0.01, kK, kT, surface(0.2),
                                                   1.0 / 24.0),
                model_case<MALV>("MultiAssetLocalVol", std::vector<Real>{100.0, 90.0}, 0.03,
                                 std::vector<Real>{0.01, 0.02},
                                 std::vector<MALV::Surface>{MALV::Surface{kK, kT, surface(0.2)},
                                                            MALV::Surface{kK, kT, surface(0.25)}},
                                 corr2(0.4), 1.0 / 24.0),
                model_case<SLVSimModel<Real>>("SLV", 100.0, 0.03, 0.01, 0.04, 1.5, 0.04, 0.5, -0.6, kK, kT,
                                              surface(1.0), 1.0 / 24.0),
                model_case<BatesSimModel<Real>>("Bates", 100.0, 0.03, 0.01, 0.04, 1.5, 0.04, 0.5, -0.6, 0.3,
                                                -0.05, 0.1, 1.0 / 24.0),
                model_case<MertonJumpDiffusionSimModel<Real>>("Merton", 100.0, 0.03, 0.01, 0.2, 0.3, -0.05,
                                                              0.1),
                model_case<KouJumpDiffusionSimModel<Real>>("Kou", 100.0, 0.03, 0.01, 0.2, 0.3, 0.4, 10.0, 5.0),
                model_case<SABRSimModel<Real>>("SABR", 100.0, 0.03, 0.25, 0.6, -0.3, 0.5, 1.0 / 24.0),
                model_case<RoughBergomiSimModel<Real>>("RoughBergomi", 100.0, 0.03, 0.01,
                                                       ForwardVarianceCurve{{0.25, 1.0}, {0.03, 0.045}}, 1.5,
                                                       -0.7, 0.1, 48),
                model_case<VasicekSimModel<Real>>("Vasicek", 0.03, 0.5, 0.04, 0.01),
                model_case<SchwartzSmithSimModel<Real>>("SchwartzSmith", 50.0, 0.03, 0.1, 1.2, 0.3, 0.01, 0.2,
                                                        -0.3),
                model_case<HullWhiteEquitySimModel<Real>>("HullWhiteEquity", 100.0, 0.01, 0.2,
                                                          HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.04)),
                                                          0.3),
            };
        }

        const TimeLine kTimeline{0.25, 0.5, 1.0};

        std::vector<SampleDef> defline()
        {
            std::vector<SampleDef> d(kTimeline.size());
            for (std::size_t i = 0; i < d.size(); ++i)
            {
                d[i].discount_mats = {kTimeline[i] + 0.5};
                d[i].forward_mats = {kTimeline[i] + 1.0};
            }
            return d;
        }

        // Deterministic draws in a few standard deviations, different per path.
        std::vector<double> draws(std::size_t n, int path)
        {
            std::vector<double> z(n);
            for (std::size_t i = 0; i < n; ++i)
                z[i] = 2.0 * std::sin(1.7 * static_cast<double>(i + 1) + 0.9 * path);
            return z;
        }

        Scenario<Real> path_of(const Model &m, int path)
        {
            Scenario<Real> scen;
            allocate_scenario(scen, defline(), m.n_underlyings());
            const std::vector<double> z = draws(m.sim_dim(), path);
            m.generate_path(z, scen);
            return scen;
        }

        void expect_same_paths(const Model &a, const Model &b)
        {
            ASSERT_EQ(a.sim_dim(), b.sim_dim());
            ASSERT_EQ(a.sim_timeline(), b.sim_timeline());
            for (int path = 0; path < 4; ++path)
            {
                const Scenario<Real> x = path_of(a, path), y = path_of(b, path);
                ASSERT_EQ(x.size(), y.size());
                for (std::size_t i = 0; i < x.size(); ++i)
                {
                    // Exact equality on purpose: a clone is the same model.
                    EXPECT_EQ(x[i].spots, y[i].spots) << "date " << i << ", path " << path;
                    EXPECT_EQ(x[i].numeraire, y[i].numeraire) << "date " << i << ", path " << path;
                    EXPECT_EQ(x[i].discounts, y[i].discounts) << "date " << i << ", path " << path;
                    EXPECT_EQ(x[i].forwards, y[i].forwards) << "date " << i << ", path " << path;
                }
            }
        }

        bool inside(const void *p, const void *object, std::size_t size)
        {
            const auto *c = static_cast<const char *>(p);
            const auto *o = static_cast<const char *>(object);
            return c >= o && c < o + size;
        }

        void expect_own_parameters(const Model &original, const Model &clone, std::size_t size)
        {
            const std::vector<Real *> &mine = original.parameters();
            const std::vector<Real *> &theirs = clone.parameters();
            ASSERT_EQ(mine.size(), theirs.size());
            ASSERT_FALSE(mine.empty());
            EXPECT_EQ(original.parameter_labels(), clone.parameter_labels());
            // The concrete object starts where its most-derived pointer does.
            const void *original_object = dynamic_cast<const void *>(&original);
            for (std::size_t j = 0; j < mine.size(); ++j)
            {
                const std::string label = original.parameter_labels()[j];
                EXPECT_NE(mine[j], theirs[j]) << label;
                EXPECT_FALSE(inside(theirs[j], original_object, size)) << label;
                EXPECT_EQ(*mine[j], *theirs[j]) << label;
            }
        }

        class SimModelClone : public ::testing::TestWithParam<Case>
        {
        };
    } // namespace

    TEST_P(SimModelClone, ClonedAfterInitIsTheSameModel)
    {
        const std::unique_ptr<Model> original = GetParam().make();
        original->init(kTimeline, defline());
        const std::unique_ptr<Model> clone = original->clone();
        expect_own_parameters(*original, *clone, GetParam().size);
        expect_same_paths(*original, *clone);
    }

    TEST_P(SimModelClone, ClonedBeforeInitIsTheSameModel)
    {
        const std::unique_ptr<Model> original = GetParam().make();
        const std::unique_ptr<Model> clone = original->clone();
        expect_own_parameters(*original, *clone, GetParam().size);
        original->init(kTimeline, defline());
        clone->init(kTimeline, defline());
        expect_same_paths(*original, *clone);
    }

    INSTANTIATE_TEST_SUITE_P(EveryModel, SimModelClone, ::testing::ValuesIn(cases()),
                             [](const ::testing::TestParamInfo<Case> &param)
                             { return param.param.name; });

} // namespace quantModeling
