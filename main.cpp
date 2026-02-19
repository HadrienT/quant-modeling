#include "quantModeling/pricers/registry.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace quantModeling
{
    // ANSI color codes
    constexpr const char *COLOR_RESET = "\033[0m";
    constexpr const char *COLOR_BOLD = "\033[1m";
    constexpr const char *COLOR_GREEN = "\033[32m";
    constexpr const char *COLOR_YELLOW = "\033[33m";
    constexpr const char *COLOR_BLUE = "\033[34m";
    constexpr const char *COLOR_CYAN = "\033[36m";
    constexpr const char *COLOR_RED = "\033[31m";
    constexpr const char *COLOR_MAGENTA = "\033[35m";

    // Helper function to print pricing results
    void print_result(const std::string &name, const PricingResult &res)
    {
        std::cout << "  " << COLOR_CYAN << std::left << std::setw(40) << name << COLOR_RESET
                  << " | NPV: " << COLOR_YELLOW << std::right << std::setw(10) << std::fixed << std::setprecision(4) << res.npv << COLOR_RESET;
        if (res.mc_std_error != 0.0)
            std::cout << " (±" << std::fixed << std::setprecision(4) << res.mc_std_error << ")";
        std::cout << "\n";
        auto fmt = [&](const std::optional<Real> &val, const std::optional<Real> &se)
        {
            if (!val)
                return std::string("N/A");
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(4) << *val;
            if (se && *se != 0.0)
                ss << " (±" << std::fixed << std::setprecision(4) << *se << ")";
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

    // Test American vanilla pricing: compare Binomial and Trinomial methods
    void test_american_vanilla(Real S, Real K, Time T, Real r, Real q, Real sigma,
                               bool is_call, const std::string &description)
    {
        const auto &registry = default_registry();

        AmericanVanillaBSInput in{S, K, T, r, q, sigma, is_call, 100, 100, 100};

        PricingRequest binomial_req{InstrumentKind::EquityAmericanVanillaOption,
                                    ModelKind::BlackScholes,
                                    EngineKind::BinomialTree,
                                    PricingInput{in}};

        PricingRequest trinomial_req{InstrumentKind::EquityAmericanVanillaOption,
                                     ModelKind::BlackScholes,
                                     EngineKind::TrinomialTree,
                                     PricingInput{in}};

        PricingRequest euro_req{InstrumentKind::EquityVanillaOption,
                                ModelKind::BlackScholes,
                                EngineKind::Analytic,
                                PricingInput{VanillaBSInput{S, K, T, r, q, sigma, is_call}}};

        auto binomial_result = registry.price(binomial_req);
        auto trinomial_result = registry.price(trinomial_req);
        auto euro_result = registry.price(euro_req);

        std::cout << COLOR_BOLD << description << COLOR_RESET << "\n";
        print_result("European Analytic (reference)", euro_result);
        print_result("American Binomial (100 steps)", binomial_result);
        print_result("American Trinomial (100 steps)", trinomial_result);

        // Coherence analysis
        const Real american_avg = (binomial_result.npv + trinomial_result.npv) / 2.0;
        const Real early_exercise_premium = american_avg - euro_result.npv;
        const Real max_diff = std::abs(binomial_result.npv - trinomial_result.npv);

        std::cout << "  " << COLOR_MAGENTA << "Coherence Check:" << COLOR_RESET << "\n";
        std::cout << "    Early exercise premium (American - European): " << COLOR_YELLOW
                  << std::fixed << std::setprecision(4) << early_exercise_premium << COLOR_RESET;
        if (early_exercise_premium >= -0.0001) // Allow rounding error
            std::cout << COLOR_GREEN << " ✓" << COLOR_RESET;
        else
            std::cout << COLOR_RED << " ✗ (Invalid!)" << COLOR_RESET;
        std::cout << "\n";

        std::cout << "    Max difference between Binomial/Trinomial: " << COLOR_YELLOW
                  << std::fixed << std::setprecision(4) << max_diff << COLOR_RESET;
        if (max_diff < 0.05)
            std::cout << COLOR_GREEN << " ✓ (Good)" << COLOR_RESET;
        else if (max_diff < 0.2)
            std::cout << COLOR_YELLOW << " ~ (Acceptable)" << COLOR_RESET;
        else
            std::cout << COLOR_RED << " ✗ (Poor)" << COLOR_RESET;
        std::cout << "\n\n";
    }

    // Test European vanilla pricing: compare Analytic, MonteCarlo, and PDE methods
    void test_european_vanilla(Real S, Real K, Time T, Real r, Real q, Real sigma,
                               bool is_call, const std::string &description)
    {
        const auto &registry = default_registry();

        VanillaBSInput in{S, K, T, r, q, sigma, is_call};
        in.n_paths = 100'000;
        in.seed = 42;
        in.mc_epsilon = 0.0;

        PricingRequest analytic_req{InstrumentKind::EquityVanillaOption,
                                    ModelKind::BlackScholes,
                                    EngineKind::Analytic,
                                    PricingInput{in}};

        PricingRequest mc_req{InstrumentKind::EquityVanillaOption,
                              ModelKind::BlackScholes,
                              EngineKind::MonteCarlo,
                              PricingInput{in}};

        PricingRequest pde_req{InstrumentKind::EquityVanillaOption,
                               ModelKind::BlackScholes,
                               EngineKind::PDEFiniteDifference,
                               PricingInput{in}};

        auto analytic_result = registry.price(analytic_req);
        auto mc_result = registry.price(mc_req);
        auto pde_result = registry.price(pde_req);

        std::cout << COLOR_BOLD << description << COLOR_RESET << "\n";
        print_result("Analytic (reference)", analytic_result);
        print_result("Monte Carlo (100k paths)", mc_result);
        print_result("PDE Crank-Nicolson (100×100)", pde_result);

        // Coherence analysis
        const Real max_diff_vs_analytic = std::max(
            std::abs(pde_result.npv - analytic_result.npv),
            std::abs(mc_result.npv - analytic_result.npv));

        std::cout << "  " << COLOR_MAGENTA << "Convergence Check:" << COLOR_RESET << "\n";
        std::cout << "    Max difference vs Analytic: " << COLOR_YELLOW
                  << std::fixed << std::setprecision(4) << max_diff_vs_analytic << COLOR_RESET;
        if (max_diff_vs_analytic < 0.05)
            std::cout << COLOR_GREEN << " ✓ (Good)" << COLOR_RESET;
        else if (max_diff_vs_analytic < 0.2)
            std::cout << COLOR_YELLOW << " ~ (Acceptable)" << COLOR_RESET;
        else
            std::cout << COLOR_RED << " ✗ (Poor)" << COLOR_RESET;
        std::cout << "\n\n";
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

    // const auto price_vanilla = [&](const std::string &label, bool is_call)
    // {
    //     VanillaBSInput in{spot, strike, maturity, rate, dividend, vol, is_call};
    //     in.n_paths = 100'000;
    //     in.seed = 42;
    //     in.mc_epsilon = 0.0;

    //     PricingRequest analytic_request{
    //         InstrumentKind::EquityVanillaOption,
    //         ModelKind::BlackScholes,
    //         EngineKind::Analytic,
    //         PricingInput{in}};

    //     PricingRequest mc_request{
    //         InstrumentKind::EquityVanillaOption,
    //         ModelKind::BlackScholes,
    //         EngineKind::MonteCarlo,
    //         PricingInput{in}};

    //     std::cout << COLOR_BOLD << label << COLOR_RESET << "\n";
    //     print_result("Analytic Engine", registry.price(analytic_request));
    //     print_result("MC Engine", registry.price(mc_request));
    // };

    // const auto price_asian = [&](const std::string &label, bool is_call, AsianAverageType average_type)
    // {
    //     AsianBSInput in{spot, strike, maturity, rate, dividend, vol, is_call, average_type};
    //     in.n_paths = 100'000;
    //     in.seed = 42;
    //     in.mc_epsilon = 0.0;

    //     PricingRequest analytic_request{
    //         InstrumentKind::EquityAsianOption,
    //         ModelKind::BlackScholes,
    //         EngineKind::Analytic,
    //         PricingInput{in}};

    //     PricingRequest mc_request{
    //         InstrumentKind::EquityAsianOption,
    //         ModelKind::BlackScholes,
    //         EngineKind::MonteCarlo,
    //         PricingInput{in}};

    //     std::cout << COLOR_BOLD << label << COLOR_RESET << "\n";
    //     print_result("Analytic Engine", registry.price(analytic_request));
    //     print_result("MC Engine", registry.price(mc_request));
    // };

    // std::cout << "==============================================\n";
    // std::cout << COLOR_BOLD << COLOR_GREEN << "VANILLA OPTIONS PRICING" << COLOR_RESET << "\n";
    // std::cout << "==============================================\n";
    // std::cout << "S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%, notional=1\n\n";

    // price_vanilla("--- Vanilla Call Option ---", true);
    // price_vanilla("--- Vanilla Put Option ---", false);

    // std::cout << "==============================================\n";
    // std::cout << COLOR_BOLD << COLOR_GREEN << "ASIAN OPTIONS PRICING" << COLOR_RESET << "\n";
    // std::cout << "==============================================\n";
    // std::cout << "S0=100, K=100, T=1y, r=5%, q=2%, sigma=20%, notional=1\n\n";

    // price_asian("--- Arithmetic Asian Call Option ---", true, AsianAverageType::Arithmetic);
    // price_asian("--- Arithmetic Asian Put Option ---", false, AsianAverageType::Arithmetic);
    // price_asian("--- Geometric Asian Call Option ---", true, AsianAverageType::Geometric);
    // price_asian("--- Geometric Asian Put Option ---", false, AsianAverageType::Geometric);

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "AMERICAN VANILLA OPTIONS PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";
    std::cout << "Testing coherence between Binomial and Trinomial methods\n";
    std::cout << COLOR_YELLOW << "(Note: PDE is European-only in refactored architecture)\n"
              << COLOR_RESET << "\n";

    // Test 1: ATM call (where early exercise is less likely)
    {
        test_american_vanilla(100.0, 100.0, 1.0, 0.05, 0.02, 0.20, true,
                              "--- Test 1: ATM Call (S=K=100, T=1y, r=5%, q=2%, σ=20%) ---");
    }

    // Test 2: ITM put (early exercise is significant)
    {
        std::cout << COLOR_YELLOW << "[Early exercise premium should be significant for puts]\n"
                  << COLOR_RESET;
        test_american_vanilla(90.0, 100.0, 1.0, 0.05, 0.02, 0.20, false,
                              "--- Test 2: ITM Put (S=90, K=100, T=1y, r=5%, q=2%, σ=20%) ---");
    }

    // Test 3: OTM call
    {
        test_american_vanilla(90.0, 100.0, 1.0, 0.05, 0.02, 0.20, true,
                              "--- Test 3: OTM Call (S=90, K=100, T=1y, r=5%, q=2%, σ=20%) ---");
    }

    // Test 4: Short maturity put
    {
        std::cout << COLOR_YELLOW << "[Shorter maturity should increase early exercise premium]\n"
                  << COLOR_RESET;
        test_american_vanilla(95.0, 100.0, 0.25, 0.05, 0.02, 0.20, false,
                              "--- Test 4: Short Maturity Put (S=95, K=100, T=3m, r=5%, q=2%, σ=20%) ---");
    }

    // Test 5: High dividend yield on call
    {
        std::cout << COLOR_YELLOW << "[High dividend yield increases call early exercise value]\n"
                  << COLOR_RESET;
        test_american_vanilla(100.0, 100.0, 1.0, 0.05, 0.08, 0.20, true,
                              "--- Test 5: High Dividend Yield Call (S=100, K=100, T=1y, r=5%, q=8%, σ=20%) ---");
    }

    // Test 6: High volatility put
    {
        std::cout << COLOR_YELLOW << "[Higher volatility reduces early exercise premium]\n"
                  << COLOR_RESET;
        test_american_vanilla(95.0, 100.0, 1.0, 0.05, 0.02, 0.40, false,
                              "--- Test 6: High Volatility Put (S=95, K=100, T=1y, r=5%, q=2%, σ=40%) ---");
    }

    // Test 7: Trinomial convergence study
    {
        std::cout << COLOR_BOLD << "--- Test 7: Convergence Study (ATM Put, varying trinomial grid size) ---" << COLOR_RESET << "\n";
        std::cout << "    Trinomial grid convergence:\n";

        for (int steps : {20, 40, 80, 160})
        {
            AmericanVanillaBSInput in{100.0, 100.0, 1.0, 0.05, 0.02, 0.20, false, steps};
            PricingRequest req{InstrumentKind::EquityAmericanVanillaOption,
                               ModelKind::BlackScholes,
                               EngineKind::TrinomialTree,
                               PricingInput{in}};
            auto res = default_registry().price(req);
            std::cout << "      Steps=" << std::setw(3) << steps << " => Price=" << std::fixed
                      << std::setprecision(4) << res.npv << " (delta=" << std::setprecision(3)
                      << *res.greeks.delta << ")\n";
        }
        std::cout << "\n";
    }

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "EUROPEAN VANILLA OPTIONS PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";
    std::cout << "Testing consistency between Analytic, Monte Carlo, and PDE methods\n\n";

    // Test 1: ATM call
    {
        test_european_vanilla(100.0, 100.0, 1.0, 0.05, 0.02, 0.20, true,
                              "--- Test 1: ATM Call (S=K=100, T=1y, r=5%, q=2%, σ=20%) ---");
    }

    // Test 2: ITM put
    {
        test_european_vanilla(90.0, 100.0, 1.0, 0.05, 0.02, 0.20, false,
                              "--- Test 2: ITM Put (S=90, K=100, T=1y, r=5%, q=2%, σ=20%) ---");
    }

    // Test 3: OTM call
    {
        test_european_vanilla(90.0, 100.0, 1.0, 0.05, 0.02, 0.20, true,
                              "--- Test 3: OTM Call (S=90, K=100, T=1y, r=5%, q=2%, σ=20%) ---");
    }

    // Test 4: Short maturity put
    {
        test_european_vanilla(95.0, 100.0, 0.25, 0.05, 0.02, 0.20, false,
                              "--- Test 4: Short Maturity Put (S=95, K=100, T=3m, r=5%, q=2%, σ=20%) ---");
    }

    // Test 5: High volatility
    {
        test_european_vanilla(95.0, 100.0, 1.0, 0.05, 0.02, 0.40, false,
                              "--- Test 5: High Volatility Put (S=95, K=100, T=1y, r=5%, q=2%, σ=40%) ---");
    }

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "BOND PRICING" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";

    ZeroCouponBondInput zc_flat{};
    zc_flat.maturity = 2.0;
    zc_flat.rate = 0.035;
    zc_flat.notional = 1000.0;

    ZeroCouponBondInput zc_curve = zc_flat;
    zc_curve.discount_times = {0.5, 1.0, 2.0, 3.0, 5.0};
    zc_curve.discount_factors = {0.985, 0.97, 0.94, 0.915, 0.88};

    PricingRequest zc_flat_request{
        InstrumentKind::ZeroCouponBond,
        ModelKind::FlatRate,
        EngineKind::Analytic,
        PricingInput{zc_flat}};

    PricingRequest zc_curve_request{
        InstrumentKind::ZeroCouponBond,
        ModelKind::FlatRate,
        EngineKind::Analytic,
        PricingInput{zc_curve}};

    std::cout << COLOR_BOLD << "--- Zero-coupon bond (flat rate) ---" << COLOR_RESET << "\n";
    print_result("Analytic Engine", registry.price(zc_flat_request));

    std::cout << COLOR_BOLD << "--- Zero-coupon bond (curve) ---" << COLOR_RESET << "\n";
    print_result("Analytic Engine", registry.price(zc_curve_request));

    FixedRateBondInput fixed_flat{};
    fixed_flat.maturity = 3.0;
    fixed_flat.rate = 0.032;
    fixed_flat.coupon_rate = 0.045;
    fixed_flat.coupon_frequency = 2;
    fixed_flat.notional = 1000.0;

    FixedRateBondInput fixed_curve = fixed_flat;
    fixed_curve.discount_times = {0.5, 1.0, 2.0, 3.0, 5.0};
    fixed_curve.discount_factors = {0.988, 0.975, 0.945, 0.92, 0.885};

    PricingRequest fixed_flat_request{
        InstrumentKind::FixedRateBond,
        ModelKind::FlatRate,
        EngineKind::Analytic,
        PricingInput{fixed_flat}};

    PricingRequest fixed_curve_request{
        InstrumentKind::FixedRateBond,
        ModelKind::FlatRate,
        EngineKind::Analytic,
        PricingInput{fixed_curve}};

    std::cout << COLOR_BOLD << "--- Fixed-rate bond (flat rate) ---" << COLOR_RESET << "\n";
    print_result("Analytic Engine", registry.price(fixed_flat_request));

    std::cout << COLOR_BOLD << "--- Fixed-rate bond (curve) ---" << COLOR_RESET << "\n";
    print_result("Analytic Engine", registry.price(fixed_curve_request));

    std::cout << "==============================================\n";
    std::cout << COLOR_BOLD << COLOR_GREEN << "END OF PRICING REPORT" << COLOR_RESET << "\n";
    std::cout << "==============================================\n";

    return 0;
}
