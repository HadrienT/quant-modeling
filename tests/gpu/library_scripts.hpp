#ifndef QM_TESTS_GPU_LIBRARY_SCRIPTS_HPP
#define QM_TESTS_GPU_LIBRARY_SCRIPTS_HPP

// The GPU script tests' fixtures: the product library's scripts and the
// models they are priced on (testGpuScripts.cpp, testGpuScriptJit.cpp).

#include <Eigen/Core>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/models/equity/slv_sim_model.hpp"

namespace quantModeling::gpu_test
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
            out.push_back({entry.path().stem().string(), text.str()});
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b)
                  { return a.name < b.name; });
        return out;
    }

    inline const ValuationContext kCtx{Date::from_iso("2026-06-01")};
    inline const std::vector<Real> kK{60, 80, 90, 100, 110, 120, 150};
    inline const std::vector<Real> kT{0.25, 0.5, 1.0, 2.0, 5.0};

    inline std::vector<Real> skewed(double level, double skew)
    {
        std::vector<Real> g;
        for (Real k : kK)
            for (Real t : kT)
                g.push_back(level + skew * (100.0 - k) / 100.0 + 0.005 * t);
        return g;
    }

    enum class Dyn
    {
        BlackScholes,
        LocalVol,
        Heston,
        SLV
    };

    inline std::unique_ptr<ISimulationModel<Real>> model(Dyn dyn, std::size_t n_assets)
    {
        if (n_assets > 1)
        {
            Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(static_cast<Eigen::Index>(n_assets),
                                                             static_cast<Eigen::Index>(n_assets), 0.4);
            corr.diagonal().setOnes();
            std::vector<Real> s0, q, vol;
            for (std::size_t i = 0; i < n_assets; ++i)
            {
                s0.push_back(100.0 - 4.0 * static_cast<double>(i));
                q.push_back(0.01);
                vol.push_back(0.2 + 0.04 * static_cast<double>(i));
            }
            return std::make_unique<MultiAssetBSSimModel<Real>>(s0, 0.03, q, vol, corr);
        }
        switch (dyn)
        {
            case Dyn::LocalVol:
                return std::make_unique<LocalVolSimModel<Real>>(100.0, 0.03, 0.01, kK, kT, skewed(0.22, 0.3));
            case Dyn::Heston:
                return std::make_unique<BatesSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, 0.0,
                                                             0.0, 0.0);
            case Dyn::SLV:
                return std::make_unique<SLVSimModel<Real>>(100.0, 0.03, 0.01, 0.04, 1.5, 0.05, 0.6, -0.7, kK, kT,
                                                           skewed(1.0, 0.4));
            default:
                return std::make_unique<BlackScholesSimModel<Real>>(100.0, 0.03, 0.01, 0.25);
        }
    }

} // namespace quantModeling::gpu_test

#endif // QM_TESTS_GPU_LIBRARY_SCRIPTS_HPP
