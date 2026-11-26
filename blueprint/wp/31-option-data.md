# WP 31 — Clean option data at ingestion: our own implied vols, OTM quotes, market forwards

| | |
|---|---|
| **Status** | *In progress*: decisions D1, D2, D3, D5 taken on 2026-11-26 (§10). The `data-ingest` lots L1–L4 and L8 are done (data-ingest #32, #33, issue data-ingest #31); what they measured, and where they depart from this design, is in §4.7. L0, L5–L7 to do. Issue #386 |
| **Depends on** | WP 08 (stored option chains, `RawVolSurface` cleaning), WP 30 (the eSSVI surface that consumes the quotes) |
| **Unlocks** | WP 30 E2 (the API on eSSVI, #381): its measured price moves mean nothing until the quotes are right |
| **Repositories** | `~/data-ingest` (ingestion, the new derived table) and this one (the consumer, the reduced cleaning) |
| **Issues** | #386 (finding and fix); #384 (the eSSVI resume issue, step 2) |

## 1. Why

While checking the large E2 price moves (#384) against model-free references, the cause turned out to sit upstream of the surface choice: the quotes.

### 1.1 The implied vols are Yahoo's, at r = q = 0

`data-ingest` stores yfinance's `impliedVolatility` as it comes (`src/data_ingest/sources/options_chain.py`, column `implied_volatility`). `RawVolSurface` (`src/market/raw_vol_surface.cpp`) reads it as the quote's vol and places it at a log-moneyness computed with **our** forward, `S exp((r - q) T)`. Yahoo backs its IVs out at **r = q = 0**: on SPY 2027-12-17 (T = 1.06), the 2026-11-25 mids give Yahoo's put IVs to 0.0004 at r = q = 0.

| K | Call mid | Put mid | Yahoo IV call / put | Black IV at r = q = 0 | Black IV at r = 4 %, q = 0.79 % |
|---|---|---|---|---|---|
| 700 | 86.31 | 44.85 | 0.272 / 0.182 | 0.268 / 0.182 | 0.228 / 0.222 |
| 720 | 73.78 | 51.85 | 0.263 / 0.171 | 0.256 / 0.170 | 0.219 / 0.215 |
| 740 | 61.90 | 59.95 | 0.252 / 0.159 | 0.245 / 0.159 | 0.210 / 0.208 |

A wrong forward pushes a call's IV up and a put's down. The cleaning keeps both sides at every strike, so a long-dated slice carries two smiles about 9 vol points apart, both heavily weighted. The consequences:
- SVI fits the mixture and degenerates (2027-12-17: b = 1.82, rho = −0.976, m = −0.76). These are the "degenerate slices" and the "+21.5 % misses" of WP 30 §1.
- eSSVI, tied across slices, lands lower.

At one year, the market's ATM vol from the mids at our forward is about 21.5–22 %. The SVI surface reads 19.3 %, the eSSVI surface 17.1 %. The error grows with T (r − q).

> **Corrected by L2–L3 (§4.7).** The 21.5–22 % is read from SPY's *American* mids at an assumed forward. The European-equivalent ATM vol at one year, computed in `options.chain_iv`, is **19.6 % on SPX** and **20.0–20.2 % on SPY de-Americanized**. The SVI surface's 19.3 % is therefore close to the market; the eSSVI's 17.1 % is still 2.5 points low. The "market" column of §1.2 is to be recomputed on `chain_iv` in L7.

### 1.2 What the model-free references say

The references, taken on SPY 2026-11-25 at T = 1, r = 4 % (the Pricing page's default; SOFR 3.94 %):
- **variance swap**: the log contract on the OTM mids (Carr & Madan 1998; Demeterfi, Derman, Kamal & Zou 1999; the VIX strip);
- **digital put**: the put mids' slope at S0, less the early-exercise premium's slope.

| | Market | SVI surface | eSSVI surface |
|---|---|---|---|
| ATM vol | ~21.5–22 % on American mids; **19.6 % (SPX) / 20.0–20.2 % (SPY de-Americanized)** in `chain_iv`, §4.7 | 19.3 % | 17.1 % |
| Fair variance (vol) | 0.0689 (26.2 %) full strip; 0.0634 (25.2 %) from 30 % strikes; 0.0567 (23.8 %) from 50 % | 0.051 within the Dupire band k ∈ [−0.6, 0.6], exploding beyond | 0.0455 (21.3 %) |
| P(S_T < S0) | 0.350 | 0.364 | 0.316 |

Every variance swap of the E2 measurement is below the market's 0.063–0.069:
- local vol: 0.0457 on SVI, 0.0444 on eSSVI;
- Heston: 0.059 on SVI, 0.049 on eSSVI.

The −15 % to −52 % moves between the surfaces compare two wrong numbers. The `volatility-swap` SLV price of −2242 on eSSVI against 97.7 on SVI is no bug: at 100 × 1,000 per unit of vol, it is 17.7 % against 20.1 % of expected realised vol, the ATM gap above.

### 1.3 Other facts found on the way

- **SPY's options are American.** Regressing parity in K gave a discount factor of about 1 on every expiry, because of the ITM puts' early-exercise premium. Taking the rate from the curve and the forward from parity near the money gives a carry of 3.1–3.4 %/yr, so q is about 0.6–0.9 %, against 0.79 % stored.
- **SPY's end-of-quarter expiries** (2027-03-31, 06-30, 09-30) have inconsistent quotes. Their parity forward spreads by 3.5–5 $ across strikes, against 0.4 $ on the monthlies. Their strikes stop at 0.72 S. *(L3, §4.7: the spread is a non-robust one, driven by a share of aberrant quotes, mostly ITM calls; robustly, and on de-Americanized prices near the money, these expiries are as consistent as the monthlies, and they are kept.)*
- **The E2 measurement priced at r = 3 %**, the Pricing page at 4 %.
- **yfinance offers European options on the S&P 500.** `^SPX` (SPX and SPXW, 51 expiries to 2031-12) and `^XSP` (mini-SPX, a tenth of SPX, 41 expiries to 2028-12, ATM spreads of 0.07 $). yfinance returns only part of SPX's strikes (56 calls on a one-month expiry), and their open interest comes back as 0 outside the session.

## 2. Principles (decided with the maintainer, 2026-11-26)

1. **Implied vols are computed at ingestion, not at pricing.** quant-modeling works on clean data. Missing or wrong data is fixed in `data-ingest`, the project's rule. This reverses the "Stage 1 only" choice written in `options_chain.py`'s docstring, which must say so.
2. **The raw quotes stay untouched.** `options.chain_snapshot` keeps what yfinance returned. The derived quantities go to a new table, so that any method change is a recomputation, never a loss.
3. **An implied vol is a quoting convention, not a model.** It is the σ that Black's formula on the forward, `C = DF [F N(d1) − K N(d2)]`, maps to the observed price. It needs F and DF, both model-free on a European option, and no assumption on the dynamics. The models (local vol, Heston, SLV) consume implied vols; they never produce them. So there is no circularity.
4. **The only circularity is the American premium**, and it is broken as OptionMetrics breaks it (§4.4):
   - OTM quotes only, where the premium is small;
   - each quote de-Americanized on a tree at its own constant vol;
   - optionally, one fixed-point pass with the calibrated local vol.
5. **One side per strike: the OTM option.** Puts below the forward, calls at or above it. Call and put at one strike carry the same information through parity. The ITM side only adds a noisier copy with a correlated error:
   - a wider spread (SPY K = 700, 1.06y: 2.85 $ on the ITM call, about ±0.5 vol point; 0.28 $ on the OTM put, about ±0.05);
   - the early-exercise premium;
   - a forward error pushing it the other way.
6. **The price is the bid/ask mid.** The quotes are live and synchronous; the last trade may be hours old. The spread weights the quote.
7. **No dbt.** The computation is root-finding and a tree per quote, not SQL, and dbt on Postgres runs SQL models only (its Python models exist on Snowflake, Databricks and BigQuery). The existing `Source` / `TableSpec` framework and an Airflow dependency are enough. dbt would be worth its cost if many SQL-only marts with quality tests appeared, which is not the case.

## 3. What the source gives, and when

- **A full chain per run.** Every run fetches the whole chain live at that moment: all expiries, all strikes (9,921 SPY quotes on 2026-11-25). It is not a diff: one day gives a complete chain.
- **No history.** yfinance serves no past chain, so history starts with the first run. That is why the DAG runs every business day.
- **Partial chains.** An expiry that fails (rate limit) leaves a partial chain, stored as such; the load registry records the row count.
- **Truncated strikes on SPX**: a coverage limit, to be measured (§9, L1).
- **Open interest** is counted by the OCC once a night and published the next morning. All day, Yahoo shows the previous day's figure. Positions closed in the closing rush lower *tomorrow's* OI; they change neither today's figure nor today's prices. The snapshot's time does not affect it. OI is a weak liquidity signal (zero on SPX/XSP outside the session); the spread and a positive bid measure the quote's own quality.
- **The snapshot's time.**
  - The cron is `30 22 * * 1-5` in UTC: 17:30 New York in winter, 18:30 in summer, after the close. So it captures closing quotes, the usual reference (OptionMetrics uses the closing best bid and offer).
  - The last minutes are the most liquid, not the worst.
  - What matters is that option and spot are observed together. SPY's options trade until 16:15, its close is set at 16:00, and the stored spot is that close. A parity forward on European options does not depend on the spot, so it absorbs the gap. On American options, the gap stays: about 15 minutes of index move. §4.3 measures it through the parity check.
- **Store `lastTradeDate`.** yfinance returns it and `data-ingest` does not keep it. It is the only way to see a quote whose bid/ask has not refreshed.

## 4. Design — `data-ingest`

### 4.1 Universe

- Add `^SPX` and `^XSP` to `OPTIONS_CHAIN_TICKERS`.
- For SPX, keep the contract's root, `SPX` (AM-settled monthlies) or `SPXW` (PM-settled), read from `contractSymbol`. Both can share an expiry date with different settlement times; their time to expiry differs by a session.
- New stored columns: `contract_symbol` and `last_trade_date`.
- Underlying level:
  - for `^SPX` and `^XSP`, the index close (a source to add if `prices` has no index), and nothing more, since the parity forward does not need it;
  - for SPY and stocks, the existing `prices.sp500_daily` close.

### 4.2 The derived table

A new source, `options-chain-iv`, writes `options.chain_iv`, one row per kept quote and per day:

| Column | Content |
|---|---|
| `date`, `ticker`, `expiry`, `root`, `option_type`, `strike` | the quote's key, as in `chain_snapshot` |
| `ttm` | years to expiry, ACT/365F, to the settlement time (AM or PM) |
| `mid`, `bid`, `ask`, `rel_spread` | price used, and its spread over the mid |
| `exercise` | `european` or `american` |
| `forward`, `discount` | the expiry's F and DF |
| `forward_source` | `parity` (European) or `carry` (rate curve and dividends, American) |
| `price_european` | the mid, or its de-Americanized equivalent |
| `early_exercise_premium` | mid − `price_european` (0 on a European) |
| `implied_vol` | Black-76 on `price_european`, F, DF: **ours** |
| `log_moneyness` | ln(K / F) |
| `vega` | Black-76 vega, for the fit's weights |
| `flags` | bit set: `no_bid`, `wide_spread`, `stale`, `below_two_ticks`, `parity_outlier`, `expiry_rejected`, `iv_failed`, `deep_itm_dropped` |
| `method_version` | the algorithm's version, so a recomputation is traceable |

A load registry, `options.chain_iv_loads`, mirrors `chain_snapshot_loads`: it holds row count, checksum, the method version and the inputs' dates (rate, dividend, spot). The expiry-level results (F, DF, implied r and q, the parity residual, and why an expiry was rejected) go to `options.chain_iv_expiries`.

### 4.3 Forward and discount, per expiry

- **European (SPX, XSP).**
  - Robust regression of `C − P = DF·F − DF·K` over the strikes where both sides have a two-sided quote, within a band around the money (start: |ln K/S| ≤ 0.1). Weight by 1 / (spread_C² + spread_P²), with Huber or Theil-Sen against outliers (Cohort, Corbetta, Martini & Laachir 2018, §4.1: "robust linear regression leveraging the put-call parity for mid prices").
  - Check DF against the rate curve. An implied rate more than a set number of basis points off flags the expiry; it does not reject it. SPX's implied rate (its box-spread financing rate) can legitimately differ from Treasuries, by an amount L0 measures.
  - The residuals' dispersion is the expiry's quality.
  - Check parity within the bid/ask bounds, `bid_C − ask_P ≤ DF (F − K) ≤ ask_C − bid_P` (Madan et al. 2019).
- **American (SPY, stocks).**
  - DF from the same day's SPX/XSP parity, interpolated in T to the expiry, with the SOFR curve as fallback (decision D3). *As built: the fallback is FRED's SOFR then Treasury CMT yields (§4.7).*
  - F from carry: an index ETF takes a continuous yield, `prices.dividend_yields`. A stock takes its discrete dividends: the next ones projected from `equity-dividends` (last amount, same frequency), so that the tree exercises at the right dates. *As built: every ticker with a dividend history takes discrete dividends, the ETFs included; the continuous yield is only the fallback (§4.7).*
  - Parity becomes a bound on American options, `S − K ≤ C − P ≤ S − K·DF` without dividends. The pairs nearest the money must fall within it, widened by the dividends; an expiry that misses is flagged.
  - The parity forward near the money is kept as a diagnostic, and its gap from the carry forward measures the spot asynchrony (§3).
- **Expiry rejection**: a parity dispersion beyond a threshold, or too few two-sided strikes. *(The end-of-quarter example does not hold once the dispersion is robust: §4.7.)*

### 4.4 The implied vol

1. **Choose the side.** The OTM option: put if K < F, call otherwise. Within a small band around F (start: |ln K/F| < 0.02), keep both, for the parity diagnostic only, and mark the one not used.
2. **De-Americanize** (American only), as OptionMetrics computes its IVs on a Cox-Ross-Rubinstein tree:
   - find σ_A such that the American tree price at σ_A equals the mid;
   - price the European option at σ_A on the same tree (or Black at the same F, DF): that is `price_european`.
   - Tree: Leisen-Reimer, smoother in σ than CRR, or CRR with Richardson extrapolation, at the dates of the projected dividends. Burkovska, Glau, Mahlstedt & Wohlmuth (2018) measure the method's error.
   - Measured on SPY 1y at the money: the premium's slope in K is about 0.03, which moves a digital put's probability by 0.03.
3. **Invert Black-76** on `price_european`, F, DF with Jäckel's "Let's Be Rational" (2015): machine precision in two iterations, no failure in the wings. `py_lets_be_rational` exists; otherwise our own C++ via the bindings.
4. **Flag**, never silently drop:
   - a price below its bound (intrinsic) or above F·DF;
   - a price under two ticks (Cohort et al. 2018: "prices cannot be smaller than 1 tick, and rounding effects would produce important distortions even for 2 ticks prices");
   - a relative spread above the threshold;
   - a stale `last_trade_date` with an unchanged bid/ask.

### 4.5 Scheduling and backfill

- A DAG task after `options-chain-snapshot`, `dividend-yields`, the prices and FRED of the same day. It recomputes a day from the stored rows, idempotently.
- **Backfill**: every stored day is recomputed from `chain_snapshot`, since the raw rows were kept. History is thus not lost.
- A new `method_version` triggers a recomputation of the history, as a deliberate command.

### 4.6 Tests (`data-ingest`)

- **Round trip**: Black price → our IV → price, to 1e-12, over |k| ≤ 3 and T from 1 day to 5 years.
- **European**: a synthetic chain generated at known (F, DF) plus bid/ask noise. The regression recovers F and DF within their error, and the outliers come out flagged.
- **American**: synthetic American prices from a tree at a known σ and known dividends. The de-Americanization recovers σ, and the European price matches Black.
- **Regression on real data** (SPY 2026-11-25, stored):
  - the call and put IVs agree within the band around F, to half a spread in vol;
  - the 1y ATM is within 21–23 %;
  - the end-of-quarter expiries are rejected;
  - the monthlies are kept.

  *As built (§4.7): the median call/put gap within |ln K/F| < 0.02 under 0.5 vol point; the 1y ATM within 19–21.5 %; every expiry beyond 18 days kept, the end-of-quarter ones included; the carry forward within 40 bp a year of the de-Americanized parity forward.*

### 4.7 As built in `data-ingest`, and what it measured (L1–L4, 2026-11-26)

The implementation is `options-chain-iv` (`src/data_ingest/sources/options_chain_iv.py`, analytics in `src/data_ingest/options/`), documented in `data-ingest`'s `docs/sources.md`. It follows §4 except where the data said otherwise:

1. **Universe and coverage (L1).**
   - `^SPX` and `^XSP` are captured; every quote keeps `contract_symbol`, `last_trade_date` and its `root`, which joins the chain's primary key.
   - yfinance's coverage, on the 2026-11-25 closes (`docs/option-coverage.md`):
     - SPX: 22,268 quotes, 21,047 of them two-sided, to 2031-12; the monthlies every 5 points, from K/S 0.03 to about 1.6–1.9;
     - XSP: 9,242 quotes, to 2028-12.
   - The "56 calls" truncation does not recur on the monthlies.
   - ATM relative spreads: SPX 0.6–1.1 %, SPXW 0.3–0.7 %, XSP 1.5–3 %.
   - On a third Friday yfinance returns one contract per strike, SPX at some and SPXW at others, so parity is fitted per root.
   - An unknown root, `SPXO`, is stored and left out.
   - The comparison with Cboe's listing is dropped (decided with the maintainer, 2026-11-26).
2. **One discount curve per day.**
   - Every European expiry takes DF from a curve built on the free parity fits whose slope is reliable: at least 14 days, a rate standard error under 25 bp, a dispersion under half the parity band.
   - F alone comes from the expiry's own parity.
   - Fitted alone, the slope is unusable on short expiries: −70 % of implied rate on a two-day SPXW.
   - The SPX implied rate runs from 4.4 % (two months) to 5.1 % (one to two years).
3. **D3's fallback, settled.**
   - The fallback is FRED: SOFR at the overnight point, then the Treasury CMT yields `DGS1MO`–`DGS30`, read directly without a bootstrap.
   - It applies to every day stored before SPX was captured, flagged `rate_fallback`. The maintainer accepted this for the history.
4. **ETFs take discrete dividends.**
   - yfinance's trailing yield for SPY is 0.79 %, against 1.06 % from the stored payments (7.58 $ over twelve months).
   - A continuous yield also spreads the December ex-date (1.9 $) over the year.
   - With D3's DF, discrete dividends projected from `prices.dividends` took the median call/put IV gap near the forward from 0.82 to 0.27 vol point, and under 0.1 point on every expiry to 2027-02.
5. **De-Americanization as a fixed point.**
   - σ solves Black(σ) = mid − premium_tree(σ), where the premium is American − European on one 201-step Leisen-Reimer tree.
   - The tree's European bias cancels in the difference.
   - σ is recovered to 1e-5 on synthetic tree prices.
6. **The American diagnostics use de-Americanized prices.**
   - On the raw American mids, the parity forward K + (C − P)/DF carries the puts' early-exercise premium: −9 $ at one year on SPY.
   - On de-Americanized prices, the carry forward and the parity forward agree within 40 bp a year of carry.
7. **No end-of-quarter rejection.**
   - SPY's end-of-quarter expiries hold aberrant quotes (a call at 131 $ at K = 651 between neighbours at 84 $).
   - They are mostly ITM, which the OTM selection leaves out. The pairs that break the parity bounds are flagged `parity_outlier`.
   - Robustly, these expiries are as consistent as the monthlies. A non-robust dispersion would reject monthlies too.
8. **The ATM vol.** At one year: 19.6 % on SPX, 20.0–20.2 % on SPY de-Americanized; SPX and SPY agree within about half a point, the cross-check of D2. See the note under §1.1.
9. **Not built.** `deep_itm_dropped`: the ITM side outside the both-sides band is not stored at all, and the raw chain keeps it. Two flags were added: `not_otm`, the side kept for the diagnostic only, and `short_maturity`, under 7 days. `rate_fallback` is also a flag.
10. **Times.** `ttm` runs from the close of the NYSE session of the quotes to 09:30 New York (SPX root) or 16:00 (every other root). SPX and XSP also trade overnight (Cboe GTH, from 20:15 New York); the 22:30 UTC snapshot falls before it.

## 5. Design — quant-modeling

1. **Read `options.chain_iv`** (`api/app/db.py`, `market_snapshot._chain`). `RawOptionQuote` takes `implied_vol` and `log_moneyness` from it. The forward of each expiry comes from the table, not from `S exp((r − q) T)`. `RawVolSurface::forward` becomes the expiry's stored forward.
2. **The cleaning keeps only what is not data quality**: the arbitrage checks and the band the fit trusts. §6 says, filter by filter.
3. **One forward for the surface and the pricer (decision D1: the market's).**
   - The issue: the surface's forwards are now the market's. If a pricer drifts at the request's r and the stored q, its forward differs from the surface's, and a vanilla no longer reprices.
   - The proposal: `market_snapshot` exposes the term structure of F and DF it read, and the pricers' drift takes the implied carry per maturity.
   - The request's `rate` would then matter for discounting only, or follow DF too.
4. **The XSP/SPX surface for S&P 500 products (decision D2).** The surface in ln(K/F) transfers between SPX and SPY. The difference between the two (SPY's dividends and fees) sits in their forwards.
5. **The live Yahoo fallbacks** (`vol_surface.py`, `routers/local_vol_pricing.py`, `routers/simulation.py`) stay as they are, as decided (CLAUDE.md): neither extended nor removed in this work package.
6. **The other consumers of the chain** move with `market_snapshot`:
   - `calibration_warmup.py`;
   - `multi_asset_market.py`;
   - `routers/market_price_tape.py`;
   - the Market page's Vol tab.
7. **Tests**: the stored SPY day's surface reprices its OTM mids within their bid/ask, and its log contract matches the market's (§8).

## 6. Review of the filters used so far (`CleaningParams`, `RawVolSurface`)

Each filter of the current pipeline, what it does, what is wrong with it, what the literature does, and what to do. The literature column is to be completed by L0 (§7). The rows cited there come from what L0 has already read.

| Filter (current) | What it does | Problem | Practice in the literature | Proposal |
|---|---|---|---|---|
| Open interest ≥ 10, or volume ≥ 10 when OI is unpopulated | liquidity | OI is the previous night's; zero on some products outside the session (seen on SPX/XSP); filtering on yesterday says nothing of today's quote | Tan, Roberts & Zohren (2026, on OptionMetrics) keep OI > 0 at the formation date; others drop zero-volume quotes (Cheng, Cheng & Peng 2024) | a flag, not a filter; the spread and the bid measure today's quote |
| `min_bid` 0.05 | price floor | a fixed dollar amount whatever the underlying; on XSP or a cheap stock it is several ticks | at least two ticks (Cohort et al. 2018); the VIX strip stops after two consecutive zero bids | two ticks of the product's tick size; zero bid flagged |
| `max_spread_ratio` 0.50 | (ask − bid) / mid ≤ 50 % | very loose; a hard cut where a weight would do | 5 % primary, 15 % for robustness in a crisis (Damircheli 2026, on OptionMetrics SPX) | spread as the fit's weight (1/spread in vol units); a cut only for the absurd (start: 50 %, to measure) |
| `min/max_plausible_iv` 2 %–300 % | rejects Yahoo's absurd IVs | applies to Yahoo's IV, which is gone | — | kept as a sanity check on **our** IV |
| Moneyness 0.70 ≤ K/S ≤ 1.40 | drops the wings | cuts, at ingestion, the wings the variance swap and the fit's extrapolation need (the market's fair variance moves from 0.0689 to 0.0567 between the full strip and a 50 % floor); in K/S rather than K/F | the VIX strip takes every OTM strike with a bid; academic samples cut at K/F ∈ [0.8, 1.2] or [0.85, 1.15] for a fit, not for storage | no cut at ingestion; the fit chooses its band in ln(K/F) (WP 30) |
| Calendar arbitrage: per (side, strike rounded to 0.5), w = IV² T non-decreasing in T, else dropped | removes quotes that break calendar monotonicity | at fixed **strike**, whereas the condition holds at fixed **forward moneyness** with carry (Gatheral & Jacquier 2014); on Yahoo's IVs; greedy (keeps the first, drops the later ones); silent | checked on the fitted surface, or on total variance at fixed k | checked on our IVs at fixed k, flagged; the eSSVI calendar condition (WP 30) enforces it on the surface |
| Butterfly: calls only, C(K1) − 2 C(K2) + C(K3) ≥ 0 on the mids, the middle dropped iteratively | removes non-convex triples | on the calls' mids, ITM included (wide spreads); puts never checked; a violation within the spreads is no arbitrage; drops instead of flags | violations judged against bid/ask (Madan et al. 2019 for parity; Fengler 2009 for convexity) | on the European-equivalent OTM prices turned into calls by parity, normalised by F; a violation beyond the spreads only; flagged |
| *(absent)* minimum maturity | — | the shortest expiries (days) have a noisy IV and an ill-defined settlement time | under 6–7 days dropped (VIX-options studies: Kyakutwika, Alfeus & Schlögl 2025; Bakshi, Cao & Chen 1997) | flag under 7 days; the fit decides |
| *(absent)* parity check | — | the two-smiles defect of §1.1 passed unseen | bid/ask parity bounds (Madan et al. 2019); forward and DF by robust regression (Cohort et al. 2018) | §4.3 |
| *(absent)* stale quotes | — | no `last_trade_date` stored | — | stored and flagged (§3, §4.1) |
| *(absent)* expiry type | — | SPY's end-of-quarter expiries are inconsistent; SPX and SPXW share dates | studies on OptionMetrics keep the standard monthlies only (Tan, Roberts & Zohren 2026) | rejected on their parity dispersion (§4.3), the root kept (§4.1) |

## 7. Literature review: good practice for option data (lot L0)

**Goal.** Before coding, compare our choices (§4, §6) with the published practice and fix the thresholds on evidence. The output is this section completed with one line per source (what it does, what we take) and the thresholds of §6 confirmed or changed.

**Already in `kbase`** (`mcp__kbase__kb_search`, collection `quant`):
- Cohort, Corbetta, Martini & Laachir, *Robust calibration and arbitrage-free interpolation of SSVI slices*, 2018 ([arXiv:1804.04924](https://arxiv.org/abs/1804.04924)), §4.1: forward and DF by robust regression on parity, OTM only, two-tick floor, Jäckel's inversion.
- Andersen, Itkin & Kazbek, *Valuing American options and flexible forwards contracts in time-dependent models*, 2026 ([arXiv:2606.27335](https://arxiv.org/abs/2606.27335)), §1: de-Americanization as practice, with its references (Broadie & Detemple 2007; Burkovska et al. 2018; De Marco & Henry-Labordère 2017).
- Studies on OptionMetrics data, for their filters (§6): Damircheli 2026 ([arXiv:2610.07554](https://arxiv.org/abs/2610.07554)), Tan, Roberts & Zohren 2026 ([arXiv:2609.33767](https://arxiv.org/abs/2609.33767)), Zhuang & Wu 2025 ([arXiv:2506.22888](https://arxiv.org/abs/2506.22888)), Kyakutwika, Alfeus & Schlögl 2025 ([arXiv:2506.23409](https://arxiv.org/abs/2506.23409)).
- Wu, Zhong & Ouyang 2025 ([arXiv:2510.19126](https://arxiv.org/abs/2510.19126)): the bid/ask parity bounds of Madan et al. 2019.
- Hull, *Options, Futures, and Other Derivatives*, 11th ed., §15.11 (implied vol, trees for American options).
- Gatheral & Jacquier 2014 and Hendriks & Martini 2019 (WP 30's sources, the arbitrage conditions).

**To fetch and ingest** (AgenticEnv, a wave of its own):
- *Cboe Volatility Index (VIX) White Paper / Methodology*: the strip's strike selection, the mid, the two-zero-bid stop, the forward from parity.
- *OptionMetrics IvyDB US Reference Manual*: how the reference vendor computes its IVs (CRR tree, discrete dividends, zero curve, closing quotes) and its standardised surface.
- Jäckel, *Let's Be Rational*, Wilmott, 2015: the inversion.
- Burkovska, Glau, Mahlstedt & Wohlmuth, *Calibration to American options: numerical investigation of the de-Americanization method*, Quantitative Finance, 2018.
- Hentschel, *Errors in implied volatility estimation*, Journal of Financial and Quantitative Analysis, 2003: how price errors and wrong inputs (forward, rate) bias the IVs. Exactly §1.1's case.
- Bakshi, Cao & Chen, *Empirical performance of alternative option pricing models*, Journal of Finance, 1997, and Dumas, Fleming & Whaley, *Implied volatility functions: empirical tests*, Journal of Finance, 1998: the classical sample filters.
- Constantinides, Jackwerth & Savov, *The puzzle of index option returns*, Review of Asset Pricing Studies, 2013: their filters on OptionMetrics.
- Fengler, *Arbitrage-free smoothing of the implied volatility surface*, Quantitative Finance, 2009: convexity and calendar on noisy prices.
- Andersen, Lake & Offengelt, *High-performance American option pricing*, Journal of Computational Finance, 2016, as a fast alternative to the tree; Barone-Adesi & Whaley 1987 as the approximation it replaces.
- Carr & Madan, *Towards a theory of volatility trading*, 1998; Demeterfi, Derman, Kamal & Zou, *More than you ever wanted to know about volatility swaps*, 1999: the log contract of §8.

**Questions the review answers**:
- spread and tick thresholds;
- minimum maturity;
- dividend projection for the tree;
- which rate curve (D3);
- whether the AM/PM settlement time matters at our horizons;
- how vendors treat end-of-quarter and weekly expiries.

## 8. Validation

On every stored day, and as the acceptance of L2/L3/L5:
- **Call/put agreement.** Within the band around F, the call and put IVs agree to half a spread in vol.
- **Model-free references.** The variance swap by log contract on the OTM mids, and the digital put by slope less early exercise (`replication3.py` of #384, moved into `data-ingest`'s tests or a script of `scripts/`):
  - the surface's log contract within the market's tail range (§1.2);
  - the surface's digital within the market's error.
- **Repricing.** The SVI and eSSVI surfaces reprice their OTM mids within bid/ask on most quotes; the share is measured and recorded.
- **The #384 measurement rerun**, at r = 4 %:
  - SVI on the old cleaning;
  - SVI on the new one;
  - eSSVI on the new one;
  - all against the references above.

## 9. Lots

| Lot | Repo | Content | Accepted when |
|---|---|---|---|
| L0 | AgenticEnv, this one | Literature review (§7): fetch, ingest, read; §6 and §7 completed | thresholds fixed on sources; this file updated |
| L1 | `data-ingest` | Universe `^SPX`, `^XSP`; `contract_symbol`, root, `last_trade_date` stored; SPX and XSP strike coverage measured (it chooses D2's primary; the Cboe comparison was dropped); index level from `^GSPC`, already stored | **Done** (data-ingest #32): a day of SPX/XSP stored; coverage in `docs/option-coverage.md` |
| L2 | `data-ingest` | `options-chain-iv`, European branch: parity forward and DF (§4.3), OTM, Black-76 by Jäckel, flags, registry, DAG task | **Done** (data-ingest #33): tests of §4.6 (round trip, European) passed; SPX/XSP day computed |
| L3 | `data-ingest` | American branch: DF from SPX/XSP parity interpolated in T, SOFR fallback (D3); dividends (continuous for ETFs, discrete projected for stocks); tree de-Americanization; parity bounds; expiry rejection | **Done** (data-ingest #33): tests of §4.6 as amended in §4.7 |
| L4 | `data-ingest` | Backfill of every stored day; method version | **Done** (data-ingest #33): `ingest run options-chain-iv --full`; counts per day logged |
| L5 | this one | `market_snapshot` and `RawVolSurface` on `chain_iv`, stored forwards; S&P 500 products on the XSP/SPX surface, SPY's as a cross-check (D2); filters as §6 decides; the other consumers (§5.6) | the European surface reprices its OTM mids (§8); SPY's cross-check agrees within errors; pytest and C++ tests |
| L6 | this one | The pricers' forward (D1): market forwards and discounts per maturity, `rate` as an override (API field, Pricing page label); `QM_OPTION_DATA` switch (D5) | a vanilla reprices on the surface's forward under every model; `legacy` gives today's numbers bit for bit |
| L7 | this one | The #384 measurement rerun (§8) and the maintainer's decision on E2 (#381) | table presented; WP 30 status updated |
| L8 | both | Documentation: `options_chain.py`'s docstring, `docs/sources.md` (`data-ingest`), this file's status | `data-ingest` side and this file done; quant-modeling's own docs with L5 |

Order: L0 first, then L1 → L2 → L3 → L4 → L5 → L6 → L7. L1 and L2 on SPX/XSP do not wait for L3. L5 can start on the European tickers alone.

## 10. Decisions (taken with the maintainer on 2026-11-26, except D4)

- **D1 — The pricers drift at the market's forward.**
  - The forwards and discounts read with the surface, maturity by maturity (the implied carry), are the pricers' forward and discount. A calibrated vanilla then reprices exactly under every model.
  - The Pricing page's `rate` becomes an optional override. Its label and the API's field description say so, and a typed rate is reported as a departure from the market.
  - Lot L6.
- **D2 — S&P 500 products are priced on the European surface (XSP or SPX).**
  - The surface is in ln(K/F), so it transfers to SPY.
  - SPY's own surface, de-Americanized, stays as a cross-check: the same day's two surfaces should agree within their errors, and a gap is a data alarm.
  - XSP or SPX as primary is chosen on L1's coverage measurement: XSP has strikes at SPY's scale, tight spreads and expiries to 2028; SPX has more expiries, to 2031, but yfinance truncates its strikes.
  - **L1's measurement points to SPX** (§4.7): relative spreads three to five times tighter than XSP's, wings to K/S 0.03 on the monthlies, and no truncation on them. XSP stays as a second European chain on the same underlying, for the discount curve and a cross-check.
  - No de-Americanization sits on the main path of S&P 500 products.
- **D3 — The discount of American options is SPX's.**
  - The discount factor implied by the same day's SPX/XSP parity, interpolated in T to the expiry: the option market's own financing rate, valid for every USD underlying.
  - Fallback when no European chain is stored that day: the SOFR curve. `data-ingest` builds it from the stored DTCC swaps or reads it from what quant-modeling builds (WP 23's `usd-sofr`); which one is settled in L3.
  - **Settled in L3 (2026-11-26):** neither. The fallback reads a curve directly from FRED: SOFR at the overnight point, then the Treasury CMT yields (§4.7).
  - A fallback day is flagged in `chain_iv_expiries`.
- **D4 — Filter thresholds (§6): open**, decided once L0 has read the sources.
- **D5 — A switch for the data, then E2 separately.**
  - `QM_OPTION_DATA=chain_iv|legacy` (default `legacy` until the maintainer switches), read by `market_snapshot`, the surface model in the calibration's cache key as for `QM_VOL_SURFACE`.
  - The switch goes live after a comparative measurement is presented (§8). Rollback needs no code change.
  - E2 (eSSVI, `QM_VOL_SURFACE`) comes after, with its own switch, so that each price move is attributed to its cause.

## 11. References

See §7. In the text:
- Carr & Madan 1998; Demeterfi, Derman, Kamal & Zou 1999 (the log contract);
- Cohort, Corbetta, Martini & Laachir 2018 (parity regression, OTM, two ticks, Jäckel);
- Jäckel 2015 (inversion);
- Burkovska, Glau, Mahlstedt & Wohlmuth 2018; Andersen, Itkin & Kazbek 2026 (de-Americanization);
- Madan et al. 2019 (parity bounds with bid/ask);
- Gatheral & Jacquier 2014; Hendriks & Martini 2019 (arbitrage conditions);
- Fengler 2009 (smoothing under arbitrage constraints);
- Hentschel 2003 (errors in implied vols).
