// Benchmark of blueprint/wp/23-xva.md §14.13 (lot X8): what the sensitivities
// of the CVA cost by adjoint differentiation, against one pricing and against
// bumping every input.
//
//   build/qm_xva_aad_bench [paths=20000] [threads=16]
//
// The pricing is the adjoint's own code run on doubles (xva_values): the same
// estimator, the same scenarios, the same threads. The ratio is the figure
// Savine's AAD book is about: every sensitivity for a small multiple of one
// valuation, whatever their number.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/market/hull_white_calibration.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/utils/thread_pool.hpp"

namespace qm = quantModeling;

namespace
{
    qm::DiscountCurve curve()
    {
        const std::vector<std::pair<double, double>> swaps{{1, 0.0455}, {2, 0.0473}, {3, 0.0477}, {5, 0.0478}, {7, 0.0481}, {10, 0.0486}, {15, 0.0500}, {20, 0.0503}, {30, 0.0494}};
        std::vector<qm::ParRateQuote> par;
        for (const auto &[tenor, rate] : swaps)
            par.push_back(qm::make_ois_quote(tenor, rate));
        return qm::bootstrap_curve({}, par);
    }

    qm::InterestRateSwap swap(const qm::HullWhiteCurveModel &m, double start, double tenor, bool payer,
                              double notional, double moneyness = 0.0)
    {
        const double par = qm::value_swap(qm::make_swap(start, tenor, 0.0, 1, 2),
                                          qm::MultiCurve{m.discount(), m.projection()})
                               .par_rate;
        return qm::make_swap(start, tenor, par + moneyness, 1, 2, notional, payer);
    }

    /// The book of the GPU benchmark (benchmarks/gpu_exposure.cpp).
    void fill(qm::HullWhiteExposureEngine &engine, const qm::HullWhiteCurveModel &m)
    {
        engine.add(swap(m, 0.0, 2.0, true, 5e7));
        engine.add(swap(m, 0.0, 5.0, true, 4e7, 0.002));
        engine.add(swap(m, 0.0, 7.0, false, 3e7, -0.001));
        engine.add(swap(m, 0.0, 10.0, true, 5e7));
        engine.add(swap(m, 1.0, 10.0, true, 2e7, 0.003));
        engine.add(swap(m, 0.0, 15.0, false, 2e7));
        engine.add(swap(m, 0.0, 20.0, true, 3e7, -0.002));
        engine.add(swap(m, 2.0, 8.0, false, 2e7));
        engine.add(qm::Swaption{swap(m, 3.0, 7.0, true, 4e7, 0.002), 3.0});
        engine.add(qm::Swaption{swap(m, 5.0, 10.0, false, 3e7), 5.0}, -1.0);
    }
} // namespace

int main(int argc, char **argv)
{
    const std::size_t paths = argc > 1 ? static_cast<std::size_t>(std::atof(argv[1])) : 20000;
    const int threads = argc > 2 ? std::atoi(argv[2]) : 16;

    const qm::HullWhiteCurveModel model(0.03, 0.01, curve());
    qm::HullWhiteExposureEngine engine(model);
    fill(engine, model);

    qm::XvaRiskInputs inputs;
    inputs.counterparty = qm::CreditCurve({0.5, 1.0, 3.0, 5.0, 10.0}, {0.02, 0.02, 0.02, 0.02, 0.02});
    inputs.own = qm::CreditCurve(0.01);
    inputs.borrowing_spread = 0.004;
    inputs.lending_spread = 0.001;
    qm::Csa csa;
    csa.minimum_transfer_amount = 1e5;
    csa.rounding = 1e4;
    inputs.csa = csa;

    qm::ExposureSimulationSettings settings;
    settings.paths = paths;
    settings.seed = 20261002;

    for (const int n : {1, threads})
    {
        qm::ThreadPool pool;
        if (n > 1)
            pool.start(static_cast<std::size_t>(n - 1)); // the caller is the last thread
        qm::ThreadPool *p = n > 1 ? &pool : nullptr;
        // On one thread a fraction of the paths is timed: the ratio is per path.
        qm::ExposureSimulationSettings s = settings;
        if (n == 1)
            s.paths = std::max<std::size_t>(paths / 8, 640);
        const qm::XvaRisks risks = engine.xva_risks(s, inputs, p);
        qm::XvaRiskInputs same = inputs;
        same.smoothing_widths = risks.smoothing_widths;
        const qm::XvaValues values = engine.xva_values(s, same, p);
        if (n > 1)
            pool.stop();

        const std::size_t factors = risks.factors.size();
        std::printf("%d thread%s, %zu paths, %zu trades, CSA: %zu inputs, %zu adjustments\n", n, n > 1 ? "s" : "",
                    s.paths, engine.trades(), factors, qm::kXvaOutputs);
        std::printf("  one valuation            %8.3f s   CVA %12.2f +- %.2f\n", values.seconds,
                    values[qm::XvaOutput::Cva].value, values[qm::XvaOutput::Cva].error);
        std::printf("  with every sensitivity   %8.3f s   CVA %12.2f +- %.2f\n", risks.seconds,
                    risks[qm::XvaOutput::Cva].value, risks[qm::XvaOutput::Cva].error);
        std::printf("  adjoint / valuation      %8.1f      (bumping every input, both ways: %zu valuations)\n",
                    risks.seconds / values.seconds, 2 * factors);
        const qm::Estimate sigma = risks.risk(qm::XvaOutput::Cva, factors - 4 - 1 - 5 - 1);
        std::printf("  dCVA/dsigma              %12.0f +- %.0f\n\n", sigma.value, sigma.error);
    }
    return 0;
}
