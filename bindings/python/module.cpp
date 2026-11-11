// The quantmodeling Python module: every domain registers its own classes and
// functions (bindings/python/bind_*.cpp, one translation unit each).

#include "common.hpp"

PYBIND11_MODULE(quantmodeling, m)
{
    register_core(m);
    register_pricing(m);
    register_rates(m);
    register_market(m);
    register_xva(m);
    register_scripting(m);
}
