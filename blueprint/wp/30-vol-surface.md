# WP 30 — An implied-vol surface free of static arbitrage (eSSVI)

| | |
|---|---|
| **Status** | *Done*. E1 (the eSSVI surface, its Dupire grid, the binding's `surface_model="essvi"`), E3 (market vega through the eSSVI calibration), E4 (the surface and its local vol on spaced pillars, §5.1) and E2 (`QM_VOL_SURFACE`, production on eSSVI) done on 2026-11-26, after WP 31 cleaned the quotes. Issue #381 |
| **Depends on** | WP 08 (stored option chains, quote cleaning), WP 16 (the local-vol and SLV script models that read the grid) |
| **Issue** | #381 |

## 1. Why

SPY's local-vol grid had holes: cells where Dupire's local variance came out
negative or infinite and were filled from a neighbour. The cause is the surface,
not Dupire. Each maturity is fitted on its own by raw SVI, and
`select_surface_slices` keeps a slice on its arbitrage checks, not on its fit:

| Slice | What the fit did |
|---|---|
| 0.849y | `sigma` at its floor 0.02, `m` = 102.5 %: the curvature sits on one strike |
| 1.562y, 2.06y | `b` = 5 and `rho` = −0.989, both at their bounds |

Repriced against the cleaned quotes it was calibrated on, the production surface
misses by up to **+21.5 % of the call price** (1.56y, 105 % moneyness), +11.7 % at 1.06y and
+13 % at 2.06y. Patching the slice selection would hide this. The fix is a surface
that cannot produce such a slice: one free of static arbitrage by construction,
whose parameters cannot run off to their bounds.

## 2. The surface

**One slice: eSSVI** (Gatheral & Jacquier 2014, eq. 4.1, with `rho` per maturity
as in Hendriks & Martini 2019):

    w(k) = 1/2 (theta + rho psi k + sqrt(psi^2 k^2 + 2 rho psi theta k + theta^2))

Here `theta` is the at-the-money total variance, `psi = theta phi(theta)` the
at-the-money skew and `k = ln(K/F)`. It is an SVI slice with `b = psi/2`,
`m = -rho theta/psi`, `sigma = theta sqrt(1-rho^2)/psi` and
`a = theta (1-rho^2)/2`. The API and the front end keep reporting raw SVI.

**No butterfly arbitrage** (Gatheral & Jacquier, Theorem 4.2):
`psi (1+|rho|) < 4` and `psi^2 (1+|rho|) <= 4 theta`.

**No calendar arbitrage** between slices `i < j` (Hendriks & Martini 2019, in
the corrected statement of Pasquazzi 2023, Proposition 4.14). With
`theta_j > theta_i`, the slices do not cross if and only if two things hold:

- the necessary condition `|rho_j psi_j - rho_i psi_i| <= psi_j - psi_i`, that is
  `psi_j >= max(psi_i (1-rho_i)/(1-rho_j), psi_i (1+rho_i)/(1+rho_j))`;
- and either `phi_j <= phi_i` with `phi = psi/theta`, or a quadratic condition.

Corbetta, Cohort, Laachir & Martini (2019, §2.2) state the necessary condition
alone as sufficient, and it is not. `testESSVI` has two slices that meet it and
the butterfly bounds, yet cross by 93 % of `theta` at `k = -0.58`. The first
version of this lot followed Corbetta et al. and had the same gap.

The calibration keeps the `phi`-non-increasing branch, `psi_j <= psi_i theta_j/theta_i`,
for three reasons:

- Under this branch, the interpolation below provably stays arbitrage-free:
  `psi/theta` of two linear functions of `T` is monotone between its ends.
- It never empties the feasible set: `rho_j = rho_i` with `psi_j = psi_i` is
  always in it.
- An equity skew's `phi` decays in `T` like a power anyway.

A random sweep in the test checks the slice pairs it admits on `w` itself, out
to `|k| = 10`.

**Calibration, slice by slice in maturity order** (Corbetta et al.'s robust
search). `rho` is sampled on a grid, then refined by golden section. For each
`rho`, `psi` is found by Brent inside its feasible interval: the butterfly bounds
above it, the calendar bound from the slice before below it. The objective is
weighted least squares in implied vol. There is no starting point to guess. A
slice whose feasible set is empty is left out and reported.

**The at-the-money term structure first.** This departs from Corbetta et al.,
who anchor `theta` on one quote. Two SPY expiries a fortnight apart (0.814y and
0.849y) quote at-the-money variances that contradict each other. Fitting them
one after the other is arbitrage-free, but it forces a forward variance near
zero between them: a 1 % local vol, a hole by another name. So:

1. Each maturity's `theta` is estimated first, by a local weighted quadratic on
   the seven quotes nearest the money, together with its standard error.
2. `theta` is then the integral of a piecewise-constant forward variance
   `f_j >= v_min` (`min_forward_vol` = 5 %) that minimises
   `sum_i (theta_i - theta_hat_i)^2 / se_i^2 + lambda sum_j (f_j - f_{j-1})^2 / d̄_j`.
   The weight `lambda` is scaled to the data's units, with
   `forward_smoothing` = 0.1. This is a convex quadratic under lower bounds,
   solved exactly by coordinate descent (`monotone_theta`).
3. The slices are fitted with `theta` fixed on that curve.

A maturity that contradicts its neighbours is outvoted by them. An isotonic
regression would instead pool them on a flat stretch at the minimum forward
variance, which is the hole again.

**Between and beyond slices** (Corbetta et al., §5): `theta`, `psi` and
`rho psi` are interpolated linearly in `T`, which keeps both conditions. Before
the first slice, `theta` and `psi` scale with `T/T_1`. Past the last slice,
`theta` grows at the last segment's slope while `psi` and `rho` are held.

**Dupire.** `sigma_loc^2 = w_T / g(k)`, with `w_T`, `w_k` and `w_kk` all
analytic. It is positive wherever the conditions hold, so the grid needs no
fallback cell.

Code: `market/essvi.hpp`, `src/market/essvi.cpp`; the Dupire grid is a template
shared with `SVISurface` (`dupire_from_svi.cpp`);
`calibrate_vol_surface(..., SurfaceModel::ESSVI)`; and the binding argument
`surface_model="essvi"`.

## 3. Measured on SPY

The stored chain has 20 slices. The local-vol grid of each surface is used to
reprice calls on 15 points (0.35y to 2.06y, strikes at 95 %, 100 % and 105 % of
spot), against the Black price of the cleaned quotes each surface was fitted
on. The error is relative, in % of the call price.

| | Production SVI (7 slices kept) | eSSVI, necessary calendar condition only (first draft) | eSSVI, `phi` non-increasing (this lot) |
|---|---|---|---|
| Slice fit, mean rmse | 2.26 vol points | 2.08 | 2.06 |
| Worst repricing error, ≤ 1.06y | +11.7 % (1.06y, 105 %) | −3.5 % (0.60y, 105 %) | −3.9 % (0.60y, 105 %) |
| Worst repricing error, 1.56–2.06y | +21.5 % (1.56y, 105 %) | −4.3 % | −3.6 % |
| Other points | | within ±1 % up to 1.06y | within ±1 % up to 1.06y |
| Local vol, smallest (92–108 %, T > 0.1) | 0.061 | 0.068 | 0.078 |
| Local-vol roughness, max \|S² ∂²σ/∂S²\| | 263 | 65 | 57 |
| Grid cells filled from a neighbour | yes | none | none |

On SPY, the `phi` bound is active on 5 of the 20 slices (0.26y, 0.81y, 0.85y,
1.56y, 2.06y), and it costs nothing in fit. The errors left are 3–4 % of a call
price, on the out-of-the-money 0.6y call and at the long end, where quotes are
widest. They barely move with `forward_smoothing` between 0.1 and 1, so they
come from the slices' shape, not from the smoothing.

## 4. Market vega through the calibration (E3)

The vega by quoted option (`api/app/market_vega.py`) needs dV/d(each quoted
implied vol). The adjoint Monte Carlo gives dV/dσ_loc on the grid, and
`essvi_superbucket` (`src/market/essvi_superbucket.cpp`) takes it back through
the four maps of the calibration, last to first:

| Map | Differentiated by |
|---|---|
| Dupire: (θ, ρ, ψ) per slice → σ_loc | the analytic local variance on an AAD tape, with the grid's fills and clamps replayed |
| Each slice's fit: (its quotes, θ, the slice before) → (ρ, ψ) | the implicit function theorem on its KKT conditions, active set included: butterfly bounds, the ρ bound, and the calendar bounds, which move with the slice before. A slice's risk therefore also flows into the slices before it |
| The term structure: (θ̂, weight) → θ | the QP's KKT system on its active set, where some forward variances sit at the floor |
| The at-the-money estimates: quotes → (θ̂, standard error) | linear in w = iv²T; the residual spread by W-orthogonality |

**The implicit function theorem is exact.** It uses the fits' full Hessians
(analytic second derivatives of the implied vol in θ, ρ and ψ), and adds the
active bounds' curvature weighted by their Lagrange multipliers.

The SVI superbucket uses the Gauss-Newton Hessian, which drops the
residual-weighted terms. A slice held on a calendar bound away from its quotes
has large residuals, and there the Gauss-Newton version was 4–16 % off the
bump-and-recalibrate value. The exact version agrees with it to 10⁻⁶–1.5·10⁻⁴,
which is the bump's own noise, in both test scenarios:
- noise with contradicting slices;
- the φ bound active, forward variances at the floor, and a slice left out.

The SVI superbucket keeps its Gauss-Newton Hessian, which carries the same
first-order error on badly fitted slices.

Every quote of every slice that entered the calibration gets a row. A slice
left out of the surface still moved the term structure. The binding reruns the
calibration and refuses to differentiate it if it does not give back the
reported slices, for example when the settings differ.

**On SPY**, for a one-year at-the-money call (200,000 paths, 4.2 s either way):

| | SVI | eSSVI |
|---|---|---|
| Total vega per vol point | 2.730 ± 0.002 | 2.690 ± 0.003 |
| Bucketed on the slices bracketing 1y | 0.85y: 0.34, 1.06y: 2.53 | 0.81y: 0.45, 0.85y: 0.06, 1.06y: 1.50, 1.16y: 0.89 |

The eSSVI vega spreads to the next expiry, 1.16y. Its at-the-money quote moves
θ at 1.06y through the smoothed term structure. That is the price of the
smoothing: a quote outvoted by its neighbours also lends them its risk.

## 5. Lots

| Lot | Content | Status |
|---|---|---|
| E1 | eSSVI slice, conditions, calibration, term-structure smoothing, surface, Dupire grid; `surface_model` in the binding; `tests/testESSVI.cpp` (slice equals SVI with exact derivatives; noiseless recovery; contradicting quotes still give an arbitrage-free surface whose grid needs no fallback) | Done |
| E2 | `market_snapshot._local_vol_market` on eSSVI behind `QM_VOL_SURFACE=svi\|essvi` (code default `svi`, production `essvi`), the surface model in the calibration's cache key; the Vol tab, the pricers, the market vega and the Heston/SLV calibrations follow it | Done (§5.1): on clean data (WP 31 §5.10) after E4; median move against SVI 0.57 % over the library |
| E4 | The surface's pillars: every feasible slice fitted, only those spaced by 25 % in maturity (`essvi_surface_pillars`, as SVI's selection) build the surface, its Dupire grid and the superbucket's | Done (§5.1): SPY's eSSVI local vol reprices its surface within 0.8 % from 3 months to 2 years |
| E3 | Market vega (superbucket) through the eSSVI calibration, §4 below: `essvi_superbucket`, the binding's `surface_model`, `api/app/market_vega.py` reading the market's surface; `tests/testESSVISuperbucket.cpp`, `api/tests/test_market_vega.py` on both surfaces | Done |

