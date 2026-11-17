#include "quantModeling/engines/mc/path_distribution.hpp"

#include "quantModeling/core/sample.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace quantModeling
{

    namespace
    {

        constexpr Real kNaN = std::numeric_limits<Real>::quiet_NaN();

        /// Sum and sum of squares of one estimator, over the paths.
        struct Moments
        {
            Real sum = 0.0, sum2 = 0.0;

            void add(Real x)
            {
                sum += x;
                sum2 += x * x;
            }
            Real mean(Real n) const { return sum / n; }
            /// Standard error of the mean (sample variance, n - 1).
            Real stderr_of_mean(Real n) const
            {
                const Real m = sum / n;
                return std::sqrt(std::max((sum2 - n * m * m) / (n - 1.0), 0.0) / n);
            }
        };

        /// The q-quantile of sorted values, linear between order statistics
        /// (Hyndman and Fan's definition 7).
        Real quantile_sorted(const std::vector<Real> &sorted, Real q)
        {
            const Real h = q * static_cast<Real>(sorted.size() - 1);
            const auto lo = static_cast<std::size_t>(std::floor(h));
            const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
            return sorted[lo] + (h - static_cast<Real>(lo)) * (sorted[hi] - sorted[lo]);
        }

        /// Out-of-the-money payoff: the put below the forward, the call from it up.
        Real otm_payoff(Real x, Real strike, Real forward)
        {
            return strike < forward ? std::max(strike - x, 0.0) : std::max(x - strike, 0.0);
        }

        /// One strike grid's estimators, turned into prices and implied vols.
        /// `price_of_call` converts an out-of-the-money price into a call's
        /// (put-call parity on the forward: C = P + df (F - K)).
        SmileEstimate finish_smile(std::size_t date, Real ttm, Real forward, Real df,
                                   const std::vector<Real> &moneyness, const std::vector<Moments> &acc, Real n)
        {
            SmileEstimate s;
            s.date = date;
            s.ttm = ttm;
            s.forward = forward;
            s.moneyness = moneyness;
            for (std::size_t j = 0; j < moneyness.size(); ++j)
            {
                const Real K = moneyness[j] * forward;
                const Real price = acc[j].mean(n);
                const Real se = acc[j].stderr_of_mean(n);
                const Real call = K < forward ? price + df * (forward - K) : price;
                const Real iv = black76_implied_vol(call, forward, K, ttm, df);
                Real iv_se = kNaN;
                if (std::isfinite(iv))
                {
                    const Real vega = black76_vega(forward, K, ttm, iv, df);
                    if (vega > 0.0)
                        iv_se = se / vega;
                }
                s.strikes.push_back(K);
                s.prices.push_back(price);
                s.price_stderr.push_back(se);
                s.implied_vols.push_back(iv);
                s.iv_stderr.push_back(iv_se);
            }
            return s;
        }

        void check(const DistributionSettings &s, Real spot, Real ttm)
        {
            if (!(ttm > 0.0) || !(spot > 0.0) || s.n_steps < 1 || s.n_paths < 2)
                throw InvalidInput("simulate_path_distribution: need spot > 0, ttm > 0, n_steps >= 1, n_paths >= 2");
            if (s.n_display < 0 || s.n_display > s.n_paths)
                throw InvalidInput("simulate_path_distribution: n_display must be in [0, n_paths]");
            if (s.n_bins < 1)
                throw InvalidInput("simulate_path_distribution: n_bins must be >= 1");
            for (Real q : s.quantiles)
                if (!(q > 0.0 && q < 1.0))
                    throw InvalidInput("simulate_path_distribution: quantile levels must be in (0, 1)");
            for (std::size_t d : s.smile_dates)
                if (d < 1 || d > s.n_steps)
                    throw InvalidInput("simulate_path_distribution: smile dates must be in [1, n_steps]");
            for (Real m : s.smile_moneyness)
                if (!(m > 0.0))
                    throw InvalidInput("simulate_path_distribution: moneyness must be > 0");
            if (s.forward_start != 0 && (s.forward_start >= s.n_steps || s.smile_moneyness.empty()))
                throw InvalidInput(
                    "simulate_path_distribution: a forward smile needs forward_start in [1, n_steps) and moneyness");
            if (s.barrier != 0.0 && !(s.barrier_strike > 0.0 && s.barrier > 1.0 && s.barrier > s.barrier_strike))
                throw InvalidInput(
                    "simulate_path_distribution: an up-and-out call needs barrier > 1 (the spot) and barrier > strike > 0");
        }

    } // namespace

    PathDistribution simulate_path_distribution(
        ISimulationModel<Real> &model, Real spot, Real r, Real q, Real ttm, const DistributionSettings &settings)
    {
        check(settings, spot, ttm);
        const std::size_t n = settings.n_steps;
        const auto n_paths = static_cast<std::size_t>(settings.n_paths);
        const Real np = static_cast<Real>(n_paths);

        // The grid and the gaussians exactly as simulate_model_paths.
        TimeLine timeline(n);
        for (std::size_t i = 0; i < n; ++i)
            timeline[i] = ttm * static_cast<Real>(i + 1) / static_cast<Real>(n);
        std::vector<SampleDef> defline(n);
        for (SampleDef &d : defline)
            d.inst_vol = true;
        model.init(timeline, defline);

        PathDistribution out;
        out.n_paths = settings.n_paths;
        out.time_grid.reserve(n + 1);
        out.time_grid.push_back(0.0);
        out.time_grid.insert(out.time_grid.end(), timeline.begin(), timeline.end());
        const std::vector<Real> &tg = out.time_grid;

        std::vector<Real> forward(n + 1), df(n + 1);
        for (std::size_t i = 0; i <= n; ++i)
        {
            forward[i] = spot * std::exp((r - q) * tg[i]);
            df[i] = std::exp(-r * tg[i]);
        }

        // Spots and vols by date, for the quantiles and histograms.
        std::vector<std::vector<Real>> spots(n + 1, std::vector<Real>(n_paths));
        std::vector<std::vector<Real>> vols(n + 1, std::vector<Real>(n_paths, kNaN));
        std::fill(spots[0].begin(), spots[0].end(), spot);

        std::vector<Moments> deflated(n + 1), atm(n + 1);
        std::vector<std::vector<Moments>> smile_acc(settings.smile_dates.size(),
                                                    std::vector<Moments>(settings.smile_moneyness.size()));
        const Real tau = ttm - (settings.forward_start ? tg[settings.forward_start] : 0.0);
        const Real fwd_ratio = std::exp((r - q) * tau);
        std::vector<Moments> fwd_acc(settings.forward_start ? settings.smile_moneyness.size() : 0);
        Moments barrier_acc, vanilla_acc;
        const bool has_barrier = settings.barrier != 0.0;
        const Real B = settings.barrier * spot, Kb = settings.barrier_strike * spot;

        Scenario<Real> scenario;
        allocate_scenario(scenario, defline, model.n_underlyings());
        std::vector<double> gaussians(model.sim_dim());
        Pcg32 rng = RngFactory(settings.seed).make(0);
        NormalBoxMuller bm;
        bool vol_filled = true;
        for (std::size_t p = 0; p < n_paths; ++p)
        {
            for (double &g : gaussians)
                g = bm(rng);
            for (Sample<Real> &s : scenario)
                s.inst_vol = kNaN;
            model.generate_path(gaussians, scenario);

            deflated[0].add(spot);
            Real running_max = spot;
            for (std::size_t i = 1; i <= n; ++i)
            {
                const Sample<Real> &smp = scenario[i - 1];
                const Real S = smp.spots.front();
                spots[i][p] = S;
                vols[i][p] = smp.inst_vol;
                vol_filled = vol_filled && std::isfinite(smp.inst_vol);
                deflated[i].add(S / smp.numeraire);
                atm[i].add(std::max(S - forward[i], 0.0) / smp.numeraire);
                running_max = std::max(running_max, S);
            }
            for (std::size_t d = 0; d < settings.smile_dates.size(); ++d)
            {
                const std::size_t i = settings.smile_dates[d];
                const Sample<Real> &smp = scenario[i - 1];
                for (std::size_t j = 0; j < settings.smile_moneyness.size(); ++j)
                    smile_acc[d][j].add(
                        otm_payoff(smp.spots.front(), settings.smile_moneyness[j] * forward[i], forward[i]) /
                        smp.numeraire);
            }
            const Sample<Real> &last = scenario[n - 1];
            if (settings.forward_start)
            {
                const Real ratio = last.spots.front() / scenario[settings.forward_start - 1].spots.front();
                for (std::size_t j = 0; j < fwd_acc.size(); ++j)
                    fwd_acc[j].add(otm_payoff(ratio, settings.smile_moneyness[j] * fwd_ratio, fwd_ratio) /
                                   last.numeraire);
            }
            if (has_barrier)
            {
                const Real call = std::max(last.spots.front() - Kb, 0.0) / last.numeraire;
                vanilla_acc.add(call);
                barrier_acc.add(running_max < B ? call : 0.0);
            }
        }

        // Display paths: the first ones, whole.
        const auto n_display = static_cast<std::size_t>(settings.n_display);
        out.paths.assign(n_display, std::vector<Real>(n + 1));
        out.vols.assign(n_display, std::vector<Real>(n + 1, kNaN));
        for (std::size_t p = 0; p < n_display; ++p)
            for (std::size_t i = 0; i <= n; ++i)
            {
                out.paths[p][i] = spots[i][p];
                if (vol_filled)
                    out.vols[p][i] = vols[i][p];
            }

        // Quantiles and histograms, date by date.
        out.has_vol = vol_filled;
        out.quantile_levels = settings.quantiles;
        const std::size_t nq = settings.quantiles.size();
        out.spot_quantiles.assign(nq, std::vector<Real>(n + 1));
        if (vol_filled)
            out.vol_quantiles.assign(nq, std::vector<Real>(n + 1, kNaN));
        out.hist_lo.assign(n + 1, spot);
        out.hist_hi.assign(n + 1, spot);
        out.hist_counts.assign(n + 1, std::vector<long long>(settings.n_bins, 0));
        for (std::size_t i = 0; i <= n; ++i)
        {
            std::vector<Real> &col = spots[i];
            std::sort(col.begin(), col.end());
            for (std::size_t k = 0; k < nq; ++k)
                out.spot_quantiles[k][i] = quantile_sorted(col, settings.quantiles[k]);
            if (i == 0)
                continue;
            const Real lo = quantile_sorted(col, 0.005), hi = quantile_sorted(col, 0.995);
            out.hist_lo[i] = lo;
            out.hist_hi[i] = hi;
            if (hi > lo)
            {
                const Real width = (hi - lo) / static_cast<Real>(settings.n_bins);
                for (Real x : col)
                    if (x >= lo && x <= hi)
                    {
                        const auto b = std::min(static_cast<std::size_t>((x - lo) / width), settings.n_bins - 1);
                        ++out.hist_counts[i][b];
                    }
            }
            if (vol_filled)
            {
                std::vector<Real> &vc = vols[i];
                std::sort(vc.begin(), vc.end());
                for (std::size_t k = 0; k < nq; ++k)
                    out.vol_quantiles[k][i] = quantile_sorted(vc, settings.quantiles[k]);
            }
        }

        // The martingale check and the at-the-money term structure.
        out.deflated_mean.resize(n + 1);
        out.deflated_stderr.resize(n + 1);
        out.deflated_exact.resize(n + 1);
        out.atm_vol.assign(n + 1, kNaN);
        out.atm_vol_stderr.assign(n + 1, kNaN);
        for (std::size_t i = 0; i <= n; ++i)
        {
            out.deflated_mean[i] = deflated[i].mean(np);
            out.deflated_stderr[i] = i == 0 ? 0.0 : deflated[i].stderr_of_mean(np);
            out.deflated_exact[i] = spot * std::exp(-q * tg[i]);
            if (i == 0)
                continue;
            const Real price = atm[i].mean(np);
            const Real iv = black76_implied_vol(price, forward[i], forward[i], tg[i], df[i]);
            out.atm_vol[i] = iv;
            if (std::isfinite(iv))
            {
                const Real vega = black76_vega(forward[i], forward[i], tg[i], iv, df[i]);
                if (vega > 0.0)
                    out.atm_vol_stderr[i] = atm[i].stderr_of_mean(np) / vega;
            }
        }

        for (std::size_t d = 0; d < settings.smile_dates.size(); ++d)
        {
            const std::size_t i = settings.smile_dates[d];
            out.smiles.push_back(finish_smile(i, tg[i], forward[i], df[i], settings.smile_moneyness, smile_acc[d], np));
        }
        if (settings.forward_start)
        {
            // E[(S_T / S_t1 - K)^+ / N_T] = e^{-rT} E[(R - K)^+]: Black-76 on
            // the ratio's forward over T - t1, discounted over the whole of T.
            out.has_forward_smile = true;
            out.forward_smile =
                finish_smile(settings.forward_start, tau, fwd_ratio, df[n], settings.smile_moneyness, fwd_acc, np);
        }
        if (has_barrier)
        {
            out.has_barrier = true;
            out.barrier_price = barrier_acc.mean(np);
            out.barrier_stderr = barrier_acc.stderr_of_mean(np);
            out.vanilla_price = vanilla_acc.mean(np);
            out.vanilla_stderr = vanilla_acc.stderr_of_mean(np);
        }
        return out;
    }

} // namespace quantModeling
