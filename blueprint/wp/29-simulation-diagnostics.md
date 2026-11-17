# WP 29 — The simulation page as a model diagnostic

Status: in progress. Depends on WP 08 (market data, stored option chains), WP 16
(the script models: local vol, Heston, SLV) and WP 28 (the page sits in the
Learn space, and takes the shared underlying).

## 1. Why

The simulation page draws about thirty paths of a chosen model and animates them.
That shows what a dynamic looks like, and teaches nothing about it:

| What is missing | Consequence |
|---|---|
| The law of the spot | Thirty paths cannot show a quantile, a skew or a fat tail |
| The volatility | Heston, SLV and SABR are about the vol process, and the page only plots S |
| The market | The model is calibrated on a ticker, and nothing shows whether it reprices that ticker's smile |
| A comparison | Local vol and stochastic vol fit the same smile today and disagree on everything that depends on the path; the page cannot show it |

The page already has what it takes: real calibrations on the stored chain (SVI,
Dupire, Heston, SLV leverage) and the very models the script pricer prices with.

## 2. Principles

- **The pricing models, not a display scheme.** Every statistic is measured on
  the timeline models of `models/equity/` (exact Black-Scholes, log-Euler local
  vol, full-truncation Euler Heston, SLV, SABR with exact alpha), on their own
  internal steps: what the page shows is what a script would price with. The
  old `simulate_black_scholes_paths` / `simulate_sabr_paths` stay for the existing
  endpoints only.
- **Every number with its error** (the rule that displayed metrics must be
  defensible): each Monte Carlo price comes with its standard error, each
  implied vol with `stderr / vega`, the martingale check with its exact target.
- **Cost bounded for a public site**: at most 20,000 paths per request,
  single-threaded, a few hundred milliseconds for the slowest model (SLV, one
  year, daily steps).

## 3. Engine: `simulate_path_distribution` (lot S1)

`engines/mc/path_distribution.hpp`. One pass over the paths, then per date:

| Output | Definition |
|---|---|
| Display paths | The first `n_display` paths, drawn exactly as `simulate_model_paths` (tested equal) |
| Spot quantiles | Linear between order statistics (Hyndman–Fan 7), at each date |
| Histogram of S_t | `n_bins` equal bins between the 0.5% and 99.5% quantiles, at each date |
| Instantaneous vol | `Sample::inst_vol`, asked for by `SampleDef::inst_vol`: sigma(t) of d ln S on the path's state at t. Black-Scholes sigma; local vol sigma_loc(S_t, t); Heston sqrt(v_t+); SLV L(S_t, t) sqrt(v_t+); SABR alpha_t F_t^(beta-1), 0 once absorbed. Quantiles and display paths, NaN at t = 0 |
| Martingale check | E[S_t / N_t] with its standard error against S0 e^(-q t) |
| ATM vol term structure | Black-76 implied vol of the at-the-money-forward call at each date |
| Vanilla smiles | Out-of-the-money options (puts below the forward) at chosen dates and moneyness K / F(0, t), inverted with Black-76 |
| Forward smile | Options on S_T / S_t1 struck at m e^((r-q)(T-t1)): E[e^(-rT) (R - k)^+] is Black-76 on the ratio over T - t1, discounted over T |
| Up-and-out call | Barrier checked on the grid dates, and the vanilla call on the same paths |

`SampleDef::inst_vol` is off for pricing: a model reads one boolean per event
date, and does the lookup only when asked.

Tests (`tests/testPathDistribution.cpp`), properties rather than numbers: the
Black-Scholes spot smile, forward smile and ATM term structure flat at sigma
within 4 standard errors; Black-Scholes quantiles equal to the lognormal ones;
the deflated spot a martingale under local vol and Heston; the Heston smile equal
to the COS one, with its skew; SABR with beta = 1 and nu = 0 equal to
Black-Scholes; the up-and-out call below the call, and equal to it for a far
barrier on the same paths.

## 4. API (lot S2)

- `POST /api/simulation/distribution`: the same model inputs as the path
  endpoints (manual parameters, or a ticker's calibration), plus the smile dates
  and moneyness. Returns the engine's output, NaN as `null`.
- The **market side**, when the model is calibrated on a ticker: the stored SVI
  surface's implied vols at the same dates and strikes, and the risk-neutral
  density of S_t by Breeden–Litzenberger, q(K) = e^(rT) d²C/dK², computed from
  the SVI total variance (analytic in k, no finite differences on noisy quotes).
- `POST /api/simulation/compare`: local vol and Heston (or SLV) calibrated on the
  same ticker, with their spot smiles, forward smiles and up-and-out prices.

## 5. Front end (lots S3, S4)

- **S3**: a fan chart of the quantile bands with the display paths over it (the
  animation stays); a date slider with the histogram of S_t, the Black-Scholes
  lognormal at the model's own ATM vol at t, and the market density when there
  is one; the instantaneous vol panel on the same time axis; the model smile
  against the market smile with its error bars; the martingale check as a tile.
- **S4**: a comparison tab, local vol against stochastic vol on one ticker: the
  same smile today, different forward smiles and different barrier prices.

## 6. Lots

| Lot | Content | Acceptance |
|---|---|---|
| S1 | Engine, `Sample::inst_vol` in the five models, bindings, property tests | Tests green; the pricing paths unchanged (the flag is off) |
| S2 | API: distribution, market smile and density, comparison | pytest with the real wheel; `openapi.json` regenerated |
| S3 | Page: fan chart, date slider and histogram, vol panel, smile against market, martingale tile | vitest, typecheck, lint; checked on the live front |
| S4 | Comparison tab | Same |

Later, out of scope: rough Bergomi on the page (its calibration exists).