### 5.1 E4 and E2, measured on SPY 2026-11-26 (clean data, WP 31)

**What E4 found.** On WP 31's clean quotes the eSSVI surface reprices the market (the one-year put at S0 to +0.1 %, the fair variance above half the forward exactly), but its local vol priced that put 3.5 % low and a three-month put 20 % out of the money 41 % low. The surface had forty pillars, SPX's expiries a few days apart at the short end:
- Dupire's dw/dT between two pillars a day apart is the difference of two fits' noise over a day: the one-year local vol at k = −0.2 jumped between 0.31 and 0.18 from one column to the next;
- `local_vol_maturities` needs two columns per segment; forty segments and fifty columns made it fall back to equal steps from 0.081 y, so the first month's put wing (local vol about 0.9) was read as the 0.081 y column's 0.31.

**The fix.** `essvi_surface_pillars`: every feasible slice is still fitted (each fit is conditioned on the one before it) and reported; only the shortest, then each one at least 25 % longer than the last kept, and the longest, build the surface. SVI's slice selection already used that spacing for the same reason. On SPY: 15 pillars, the grid aligned on them from 0.026 y.

**Local vol against its own surface** (200k paths, 252 steps a year; Black price on the surface, in %):

| T | k = −0.2 put | −0.1 put | 0.0 call | +0.1 call |
|---|---|---|---|---|
| 0.25, before E4 | −41.0 | −21.3 | −5.1 | −9.9 |
| 0.25, after | −0.0 | +0.6 | +0.8 | +3.9 |
| 1.0, before | −13.9 | −6.9 | −1.6 | +3.8 |
| 1.0, after | −0.1 | +0.2 | +0.2 | +0.6 |
| 2.0, after | −0.1 | +0.1 | +0.1 | +0.2 |

