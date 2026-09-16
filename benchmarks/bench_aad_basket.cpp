#include <benchmark/benchmark.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"

#include <Eigen/Core>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

// blueprint/wp/17-aad.md lot 17e's own demonstration: for a basket of n
// correlated underlyings, bump-and-reprice needs ~2 reprices (up/down) per
// sensitivity -- here 2 sensitivities per asset (delta, vega) -- while
// simulate_aad() gives every one of them from a single run. This benchmark
// measures both costs directly, across a range of n, instead of asserting
// the O(1)-in-n claim from the roadmap table.

namespace qm = quantModeling;

namespace
{
    constexpr int kPaths = 5000;
    constexpr qm::Real kS0 = 100.0, kR = 0.03, kQ = 0.0, kSigma = 0.20,
                       kRho = 0.30, kK = 100.0;

    template <class T>
    struct BasketCallT final : qm::ISimulatableProduct<T>
    {
        qm::Real K;
        std::size_t n;
        qm::TimeLine tl_{1.0};
        std::vector<qm::SampleDef> dl_{1};
        std::vector<std::string> labels_{"price"};

        BasketCallT(qm::Real k, std::size_t n_assets) : K(k), n(n_assets) {}
        const qm::TimeLine &timeline() const override { return tl_; }
        const std::vector<qm::SampleDef> &defline() const override { return dl_; }
        const std::vector<std::string> &payoff_labels() const override
        {
            return labels_;
        }
        std::size_t n_underlyings() const override { return n; }
        void payoffs(const qm::Scenario<T> &p, std::vector<T> &out) const override
        {
            using std::max;
            T total = p[0].spots[0];
            for (std::size_t i = 1; i < n; ++i)
                total = total + p[0].spots[i];
            const T mean = total / static_cast<double>(n);
            out.assign(1, max(mean - K, 0.0) / p[0].numeraire);
        }
    };

    /// Equicorrelation: PSD for any rho in (-1/(n-1), 1), comfortably true
    /// here for any n at rho = 0.30.
    Eigen::MatrixXd equicorrelation(std::size_t n, double rho)
    {
        Eigen::MatrixXd corr = Eigen::MatrixXd::Constant(
            static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n), rho);
        corr.diagonal().setOnes();
        return corr;
    }

    qm::PricingSettings settings()
    {
        qm::PricingSettings s;
        s.mc_paths = kPaths;
        s.mc_seed = 1;
        return s;
    }
} // namespace

static void BM_Basket_BumpAndReprice(benchmark::State &state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    const BasketCallT<qm::Real> product(kK, n);
    const std::vector<qm::Real> s0(n, kS0), q(n, kQ), sigma(n, kSigma);
    const Eigen::MatrixXd corr = equicorrelation(n, kRho);
    const qm::Real h_spot = 0.01, h_vol = 0.001;

    for (auto _ : state)
    {
        {
            qm::MultiAssetBSSimModel<qm::Real> model(s0, kR, q, sigma, corr);
            const auto res = qm::simulate<qm::Real>(product, model, settings());
            benchmark::DoNotOptimize(res.npv());
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            std::vector<qm::Real> s_up = s0, s_dn = s0;
            s_up[i] += h_spot;
            s_dn[i] -= h_spot;
            qm::MultiAssetBSSimModel<qm::Real> m_up(s_up, kR, q, sigma, corr);
            qm::MultiAssetBSSimModel<qm::Real> m_dn(s_dn, kR, q, sigma, corr);
            benchmark::DoNotOptimize(
                qm::simulate<qm::Real>(product, m_up, settings()).npv());
            benchmark::DoNotOptimize(
                qm::simulate<qm::Real>(product, m_dn, settings()).npv());

            std::vector<qm::Real> v_up = sigma, v_dn = sigma;
            v_up[i] += h_vol;
            v_dn[i] -= h_vol;
            qm::MultiAssetBSSimModel<qm::Real> mv_up(s0, kR, q, v_up, corr);
            qm::MultiAssetBSSimModel<qm::Real> mv_dn(s0, kR, q, v_dn, corr);
            benchmark::DoNotOptimize(
                qm::simulate<qm::Real>(product, mv_up, settings()).npv());
            benchmark::DoNotOptimize(
                qm::simulate<qm::Real>(product, mv_dn, settings()).npv());
        }
        // rho: one more bumped sensitivity, matching the roadmap's own "50
        // deltas + 50 vegas + rho" illustration for the basket example.
        {
            qm::MultiAssetBSSimModel<qm::Real> m_up(s0, kR + h_spot / 100.0, q,
                                                    sigma, corr);
            qm::MultiAssetBSSimModel<qm::Real> m_dn(s0, kR - h_spot / 100.0, q,
                                                    sigma, corr);
            benchmark::DoNotOptimize(
                qm::simulate<qm::Real>(product, m_up, settings()).npv());
            benchmark::DoNotOptimize(
                qm::simulate<qm::Real>(product, m_dn, settings()).npv());
        }
    }
    state.SetLabel(std::to_string(4 * n + 3) + " reprices (" +
                   std::to_string(2 * n + 1) +
                   " sensitivities: n deltas + n vegas + rho, n=" +
                   std::to_string(n) + ")");
}
BENCHMARK(BM_Basket_BumpAndReprice)
    ->Arg(1)
    ->Arg(5)
    ->Arg(10)
    ->Arg(25)
    ->Arg(50)
    ->Unit(benchmark::kMillisecond);

static void BM_Basket_AAD(benchmark::State &state)
{
    using qm::aad::Number;
    const auto n = static_cast<std::size_t>(state.range(0));
    const BasketCallT<Number> product(kK, n);
    const Eigen::MatrixXd corr = equicorrelation(n, kRho);

    for (auto _ : state)
    {
        const std::vector<Number> s0(n, Number(kS0)), q(n, Number(kQ)),
            sigma(n, Number(kSigma));
        qm::MultiAssetBSSimModel<Number> model(s0, Number(kR), q, sigma, corr);
        const auto res = qm::simulate_aad(product, model, kPaths, 1);
        benchmark::DoNotOptimize(res.price);
    }
    state.SetLabel("1 simulate_aad run (" + std::to_string(3 * n + 1) +
                   " params: rate + n spot[i] + n div[i] + n vol[i], n=" +
                   std::to_string(n) + ")");
}
BENCHMARK(BM_Basket_AAD)
    ->Arg(1)
    ->Arg(5)
    ->Arg(10)
    ->Arg(25)
    ->Arg(50)
    ->Unit(benchmark::kMillisecond);
