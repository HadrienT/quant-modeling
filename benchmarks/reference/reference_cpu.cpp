// Hand-written reference paths on one CPU thread (blueprint/wp/26-performance.md,
// lot P0): the ceiling for the script engine's CPU rows of
// benchmarks/perf_matrix.py.
//
//   build/qm_reference_bench [target_rel_se=1e-3]

#include "reference_cases.hpp"

#include <cstdlib>

using namespace qm_reference;

int main(int argc, char **argv)
{
    const double target = argc > 1 ? std::atof(argv[1]) : 1e-3;
    const Grid g = local_vol_grid();
    const LocalVolGrid view{g.K.data(), static_cast<int>(g.K.size()), g.T.data(), static_cast<int>(g.T.size()),
                            g.sigma.data()};
    std::printf("Reference paths, one CPU thread, time to a relative std error of %.0e\n", target);
    for (const Case &c : cases())
    {
        const LocalVolGrid *grid = c.local_vol ? &view : nullptr;
        time_to_error(c.name, target,
                      [&](uint64_t units)
                      {
                          Estimate e;
                          double sum = 0.0, sq = 0.0;
                          e.seconds = seconds(
                              [&]
                              {
                                  for (uint64_t u = 0; u < units; ++u)
                                  {
                                      const double x = unit(c.product, c.terms, grid, 1, u);
                                      sum += x;
                                      sq += x * x;
                                  }
                              });
                          const double n = static_cast<double>(units);
                          e.mean = sum / n;
                          e.se = std::sqrt(std::max(0.0, sq / n - e.mean * e.mean) / (n - 1));
                          return e;
                      });
    }
    return 0;
}