SVI's local vol on the same data: within 1.3 % on the puts and at the money, +3.9 % on the three-month call at k = +0.1 and +5.0 % on the one-year one. The short call wing past the quoted band (where the grid is flat and the surface extrapolates) is the remaining gap of both.

**Against the market** (WP 31 §5.10's references, one-year products):

| | fair variance | P(S_T < S0) | put at S0 |
|---|---|---|---|
| market | 0.054–0.062 | 0.337 | 45.89 |
| SVI: local vol / Heston / SLV | 0.058 / 0.058 / 0.060 | 0.339 / 0.328 / 0.340 | −1.3 / −3.4 / −2.2 % |
| eSSVI after E4 | 0.058 / 0.060 / 0.060 | 0.331 / 0.330 / 0.331 | −0.3 / −2.6 / −1.3 % |

**E2's rehearsal**: every tracked ticker calibrates in the same time as on SVI; the 39 single-underlying library products price under `auto` with no error; the median move against SVI is 0.57 %, the largest the SLV variance products (−8 %) and the SLV barriers (+5 %).

## 6. Sources

- J. Gatheral, A. Jacquier, *Arbitrage-free SVI volatility surfaces*,
  Quantitative Finance 14(1), 2014.
- S. Hendriks, C. Martini, *The extended SSVI volatility surface*, Journal of
  Computational Finance 22(5), 2019.
- J. Corbetta, P. Cohort, I. Laachir, C. Martini, *Robust calibration and
  arbitrage-free interpolation of SSVI slices*, Decisions in Economics and
  Finance 42, 2019 (arXiv:1804.04924).
- L. Pasquazzi, *eSSVI surface calibration*, 2023 (arXiv:2304.02106): the
  corrected calendar proposition, Appendix, Proposition 4.14.
- A. Mingone, *No arbitrage global parametrization for the eSSVI volatility
  surface*, 2022 (arXiv:2204.00312).

All but Hendriks & Martini are in the knowledge base.
