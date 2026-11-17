# WP 28 — Information architecture and user journeys *(front end)*

Status: lots U1 and U2 done (#290). Depends on WP 03 (shell), 07 (workbench), 08 (market),
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

## 4. What was built

**U1.** `NAV_GROUPS` and `PAGE_HINTS` sit next to `NAV_PAGES` in
`shared/config/pages.ts`; `useNavPages()` returns the visible groups. The bar
(`app/layout/NavGroups.tsx`) is a disclosure per space: a button that shows a
panel of plain links, closed on Escape, an outside click, a focus that leaves it
and navigation. Not Radix's popover: opening it hung the jsdom tests, and a site
navigation is a list of links, not a menu widget. The bar holds the four spaces
from `md` (768 px) up, against `lg` for the eleven links before.

**U2.** Four bridges, each a plain link carrying its state in the URL:

| From | To | How |
|---|---|---|
| Market, vol tab, each tenor of the delta matrix | Workbench, European option | `?p=` with spot = strike = the surface's spot, the tenor in years, its ATM vol. Rate and dividend are not in the surface: they stay at the form's defaults, which the caption under the matrix says |
| Credit, ratings table | xVA | `?cpty=` (`CCC and lower` → `CCC`) |
| Workbench header, products a portfolio can hold | Portfolio, trade form open | `/portfolio?trade=` (the catalog key and the form values). It survives the selection of a portfolio, and a visitor with none is told the form opens once one is created. Dropped from the URL once the trade is booked or cancelled |
| Portfolio, expanded row of a derivative position | Workbench | The contract terms of the instrument, the market inputs of its latest mark (same names as the request: spot, rate, dividend, vol), the time left to expiry. Offered only when the product is in the workbench and the position has not expired |

The last bridge needs the inverse of a product's `toRequest`
(`shared/products/portfolioProducts.ts`, `formValuesFromRequest`). Which fields
the form keeps in percent is written nowhere, so it is read off `toRequest` by
probing each numeric field, and the result is checked by a round trip: a field
that would not come back to the same number refuses the link. A property test
round-trips every product a portfolio can hold. The catalog key → pricer id
table moved from the portfolio feature to `shared/products`, so the workbench
and the portfolio share it without importing each other.

Checked in a browser (Playwright, Vite with no API behind it, so no valuation
reaches the production audit trail): the menus in both themes at 1280 px and at
800 px, the folded menu at 390 px with no horizontal scroll, the palette, and
the workbench → portfolio journey through the creation of a first portfolio.
Not checked in a browser: the two bridges that need market data (vol matrix,
ratings table); their integration tests check the URLs they build.

## 5. Rules

- A bridge is a link, never shared state between features: a feature does not
  import another feature (`web/tests/discipline.test.ts`). What two features
  share goes to `shared/`.
- The groups live in `shared/config/pages.ts` next to `NAV_PAGES`. `NAV_PAGES`
  stays the flat list the API mirrors; the groups only reference its paths.
- Any figure a starting point leads to follows the existing rule: a defensible
  number, its model and inputs next to it.
- Interface text in English.

## 6. Out of scope

New products, new API routes (before U6 decides), a visual redesign of the pages
themselves.
