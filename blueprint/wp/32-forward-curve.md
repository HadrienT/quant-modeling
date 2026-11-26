# WP 32 — A forward curve in the core, read by every model

| | |
|---|---|
| **Status** | *In progress*: design only (2026-11-26); no lot started. Issue #389 |
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
