# WP 32 — A forward curve in the core, read by every model

| | |
|---|---|
| **Status** | *In progress*: F1 and F2 done (2026-11-27, §4): `ForwardCurve` in the core and the Python module; the CPU single-underlying models step on it through `ScriptModelSpec::forward_curve`, which nothing sets yet (F5). F3–F6 remain. Issue #389 |
| **Depends on** | WP 31 L6 (the market's forwards and discounts reach the API), WP 17 (AAD), WP 19 (GPU Monte Carlo), WP 27 (generated adjoints) |
| **Unlocks** | multi-date products on the market's carry; every model added after F1 |

## 1. Why

WP 31 L6 (decision D1) prices each product on a flat rate and yield fitted to the market's discount DF(T) and forward F(T) at its last date T. A vanilla expiring at T then reprices exactly under every model; a product with earlier dates (an autocall's observations, an Asian's fixings, a barrier) sees the carry flat over its life. The market is not flat:
- SPX's implied rate runs from 4.4 % at two months to 5.1 % at one to two years (WP 31 §4.7);
- SPY's December dividend (1.9 $) is a jump in its forward, which a flat yield spreads over the year;
- a single stock's discrete dividends are the same.

Every model today takes a scalar `rate` and `dividend`: the CPU simulation models through `ScriptModelSpec`, the GPU and JIT kernels (`dm.r`, `dm.q`), the AAD adjoints (one rho, one dividend risk) and the analytic engines. Only the Hull-White equity hybrid reads a discount curve. New models are planned; adding the curve to each one after the fact would be a retrofit per model. This WP defines the one object they all read, before they land.

## 2. Design

- **`ForwardCurve`** (`include/quantModeling/market/forward_curve.hpp`): the spot S0, a `DiscountCurve` (the existing class, flat-forward extrapolation), and ln(F(t)/S0) on pillars, linear in t between them (a piecewise-constant carry rate) and flat-carry beyond. It answers `forward(t)`, `discount(t)`, `carry(t1, t2) = ln F(t2)/F(t1)` and `rate(t1, t2) = ln DF(t1)/DF(t2)`. A flat curve is built from (r, q) and reproduces today's numbers bit for bit: that is F1's first test.
- **The models' contract.** `ScriptModelSpec` carries an optional `ForwardCurve`; absent, the flat one of (rate, dividend). A model's step uses `carry(t_i, t_i+1)` where it used (r − q) Δt, and the script engine discounts a payment at t by `discount(t)`. The local-vol grid maps k to K through F(t) (WP 31 makes k forward moneyness already).
- **Discrete dividends.** A later extension: a pillar pair around an ex-date makes the carry a jump; the curve's linear-in-t log-forward handles it with two pillars a second apart. No proportional-versus-cash choice is made here.
- **AAD.** Risks per pillar: rho per discount pillar, a carry risk per forward pillar; today's scalar rho and dividend risk are their sums, so the response keeps its fields.
- **GPU and JIT.** The host precomputes, on the simulation's time grid, one carry per step and one discount per payment date, as device arrays: the kernels read `carry[step]` instead of `(r - q) * dt`. One change, the same in every kernel; the JIT kernel reads the same arrays.

## 3. Lots

| Lot | Content | Accepted when |
|---|---|---|
| F1 | `ForwardCurve` in the core, its bindings | a flat curve reproduces today's prices bit for bit; property tests: E[S_t] = F(t) and zero coupons reprice DF(t) |
| F2 | CPU simulation models (Black-Scholes, local vol, Heston, SLV, rough Bergomi, jumps) step on the curve | the martingale and zero-coupon properties under each model; a vanilla at every pillar reprices on the surface |
| F3 | Script engine discounting on DF(t); AAD risks per pillar | the pillar risks match bumps; their sums are today's scalar risks |
| F4 | GPU and JIT kernels | CPU/GPU agree within the tolerances of WP 26 (1e-12 prices, 1e-10 risks) |
| F5 | The API hands the stored curve over (`options.chain_iv_expiries`); the flat-per-product path of WP 31 L6 goes | an autocall's observation dates read the market's forwards; legacy unchanged |
| F6 | Analytic engines | closed forms on the curve match the Monte Carlo |

**Rule from F1 on:** a new model takes the curve from day one.

## 4. As built

### F1 (2026-11-27)

- `include/quantModeling/market/forward_curve.hpp`, `src/market/forward_curve.cpp`; `qm.ForwardCurve` in the Python module (`bind_market.cpp`), built from (spot, times, forwards, discount factors) or `ForwardCurve.flat(spot, rate, dividend)`.
- **The flat curve keeps its scalars.** `flat(S0, r, q)` stores (r, q) and evaluates the models' own expressions: `forward(t) = S0 e^{(r − q) t}`, `discount(t) = e^{−r t}`, `carry(t1, t2) = (r − q)(t2 − t1)`. Bit for bit with those expressions is tested. It does not make a model's price bit for bit on its own: several models fold the drift into one constant, `(r − q − σ²/2) Δt`, which `carry − σ²Δt/2` does not reproduce to the last bit. So F2 keeps each model's scalar path when `is_flat()`, and reads the curve otherwise; `flat_rate()` and `flat_dividend()` hand the scalars back.
- `dividend(t1, t2) = rate − carry`, the integrated dividend yield (repo included): the leg a model with a separate rate and yield (a numeraire, a quanto drift) needs.
- `log_forward_weights(t)` gives ln F(t) as a combination of the pillars' ln F, as `DiscountCurve::log_discount_weights` does for ln DF: the risks per forward pillar of F3 are built on it.
- `tests/testForwardCurve.cpp`: the flat curve's expressions bit for bit; pillars repriced; carry and rate telescoping over any grid, extrapolation included; zero coupons reprice DF(t) and F·DF = S0 e^{−∫q}; E[S_t] = F(t) under exact lognormal steps on a grid that ignores the pillars (200,000 paths, 4 standard errors); a cash dividend as two pillars a second apart, exact away from the gap; the weights against pillar bumps.

### F2 (2026-11-27)

- **The contract.** `ISimulationModel::set_forward_curve(curve)`, before `init()`; `ScriptModelSpec::forward_curve` (optional) is read by `make_script_model`. Black-Scholes, local vol, Heston (and Bates), SLV and rough Bergomi take it. The several-underlying models, the Hull-White hybrid and the short-rate models refuse a curve with an explicit error: a multi-asset curve needs one forward per asset over one discount curve, left for when a multi-asset product needs it.
- **A flat curve is its scalars.** The factory turns `ForwardCurve::flat(S0, r, q)` back into (r, q) and today's path: every model's paths are bit for bit those of the scalars (tested), and the device route is unchanged.
- **Rate and dividend become parallel shifts.** On a curve the model's `rate` and `div` parameters are zero and shift the curve: step k's rate argument is `c_k + r` with `c_k = ln F(t_k)/F(t_{k−1}) / Δt_k`, the numeraire `e^{r t}/DF(t)`, a discount `DF(m)/DF(t) e^{−r(m−t)}`, a forward `S F(m)/F(t) e^{(r−q)(m−t)}`. So the AAD rho and dividend risk are those of a parallel shift of the curve (tested against `ForwardCurve::shifted`), the sums the per-pillar risks of F3 will split. The bumped rho of `script_route` bumps the curve the same way (`shifted(±h, 0)`).
- **No curve lookup on a path.** `CurveCarry` (`models/equity/curve_carry.hpp`) tabulates in `init()` the carry rate of every step and, per event, 1/DF, the discount ratios and the forward growths a payoff reads. The step functors shared with the IR (`path_steps.hpp`) are unchanged: they receive `c_k + r` where they received `r`.
- **CPU only until F4.** `describe_device` refuses a model on a curve, so the GPU, the compiled IR and the JIT decline it and the engine stays on the CPU (an explicit GPU request says why).
- **Set aside here.** A route's `control_mean` is a closed form on the scalars: it is not used on a curve (F6). Black-Scholes's Brownian-bridge survival over a step that contains a pillar assumes the step's average carry, an approximation of second order (the carry's change across a pillar is under 1 %/yr). Theta by bumped dates keeps the curve anchored to today.
- `tests/testForwardCurveModels.cpp`, under every model: a flat curve bit for bit; numeraire, discounts and forwards are the curve's (zero coupons reprice DF(t)); E[S(t)/N(t)] = F(t) DF(t) at dates around the pillars and the dividend, which drops the deflated spot by its amount; where the drift does not feed the vol (Black-Scholes, Heston, rough Bergomi) a vanilla on the curve equals, path by path, the vanilla under the flat (r, q) fitted at its maturity, to 1e-11; Black-Scholes against Black's formula and Heston against its COS price on F(T), DF(T); the AAD rate and dividend against the shifted curve; a script through `simulate_script`.
