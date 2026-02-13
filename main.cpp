#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/analytic/asian.hpp"
#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <Eigen/Dense>
#include <iostream>
#include <memory>
#include <iomanip>
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

    // Market parameters
    auto model = std::make_shared<BlackScholesModel>(100.0, 0.05, 0.02, 0.20);
    auto exercise = std::make_shared<EuropeanExercise>(1.0);
    MarketView market = {};

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "VANILLA OPTIONS PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";
    std::cout << "S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%, notional=1\n\n";

    // Vanilla Call
    {
        std::cout << COLOR_BOLD << "--- Vanilla Call Option ---" << COLOR_RESET << "\n";
        auto payoff = std::make_shared<PlainVanillaPayoff>(OptionType::Call, 100.0);
        VanillaOption opt(payoff, exercise, 1.0);

        PricingSettings settings_ana = {1'000'000, 1, 0.0};
        PricingContext ctx_ana{market, settings_ana, model};
        BSEuroVanillaAnalyticEngine engine_ana(ctx_ana);
        print_result("Analytic Engine", price(opt, engine_ana));

        PricingSettings settings_mc_std = {100'000, 42, 0.0};
        settings_mc_std.mc_antithetic = false;
        PricingContext ctx_mc_std{market, settings_mc_std, model};
        BSEuroVanillaMCEngine engine_mc_std(ctx_mc_std);
        print_result("MC Engine (standard)", price(opt, engine_mc_std));

        PricingSettings settings_mc_anti = {100'000, 42, 0.0};
        settings_mc_anti.mc_antithetic = true;
        PricingContext ctx_mc_anti{market, settings_mc_anti, model};
        BSEuroVanillaMCEngine engine_mc_anti(ctx_mc_anti);
        print_result("MC Engine (antithetic)", price(opt, engine_mc_anti));
    }

    // Vanilla Put
    {
        std::cout << COLOR_BOLD << "--- Vanilla Put Option ---" << COLOR_RESET << "\n";
        auto payoff = std::make_shared<PlainVanillaPayoff>(OptionType::Put, 100.0);
        VanillaOption opt(payoff, exercise, 1.0);

        PricingSettings settings_ana = {1'000'000, 1, 0.0};
        PricingContext ctx_ana{market, settings_ana, model};
        BSEuroVanillaAnalyticEngine engine_ana(ctx_ana);
        print_result("Analytic Engine", price(opt, engine_ana));

        PricingSettings settings_mc_std = {100'000, 42, 0.0};
        settings_mc_std.mc_antithetic = false;
        PricingContext ctx_mc_std{market, settings_mc_std, model};
        BSEuroVanillaMCEngine engine_mc_std(ctx_mc_std);
        print_result("MC Engine (standard)", price(opt, engine_mc_std));

        PricingSettings settings_mc_anti = {100'000, 42, 0.0};
        settings_mc_anti.mc_antithetic = true;
        PricingContext ctx_mc_anti{market, settings_mc_anti, model};
        BSEuroVanillaMCEngine engine_mc_anti(ctx_mc_anti);
        print_result("MC Engine (antithetic)", price(opt, engine_mc_anti));
    }

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "ASIAN OPTIONS PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";
    std::cout << "S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%, notional=1\n\n";

    // Arithmetic Asian Call
    {
        std::cout << COLOR_BOLD << "--- Arithmetic Asian Call Option ---" << COLOR_RESET << "\n";
        auto payoff = std::make_shared<ArithmeticAsianPayoff>(OptionType::Call, 100.0);
        AsianOption opt(payoff, exercise, AsianAverageType::Arithmetic, 1.0);

        PricingSettings settings_ana = {1'000'000, 1, 0.0};
        PricingContext ctx_ana{market, settings_ana, model};
        BSEuroArithmeticAsianAnalyticEngine engine_ana(ctx_ana);
        print_result("Analytic Engine (TW approx)", price(opt, engine_ana));

        PricingSettings settings_mc_std = {100'000, 42, 0.0};
        settings_mc_std.mc_antithetic = false;
        PricingContext ctx_mc_std{market, settings_mc_std, model};
        BSEuroAsianMCEngine engine_mc_std(ctx_mc_std);
        print_result("MC Engine (standard)", price(opt, engine_mc_std));

        PricingSettings settings_mc_anti = {100'000, 42, 0.0};
        settings_mc_anti.mc_antithetic = true;
        PricingContext ctx_mc_anti{market, settings_mc_anti, model};
        BSEuroAsianMCEngine engine_mc_anti(ctx_mc_anti);
        print_result("MC Engine (antithetic)", price(opt, engine_mc_anti));
    }

    // Arithmetic Asian Put
    {
        std::cout << COLOR_BOLD << "--- Arithmetic Asian Put Option ---" << COLOR_RESET << "\n";
        auto payoff = std::make_shared<ArithmeticAsianPayoff>(OptionType::Put, 100.0);
        AsianOption opt(payoff, exercise, AsianAverageType::Arithmetic, 1.0);

        PricingSettings settings_ana = {1'000'000, 1, 0.0};
        PricingContext ctx_ana{market, settings_ana, model};
        BSEuroArithmeticAsianAnalyticEngine engine_ana(ctx_ana);
        print_result("Analytic Engine (TW approx)", price(opt, engine_ana));

        PricingSettings settings_mc_std = {100'000, 42, 0.0};
        settings_mc_std.mc_antithetic = false;
        PricingContext ctx_mc_std{market, settings_mc_std, model};
        BSEuroAsianMCEngine engine_mc_std(ctx_mc_std);
        print_result("MC Engine (standard)", price(opt, engine_mc_std));

        PricingSettings settings_mc_anti = {100'000, 42, 0.0};
        settings_mc_anti.mc_antithetic = true;
        PricingContext ctx_mc_anti{market, settings_mc_anti, model};
        BSEuroAsianMCEngine engine_mc_anti(ctx_mc_anti);
        print_result("MC Engine (antithetic)", price(opt, engine_mc_anti));
    }

    // Geometric Asian Call
    {
        std::cout << COLOR_BOLD << "--- Geometric Asian Call Option ---" << COLOR_RESET << "\n";
        auto payoff = std::make_shared<GeometricAsianPayoff>(OptionType::Call, 100.0);
        AsianOption opt(payoff, exercise, AsianAverageType::Geometric, 1.0);

        PricingSettings settings_ana = {1'000'000, 1, 0.0};
        PricingContext ctx_ana{market, settings_ana, model};
        BSEuroGeometricAsianAnalyticEngine engine_ana(ctx_ana);
        print_result("Analytic Engine (closed-form)", price(opt, engine_ana));

        PricingSettings settings_mc_std = {100'000, 42, 0.0};
        settings_mc_std.mc_antithetic = false;
        PricingContext ctx_mc_std{market, settings_mc_std, model};
        BSEuroAsianMCEngine engine_mc_std(ctx_mc_std);
        print_result("MC Engine (standard)", price(opt, engine_mc_std));

        PricingSettings settings_mc_anti = {100'000, 42, 0.0};
        settings_mc_anti.mc_antithetic = true;
        PricingContext ctx_mc_anti{market, settings_mc_anti, model};
        BSEuroAsianMCEngine engine_mc_anti(ctx_mc_anti);
        print_result("MC Engine (antithetic)", price(opt, engine_mc_anti));
    }

    // Geometric Asian Put
    {
        std::cout << COLOR_BOLD << "--- Geometric Asian Put Option ---" << COLOR_RESET << "\n";
        auto payoff = std::make_shared<GeometricAsianPayoff>(OptionType::Put, 100.0);
        AsianOption opt(payoff, exercise, AsianAverageType::Geometric, 1.0);

        PricingSettings settings_ana = {1'000'000, 1, 0.0};
        PricingContext ctx_ana{market, settings_ana, model};
        BSEuroGeometricAsianAnalyticEngine engine_ana(ctx_ana);
        print_result("Analytic Engine (closed-form)", price(opt, engine_ana));

        PricingSettings settings_mc_std = {100'000, 42, 0.0};
        settings_mc_std.mc_antithetic = false;
        PricingContext ctx_mc_std{market, settings_mc_std, model};
        BSEuroAsianMCEngine engine_mc_std(ctx_mc_std);
        print_result("MC Engine (standard)", price(opt, engine_mc_std));

        PricingSettings settings_mc_anti = {100'000, 42, 0.0};
        settings_mc_anti.mc_antithetic = true;
        PricingContext ctx_mc_anti{market, settings_mc_anti, model};
        BSEuroAsianMCEngine engine_mc_anti(ctx_mc_anti);
        print_result("MC Engine (antithetic)", price(opt, engine_mc_anti));
    }

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "END OF PRICING REPORT" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";

    return 0;
}
