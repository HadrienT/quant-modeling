#ifndef QM_TESTS_IR_FIXTURES_HPP
#define QM_TESTS_IR_FIXTURES_HPP

// Fixtures of the IR's path tests (testScriptIrPath.cpp, testScriptIrAd.cpp):
// the product library's scripts and the models a path is run on.

#include "library_exercise.hpp"
#include <Eigen/Core>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_path_host.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"
#include "quantModeling/scripting/ir_ad.hpp"
#include "quantModeling/scripting/ir_path.hpp"

namespace quantModeling::ir_test
{

    struct LibraryScript
    {
        std::string name, source;
    };

    inline std::vector<LibraryScript> library()
    {
        std::vector<LibraryScript> out;
        for (const auto &entry : std::filesystem::directory_iterator(QM_PRODUCT_LIBRARY_DIR))
        {
            if (entry.path().extension() != ".qms")
                continue;
            std::ifstream in(entry.path());
            std::stringstream text;
            text << in.rdbuf();
            if (library_script_has_exercise(text.str()))
                continue;
            out.push_back({entry.path().stem().string(), text.str()});
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b)
                  { return a.name < b.name; });
        return out;
    }

    inline const ValuationContext kCtx{Date::from_iso("2026-06-01")};
    inline const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
    inline const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

    inline std::vector<Real> skewed()
    {
        std::vector<Real> g;
        for (Real k : kK)
            for (Real t : kT)
                g.push_back(0.22 + 0.3 * (100.0 - k) / 100.0 + 0.005 * t);
        return g;
    }

    enum class Dyn
    {
        BlackScholes,
        LocalVol,
        Heston,
        SLV
    };

    inline const char *name(Dyn d)
    {
        switch (d)
        {
            case Dyn::LocalVol:
                return "local vol";
            case Dyn::Heston:
                return "Heston";
            case Dyn::SLV:
                return "SLV";
            default:
                return "Black-Scholes";
        }
    }

    /// The model of `dyn` for a script on n assets, in the number type T
    /// (Black-Scholes alone has several assets).
    template <class T>
    std::unique_ptr<ISimulationModel<T>> make_model(Dyn dyn, std::size_t n)
    {
        if (n > 1)
        {
            Eigen::MatrixXd corr =
                Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n), 0.4);
            corr.diagonal().setOnes();
            std::vector<T> s0, q, vol;
            for (std::size_t i = 0; i < n; ++i)
            {
                s0.push_back(T(100.0 - 4.0 * static_cast<double>(i)));
                q.push_back(T(0.01));
                vol.push_back(T(0.2 + 0.04 * static_cast<double>(i)));
            }
            return std::make_unique<MultiAssetBSSimModel<T>>(s0, T(0.03), q, vol, corr);
        }
        switch (dyn)
        {
            case Dyn::LocalVol:
            {
                std::vector<T> sig;
                for (Real x : skewed())
                    sig.emplace_back(x);
                return std::make_unique<LocalVolSimModel<T>>(T(100.0), T(0.03), T(0.01), kK, kT, sig, 1.0 / 24.0);
            }
            case Dyn::Heston:
                return std::make_unique<BatesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05),
                                                          T(0.6), T(-0.7), T(0.0), T(0.0), T(0.0));
            case Dyn::SLV:
            {
                std::vector<T> leverage; // around 1, skewed
                for (Real k : kK)
                    for (Real t : kT)
                        leverage.emplace_back(1.0 + 0.4 * (100.0 - k) / 100.0 + 0.005 * t);
                return std::make_unique<SLVSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.04), T(1.5), T(0.05), T(0.6),
                                                        T(-0.7), kK, kT, leverage);
            }
            default:
                return std::make_unique<BlackScholesSimModel<T>>(T(100.0), T(0.03), T(0.01), T(0.25));
        }
    }

    inline std::vector<std::vector<Time>> discount_mats(const ScriptedProduct<Real> &p)
    {
        std::vector<std::vector<Time>> out;
        for (const SampleDef &d : p.defline())
            out.push_back(d.discount_mats);
        return out;
    }

    /// A script on a model, ready to run a path either way: the hand-written
    /// path's view, and the IR's program and data.
    struct Case
    {
        ScriptedProduct<Real> product;
        std::unique_ptr<ISimulationModel<Real>> model;
        DeviceModel dm;
        std::vector<std::vector<Time>> mats;
        std::vector<Real> base;
        std::unique_ptr<mc::ScriptPathHost> host;

        Case(const std::string &source, Dyn dyn, const ScriptSettings &settings)
            : product(source, kCtx, settings), model(make_model<Real>(dyn, product.n_underlyings()))
        {
            model->init(product.timeline(), product.defline());
            if (!model->describe_device(dm))
                throw InvalidInput("the model has no device description");
            mats = discount_mats(product);
            base = product.baseline();
            host = std::make_unique<mc::ScriptPathHost>(product.program(), dm, mats, base);
        }
    };

    /// Every risk of a layout, as derivative directions.
    inline std::vector<std::int32_t> all_risks(const scripting::ir::PathLayout &l)
    {
        std::vector<std::int32_t> d(static_cast<std::size_t>(l.n_risks));
        std::iota(d.begin(), d.end(), 0);
        return d;
    }

    /// The payoff of one path, its gradient and its Hessian (n_risks x
    /// n_risks, row-major by parameter then direction).
    struct SecondOrder
    {
        double payoff = 0.0;
        std::vector<double> gradient, hessian;
    };

    /// One script on one model with its second-order programs (tangent over
    /// adjoint, WP 27 §5.3): a path's Hessian at any parameters.
    struct SecondOrderCase
    {
        Case c;
        scripting::ir::PathProgram primal, forward, backward;
        int K;
        std::uint64_t seed;
        SecondOrderCase(const std::string &source, Dyn dyn, const ScriptSettings &settings = {},
                        std::uint64_t seed_ = 19)
            : c(source, dyn, settings),
              primal(scripting::ir::path_program(c.product.ir(), c.dm.kind, c.dm.n_assets, true)),
              forward(scripting::ir::tangent(primal, all_risks(primal.layout))),
              backward(scripting::ir::tangent(scripting::ir::adjoint(primal), all_risks(primal.layout))),
              K(primal.layout.n_risks),
              seed(seed_)
        {
        }

        /// The parameters (the layout's order) the case starts from.
        std::vector<double> theta() const
        {
            const scripting::ir::PathData data =
                scripting::ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
            return std::vector<double>(data.theta.begin(), data.theta.begin() + K);
        }

        /// Path `path` with the parameters at `theta`.
        SecondOrder at(const std::vector<double> &theta, int path) const
        {
            scripting::ir::PathData data =
                scripting::ir::path_data(primal.layout, c.dm, c.product.ir(), c.mats, c.base);
            std::copy(theta.begin(), theta.end(), data.theta.begin());
            SecondOrder out;
            out.gradient.assign(data.theta.size(), 0.0);
            out.hessian.assign(data.theta.size() * static_cast<std::size_t>(K), 0.0);
            scripting::ir::DerivativeScratch w;
            mc::PathDraws draws;
            draws.seed = seed;
            draws.stride = c.host->view.stride;
            draws.begin(static_cast<uint64_t>(path));
            out.payoff = scripting::ir::run_path_second_order(forward, backward, K, data, c.dm, c.product.ir(), draws,
                                                              1.0, w, out.gradient.data(), out.hessian.data());
            return out;
        }
    };

    inline bool same_bits(double a, double b)
    {
        return std::memcmp(&a, &b, sizeof a) == 0;
    }

} // namespace quantModeling::ir_test

#endif // QM_TESTS_IR_FIXTURES_HPP
