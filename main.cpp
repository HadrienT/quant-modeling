#include "quantModeling/pricers/registry.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace quantModeling
{
    // ANSI color codes
    constexpr const char *COLOR_RESET = "\033[0m";
    constexpr const char *COLOR_BOLD = "\033[1m";
    constexpr const char *COLOR_GREEN = "\033[32m";
    constexpr const char *COLOR_YELLOW = "\033[33m";
    constexpr const char *COLOR_BLUE = "\033[34m";
    constexpr const char *COLOR_CYAN = "\033[36m";

    // Helper function to print pricing results
    void print_result(const std::string &name, const PricingResult &res)
    {
        std::cout << "  " << COLOR_CYAN << std::left << std::setw(40) << name << COLOR_RESET
                  << " | NPV: " << COLOR_YELLOW << std::right << std::setw(10) << std::fixed << std::setprecision(3) << res.npv << COLOR_RESET;
        if (res.mc_std_error != 0.0)
            std::cout << " (±" << std::fixed << std::setprecision(3) << res.mc_std_error << ")";
        std::cout << "\n";
        auto fmt = [&](const std::optional<Real> &val, const std::optional<Real> &se)
        {
            if (!val)
                return std::string("N/A");
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(3) << *val;
            if (se && *se != 0.0)
                ss << " (±" << std::fixed << std::setprecision(3) << *se << ")";
            return ss.str();
        };

        std::cout << "    Greeks: "
                  << "delta=" << COLOR_BLUE << fmt(res.greeks.delta, res.greeks.delta_std_error) << COLOR_RESET
                  << " gamma=" << COLOR_BLUE << fmt(res.greeks.gamma, res.greeks.gamma_std_error) << COLOR_RESET
                  << " vega=" << COLOR_BLUE << fmt(res.greeks.vega, res.greeks.vega_std_error) << COLOR_RESET
                  << " rho=" << COLOR_BLUE << fmt(res.greeks.rho, res.greeks.rho_std_error) << COLOR_RESET
                  << " theta=" << COLOR_BLUE << fmt(res.greeks.theta, res.greeks.theta_std_error) << COLOR_RESET << "\n";
        std::cout << "    Diag: " << res.diagnostics << "\n\n";
    }
}

int main()
{
    using namespace quantModeling;
    const auto &registry = default_registry();

    const Real spot = 100.0;
    const Real strike = 100.0;
    const Time maturity = 1.0;
    const Real rate = 0.05;
    const Real dividend = 0.02;
    const Real vol = 0.20;

    const auto price_vanilla = [&](const std::string &label, bool is_call)
    {
        VanillaBSInput in{spot, strike, maturity, rate, dividend, vol, is_call};
        in.n_paths = 100'000;
        in.seed = 42;
        in.mc_epsilon = 0.0;

        PricingRequest analytic_request{
            InstrumentKind::EquityVanillaOption,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};

        PricingRequest mc_request{
            InstrumentKind::EquityVanillaOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};

        std::cout << COLOR_BOLD << label << COLOR_RESET << "\n";
        print_result("Analytic Engine", registry.price(analytic_request));
        print_result("MC Engine", registry.price(mc_request));
    };

    const auto price_asian = [&](const std::string &label, bool is_call, AsianAverageType average_type)
    {
        AsianBSInput in{spot, strike, maturity, rate, dividend, vol, is_call, average_type};
        in.n_paths = 100'000;
        in.seed = 42;
        in.mc_epsilon = 0.0;

        PricingRequest analytic_request{
            InstrumentKind::EquityAsianOption,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};

        PricingRequest mc_request{
            InstrumentKind::EquityAsianOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};

        std::cout << COLOR_BOLD << label << COLOR_RESET << "\n";
        print_result("Analytic Engine", registry.price(analytic_request));
        print_result("MC Engine", registry.price(mc_request));
    };

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "VANILLA OPTIONS PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";
    std::cout << "S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%, notional=1\n\n";

    price_vanilla("--- Vanilla Call Option ---", true);
    price_vanilla("--- Vanilla Put Option ---", false);

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "ASIAN OPTIONS PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";
    std::cout << "S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%, notional=1\n\n";

    price_asian("--- Arithmetic Asian Call Option ---", true, AsianAverageType::Arithmetic);
    price_asian("--- Arithmetic Asian Put Option ---", false, AsianAverageType::Arithmetic);
    price_asian("--- Geometric Asian Call Option ---", true, AsianAverageType::Geometric);
    price_asian("--- Geometric Asian Put Option ---", false, AsianAverageType::Geometric);

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "END OF PRICING REPORT" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";

    return 0;
}
