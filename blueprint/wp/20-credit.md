# WP 20 — Crédit : courbes, CDS, modèle structurel, page `/credit`

| | |
|---|---|
| **Dépend de** | chantier 0 de la [roadmap](../../etc/roadmap.md) (bootstrap de courbe), [08](08-market-data.md) (page market, onglet rates) |
| **Débloque** | chantier 4c–4d (exposition, CVA) : la courbe de survie de la contrepartie |
| **Branches** | `feat/credit-curves` (ici), `feat/credit-and-sec-fundamentals` (`~/data-ingest`) |

## Ce qui est fait

**Données (`~/data-ingest`).**
- `fred-macro` : OAS ICE BofA par notation (AAA → CCC), OAS investment grade
  par maturité (1-3Y → 15Y+), rendements Moody's Aaa / Baa.
- `sec-fundamentals` → `fundamentals.sec_facts` : ~60 concepts XBRL de chaque
  10-K / 10-Q du S&P 500 (EDGAR, `companyfacts`), stockés bruts, une ligne par
  fait **et** par dépôt (point-in-time). `sec-filings` → `fundamentals.sec_filings` :
  l'index des dépôts avec l'URL du document, le nom et le SIC. Une société à
  plusieurs classes (GOOG / GOOGL) est une seule clé CIK, `tickers` les liste.
  `SEC_USER_AGENT` obligatoire (403 sinon).

**C++.**
- `market/credit_curve.hpp` : hazard rate constant par morceaux, survie.
- `instruments/credit/cds.hpp`, `models/credit/intensity.hpp`,
  `engines/analytic/cds.hpp` : CDS (jambe de prime avec couru au défaut, jambe
  de protection), intégrales exactes entre les nœuds (ISDA Standard Model,
  O'Kane 2008 ch. 6). Moteur branché sur le visiteur (`CdsAnalyticEngine`).
- `market/credit_bootstrap.hpp` : bootstrap depuis des spreads par ; une courbe
  qui décroît trop vite est une erreur explicite, jamais un plancher.
- `models/credit/merton_structural.hpp` : Merton 1974, calibration (V, σ_V)
  par Newton sur (E, σ_E).
- Tests de propriété (`testCredit.cpp`, `testMertonStructural.cpp`) : triangle
  de crédit exact à taux nul, erreur d'ordre 1 en Δ sinon (rapport 13 entre
  trimestriel et hebdomadaire), jambe de protection = quadrature, repricing au
  pair, E + B = V, s = −ln(1 − PD·LGD)/T, aller-retour de calibration.

**API** (`api/app/credit.py`, `fundamentals.py`, `routers/credit.py`, sous
`/api/credit/*`) et **page** `/credit` (onglets *Credit curves* et *Companies*),
méthodologie servie avec les données.

## Ce qui manque, et pourquoi

- **Cotations CDS single-name** : aucune source gratuite (Markit, ICE,
  Bloomberg). La page montre des spreads obligataires, avec la base CDS–obligation
  et la prime de liquidité dites dans la méthodologie. Le moteur accepte de
  vraies cotations CDS dès qu'une source existe.
- **Placement des buckets de maturité** : milieux de bucket, 15Y+ à 20 ans —
  la maturité moyenne ICE par bucket n'est pas gratuite. Convention affichée.
- **Dette hors concepts us-gaap standard** (Ford, extensions XBRL propres) : pas
  de point de défaut, la page le dit.
- **Suite** : chantier 4a (swaps, swaptions, multi-courbe), puis l'exposition
  (4c) qui consommera `CreditCurve` pour la CVA ; sensibilités aux spreads par
  AAD (les jambes sont différentiables, pas encore templatées sur `aad::Number`).
