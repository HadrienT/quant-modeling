# WP 28 — Information architecture and user journeys *(front end)*

Status: lot U1 in progress. Depends on WP 03 (shell), 07 (workbench), 08 (market),
09 (portfolio). No API change before lot U5.

## 1. Why

The front end was rewritten page by page (WP 00–14), then each core project added
its own page (credit, xVA, scripting, backtest). Every page is sound; the whole
reads as a list of the library's modules, not as a tool:

| Finding | Where | Consequence |
|---|---|---|
| 11 navigation entries on one flat line | `shared/config/pages.ts`, `app/layout/Nav.tsx` | Nothing says where to start or what belongs together; the bar folds into a menu below 1024 px |
| Pages do not talk to each other | Market shows a surface, Pricing asks for the same inputs again; a priced product cannot be sent to a portfolio | Each page is a dead end. Only three links to the workbench exist outside it (`SwapMarketPanel`, `FxCorrelationPanel`, `ProductReference`) |
| The workbench shows everything at once | `features/pricing/PricingPage.tsx`: engine tabs, device picker, device race, form, A/B compare, convergence, market vega | A visitor who wants a price and its greeks reads six controls first |
| The home page describes the stack | `features/home/HomePage.tsx` | It tells what the site is made of, not what one can do in one click |
| xVA exists twice | `/xva` (preset netting sets) and the xVA tab of a portfolio | Two entry points with different inputs for the same computation (see #173) |
| Each page has its own ticker picker | `MarketTickerPicker`, `TickerPicker`, simulation, backtest | The underlying chosen on one page is lost on the next |

## 2. Target: four spaces, in the order a desk works

| Space | Question it answers | Pages |
|---|---|---|
| **Market** | What does the market say? | Market (prices, vol, rates, FX), Credit |
| **Price** | What is one product worth? | Pricing, Scripting, Strategies |
| **Risk** | What does a book cost and risk? | Portfolio, xVA, Backtest |
| **Learn** | How does it work? | Products (reference), Simulation, About |

The navigation shows the four spaces; each opens onto its pages with a one-line
description. URLs do not change before lot U5, so shared links, the admin panel's
hidden pages (`api/app/site_pages.py`) and `PageGate` keep working unchanged. A
space whose pages are all hidden from a visitor does not show.

## 3. Lots

| Lot | Content | Acceptance |
|---|---|---|
| **U1** | Grouped navigation (desktop menus, mobile sections), command palette grouped the same way, home page organised by space with three one-click starting points (pre-filled URLs) | 4 entries in the bar at ≥ 1024 px; every page reachable in two clicks; a hidden page is absent from its menu for a visitor and flagged for an administrator; keyboard navigation of the menus (Radix); existing tests pass |
| **U2** | Bridges: Market → "Price an option on this ticker" (spot, vol, rate pre-filled through `?p=`), Pricing → "Add to portfolio", Portfolio position → "Open in workbench", Credit → "Use as counterparty" in xVA (`?cpty=`) | Each bridge is a plain link carrying its state in the URL; an integration test per bridge checks the pre-filled values |
| **U3** | Progressive disclosure in the workbench: product, inputs, price and greeks by default, the model chosen for the user and announced; convergence, device race, model comparison and market vega in a "Diagnostics" section, closed by default, its state kept in the URL | Nothing is removed; a shared link reopens the section as it was; `PricingPage.tsx` stays under the 230-line cap |
| **U4** | A market context shared by the pages: the ticker (and currency) last chosen follows the visitor, shown in the bar, stored in the URL first and `localStorage` second | Choosing SPY in Market then opening Pricing, Simulation or Backtest starts on SPY; an explicit URL parameter always wins |
| **U5** | Route consolidation, each behind a redirect as `/rates` was: Credit as a tab of Market, Strategies under Price, one xVA entry point. **Needs a maintainer decision on the xVA merge (#173) before starting**; changes `HIDEABLE_PAGES` on the API side | Old URLs redirect; `test_site_pages.py` agrees with `pages.ts` |
| **U6** | Front end / API drift: list the API routes no page uses (88 routes, `web/openapi.json`) and decide for each: surface it, or leave it API-only | A table in this file; one issue per route to surface |

Order: U1, U2 change how the site is perceived without rewriting a page; U3 next;
U4 and U5 once the journeys exist; U6 can run at any time.

## 4. Rules

- A bridge is a link, never shared state between features: a feature does not
  import another feature (`web/tests/discipline.test.ts`). What two features
  share goes to `shared/`.
- The groups live in `shared/config/pages.ts` next to `NAV_PAGES`. `NAV_PAGES`
  stays the flat list the API mirrors; the groups only reference its paths.
- Any figure a starting point leads to follows the existing rule: a defensible
  number, its model and inputs next to it.
- Interface text in English.

## 5. Out of scope

New products, new API routes (before U6 decides), a visual redesign of the pages
themselves.
