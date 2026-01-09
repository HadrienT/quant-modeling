#include "quantModeling/utils/stats.hpp"
#include <cmath>

double norm_cdf(double x)
{
    return 0.5 * erfc(-x / std::sqrt(2.0));
};