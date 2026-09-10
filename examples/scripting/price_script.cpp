// Price a payoff script from the command line.
//
//   cmake --preset default && cmake --build build --target qm_price_script
//   ./build/qm_price_script examples/scripting/european_call.qms
//   ./build/qm_price_script my_script.qms --spot 95 --vol 0.30 --sobol
//
// The script is priced with the single generic Monte-Carlo engine against a
// flat Black-Scholes model (one underlying, reachable as spot()). Every event
// date must fall strictly after the valuation date. See
// blueprint/wp/16-scripting.md for the language.

#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/scripting/script_error.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace quantModeling;

namespace
{
    struct Args
    {
        std::string script_path;
        double spot = 100.0;
        double rate = 0.03;
        double dividend = 0.0;
        double vol = 0.20;
        std::string valuation; // empty -> today
        int paths = 200000;
        int seed = 1;
        bool sobol = false;
    };

    [[noreturn]] void usage(int code)
    {
        std::cerr <<
            R"(usage: qm_price_script <script-file> [options]

  --spot F        spot of the single underlying   (default 100)
  --rate F        continuously-compounded rate     (default 0.03)
  --div  F        continuous dividend yield        (default 0)
  --vol  F        flat Black-Scholes volatility    (default 0.20)
  --valuation D   valuation date YYYY-MM-DD        (default: today)
  --paths N       Monte-Carlo paths                (default 200000)
  --seed N        RNG seed                         (default 1)
  --sobol         scrambled-Sobol RQMC instead of pseudo-random
  -h, --help
)";
        std::exit(code);
    }

    double need_double(int argc, char **argv, int &i, const char *flag)
    {
        if (++i >= argc)
        {
            std::cerr << flag << " needs a value\n";
            std::exit(2);
        }
        return std::atof(argv[i]);
    }

    int need_int(int argc, char **argv, int &i, const char *flag)
    {
        if (++i >= argc)
        {
            std::cerr << flag << " needs a value\n";
            std::exit(2);
        }
        return std::atoi(argv[i]);
    }

    Args parse_args(int argc, char **argv)
    {
        Args a;
        for (int i = 1; i < argc; ++i)
        {
            const char *arg = argv[i];
            if (!std::strcmp(arg, "-h") || !std::strcmp(arg, "--help"))
                usage(0);
            else if (!std::strcmp(arg, "--spot"))
                a.spot = need_double(argc, argv, i, arg);
            else if (!std::strcmp(arg, "--rate"))
                a.rate = need_double(argc, argv, i, arg);
            else if (!std::strcmp(arg, "--div"))
                a.dividend = need_double(argc, argv, i, arg);
            else if (!std::strcmp(arg, "--vol"))
                a.vol = need_double(argc, argv, i, arg);
            else if (!std::strcmp(arg, "--valuation"))
            {
                if (++i >= argc)
                    usage(2);
                a.valuation = argv[i];
            }
            else if (!std::strcmp(arg, "--paths"))
                a.paths = need_int(argc, argv, i, arg);
            else if (!std::strcmp(arg, "--seed"))
                a.seed = need_int(argc, argv, i, arg);
            else if (!std::strcmp(arg, "--sobol"))
                a.sobol = true;
            else if (arg[0] == '-')
            {
                std::cerr << "unknown option: " << arg << "\n";
                usage(2);
            }
            else if (a.script_path.empty())
                a.script_path = arg;
            else
            {
                std::cerr << "unexpected argument: " << arg << "\n";
                usage(2);
            }
        }
        if (a.script_path.empty())
            usage(2);
        return a;
    }

    std::string read_file(const std::string &path)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::cerr << "cannot open script file: " << path << "\n";
            std::exit(1);
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

int main(int argc, char **argv)
{
    const Args args = parse_args(argc, argv);
    const std::string script = read_file(args.script_path);

    const Date valuation = args.valuation.empty()
                               ? Date::today()
                               : Date::from_iso(args.valuation);

    try
    {
        const ValuationContext ctx{valuation};
        ScriptedProduct<Real> product(script, ctx);
        BlackScholesSimModel<Real> model(args.spot, args.rate, args.dividend,
                                         args.vol);

        PricingSettings settings;
        settings.mc_paths = args.paths;
        settings.mc_seed = args.seed;
        settings.mc_antithetic = true;
        settings.mc_sampler =
            args.sobol ? SamplerKind::Sobol : SamplerKind::PseudoRandom;
        settings.mc_rqmc_batches = 32;

        const auto res = simulate<Real>(product, model, settings);

        std::printf("── script ─────────────────────────────────────────────\n%s\n",
                    script.c_str());
        std::printf("── market ─────────────────────────────────────────────\n");
        std::printf("valuation %s   spot %.4g   rate %.3g%%   div %.3g%%   vol %.3g%%\n",
                    valuation.to_iso().c_str(), args.spot, 100 * args.rate,
                    100 * args.dividend, 100 * args.vol);
        std::printf("sampler %s   paths %d   seed %d\n",
                    args.sobol ? "sobol RQMC" : "pseudo-random", args.paths,
                    args.seed);

        std::printf("── timeline ───────────────────────────────────────────\n");
        for (const Time t : product.timeline())
            std::printf("  t = %.6f y\n", t);

        std::printf("── variables ──────────────────────────────────────────\n  ");
        if (product.variable_names().empty())
            std::printf("(none)");
        for (const std::string &name : product.variable_names())
            std::printf("%s  ", name.c_str());

        std::printf("\n── result ─────────────────────────────────────────────\n");
        std::printf("price       %.6f\n", res.npv());
        std::printf("std error   %.6f   (%.2f bp of price)\n", res.std_error(),
                    res.npv() != 0.0 ? 1e4 * res.std_error() / res.npv() : 0.0);
        std::printf("engine      %s\n", res.diagnostics.c_str());
        return 0;
    }
    catch (const scripting::ScriptError &e)
    {
        std::fprintf(stderr, "script error:\n%s\n", e.what());
        return 1;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
