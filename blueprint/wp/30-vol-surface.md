# WP 30 — An implied-vol surface free of static arbitrage (eSSVI)

| | |
|---|---|
| **Status** | *In progress*. E1 (the eSSVI surface in the C++ core, its Dupire grid, the binding behind `surface_model="essvi"`) done; E2 (the API's local-vol market on eSSVI) and E3 (market vega through the calibration) to do, issue #381 |
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
misses by up to **+21.5 vol %** (1.56y, 105 % moneyness), +11.7 % at 1.06y and
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

**No calendar arbitrage** between slices `i < j` (Hendriks & Martini; Corbetta,
Cohort, Laachir & Martini 2019, §2.2 and §5.1): `theta_j >= theta_i` and
`|rho_j psi_j - rho_i psi_i| <= psi_j - psi_i`, that is
`psi_j >= max(psi_i (1-rho_i)/(1-rho_j), psi_i (1+rho_i)/(1+rho_j))`.

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

The stored chain has 20 slices. The surface is repriced against the cleaned
quotes it was fitted on.

| | Production SVI | eSSVI, smoothed `theta` |
|---|---|---|
| Worst error, ≤ 1.06y | +11.7 vol % | within ±1 % |
| Worst error, 1.56–2.06y | +21.5 % | −2.4 to −4.3 % (about the bid-ask) |
| Local-vol roughness | 263 | 63–67 |
| Grid cells filled from a neighbour | yes | none |

The long-end error barely moves with `forward_smoothing` between 0.03 and 1. It
comes from the slices' shape, not from the smoothing.

## 4. Lots

| Lot | Content | Status |
|---|---|---|
| E1 | eSSVI slice, conditions, calibration, term-structure smoothing, surface, Dupire grid; `surface_model` in the binding; `tests/testESSVI.cpp` (slice equals SVI with exact derivatives; noiseless recovery; contradicting quotes still give an arbitrage-free surface whose grid needs no fallback) | Done |
| E2 | `market_snapshot._local_vol_market` on eSSVI. Local-vol and SLV prices move, so measure the library products before and after, and the maintainer decides. The raw-SVI path stays available | To do |
| E3 | Market vega (superbucket) through the eSSVI calibration: implicit function theorem on the active constraints | To do |

## 5. Sources

- J. Gatheral, A. Jacquier, *Arbitrage-free SVI volatility surfaces*,
  Quantitative Finance 14(1), 2014.
- S. Hendriks, C. Martini, *The extended SSVI volatility surface*, Journal of
  Computational Finance 22(5), 2019.
- J. Corbetta, P. Cohort, I. Laachir, C. Martini, *Robust calibration and
  arbitrage-free interpolation of SSVI slices*, Decisions in Economics and
  Finance 42, 2019.
