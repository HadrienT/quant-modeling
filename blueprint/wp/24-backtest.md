# WP 24 — Backtest : la moitié de quant-modeling

| | |
|---|---|
| **Status** | In progress (lot B8, issue #166). Done: the wheel, the point-in-time reads, the router, the `backtest.run` event and its replay, the strategy tab, the hedging tab with its tasks, the trials journal, the S&P 500 universe at date. Left: a stress test and a VaR on the trade ledger (#292), then the risk-model tab on them. |
| **Dépend de** | le dépôt `~/quant-backtest` ([GitHub](https://github.com/HadrienT/quant-backtest)) et ses lots B2 à B7 ; la piste d'audit et le replay ([WP 18](18-observability.md)) ; le portefeuille et sa VaR ([WP 09](09-portfolio-risk.md)) ; le dépôt `~/data-ingest` (lot B1) |
| **Débloque** | une page `/backtest` dont chaque chiffre est défendable |
| **Branches** | `feat/backtest-b8-…`, une par tranche |

Le backtest ne vit pas dans ce dépôt. Le moteur est une lib C++ à part,
`~/quant-backtest`, livrée comme un wheel pybind11 `quantbacktest` : décision
du mainteneur du 11 octobre 2026, pour la performance et pour ne pas charger
le dépôt de pricing. Ce lot ne décrit que ce qui reste ici, comme le
[WP 18](18-observability.md) ne décrit que la moitié « producteur » de
`quant-platform`.

Tout le reste est là-bas, et y est canonique :

| Sujet | Où |
|---|---|
| Ce qu'un desk appelle backtest, formules et références ; les quinze constats sur l'ancienne page ; l'architecture ; les lots B0 à B8 ; les tests de propriété ; l'état des données | `~/quant-backtest/docs/companion.md` |
| Qui possède quoi entre les deux dépôts, forme de l'interface, épinglage des versions | `~/quant-backtest/docs/contract.md` |
| Décisions et alternatives écartées (dépôt séparé, C++, wheel plutôt que service, repartir de zéro) | `~/quant-backtest/blueprint/decisions.md` |

## 1. État

- **L'onglet stratégie est en ligne** (12 octobre 2026). La page `/backtest`
  est revenue dans le menu, l'accueil et le sitemap. L'ancien routeur
  (`POST /api/backtest/run`) et l'ancienne page ont été supprimés, sans rien
  en reprendre.
- **Fait** : lectures point-in-time (`db.py`, `backtest/market.py`), routeur
  (`GET /api/backtest/universe`, `POST /api/backtest/strategy`,
  `POST /api/backtest/validation`), événement `backtest.run` et son replay
  (`backtest/record.py`, `POST /api/admin/replay/{id}`), page avec cinq
  volets : performance, coûts, significativité, surapprentissage, données et
  conventions.
- **The hedging tab is done** (2026-11-17), and with it the "Task" slice: an
  option bought and delta-hedged in a simulated world (Black-Scholes, Heston
  typed, or Heston calibrated on a ticker's stored chain by
  `stochastic_vol.py`), up to five hedges on the same paths, the
  minimum-variance delta fitted on another seed. A Heston world prices the
  option on every path at every date (about 60 s for 20 000 paths and the
  true model's delta on four threads of the development server), so the run
  is a task: `POST /api/backtest/hedging` returns its id, `GET
  /api/backtest/tasks/{id}` its progress then its result, `DELETE` cancels
  it (`backtest/tasks.py`: one worker, the server has two CPUs; at most four
  tasks queued or running; a finished task readable for an hour). The run is
  a `backtest.run` event of kind `hedging`, replayed like the others. The
  strategy runs stay synchronous: two seconds.
- **The trials journal is done** (2026-11-17). The deflated Sharpe ratio
  counts, beside the request's grid, every other configuration recorded on
  the same universe and period: the strategy and validation `backtest.run`
  events of the audit trail, read with the `audit_reader` role
  (`audit/store.py`, `backtest_requests`; the same tickers in any order, the
  same start and end). Their configurations are run again by the wheel on the
  request's days, cost and timing (at most 96, the most recent), and only the
  deflated ratio sees them: the walk-forward, the purged cross-validation and
  the overfitting probability judge the request's grid. A configuration the
  engine refuses on these data is not a trial, and the text says so. The set
  counted is fingerprinted (`journal`): a replay after more trials reports
  drifted inputs, not a different figure. Without the audit database (the
  development server, CI) only the request counts, and the page says why.
- **The S&P 500 universe at date is done** (2026-11-17). `universe: "sp500"`
  takes every member of the window that has prices and hands the engine the
  membership table (`prices.sp500_membership`, latest snapshot): a name is
  held only from the day it joins to the day it leaves, with no look-ahead in
  the composition. Survivorship remains (a delisted company has no prices:
  271 of the 499 members of January 2010 have one), and the page says so.
  Risk parity (107 s on 532 names over fourteen years) and the validation
  grid are refused there; equal weights, minimum variance and maximum Sharpe
  take 13 to 22 s end to end on the development server. The holdings chart
  shows the twelve largest names and the others summed.
- **Reste à faire** : onglet modèle de risque, sur la VaR du registre (#292).
- **Lectures « as-of »** : `macro.fred_series` et `factors.french_daily` ne
  sont versionnées que depuis leur chargement (septembre et octobre 2026). Le
  backtest lit le **premier millésime stocké** de chaque date : ce que la base
  a de plus proche de la valeur connue ce jour-là, et qu'une révision
  ultérieure ne déplace pas sous un replay.
- **Ce qui reste du calcul ici**, et rien d'autre (`backtest/engine.py`) :
  changements d'unité (points de base, pourcents), rendements
  `value[t] / value[t-1] - 1`, rendement moins taux sans risque, indices de
  ligne en dates. Côté page : l'indice de référence rebasé sur le capital
  initial pour être tracé à côté des livres.
- **Tranche « Wheel » faite.** Le moteur est épinglé par son commit dans
  `api/quantbacktest.pin`, seul endroit où l'épinglage vit : un commit plutôt
  qu'une version, parce qu'en `0.x` une signature peut changer sans que la
  version bouge. `scripts/install_quantbacktest.sh` le lit, et à travers lui
  `web/Dockerfile.prod`, `api/Dockerfile`, `scripts/run_api.sh` et le job CI
  `pytest` ; le commit est passé au build (`-DQB_GIT_SHA`), si bien que
  `build_info()["git_sha"]` le rend dans l'image aussi.
  `api/tests/test_quantbacktest_pin.py` échoue si le wheel installé n'est pas
  ce commit.

## 2. Ce que ce dépôt doit faire (lot B8)

Règle de partage : **tout calcul là-bas, toute entrée-sortie ici**.

| Tranche | Contenu |
|---|---|
| Wheel | `quantbacktest` épinglé à une version exacte, dans `web/Dockerfile.prod`, `scripts/run_api.sh` et le job CI `pytest` |
| Lecture | Requêtes point-in-time sur la base de `data-ingest` : prix bruts, dividendes et divisions, univers à date, taux court à date, facteurs. Une donnée absente est une erreur explicite. La clé de cache contient les dates (constat 1) |
| Routeur | `api/app/routers/backtest.py` réécrit : il valide la requête, lit, appelle le wheel, rend le résultat. Aucun calcul |
| Tâche | Un run long est une tâche avec progression et annulation, ce que le [WP 11 §2](11-backtest.md) demandait déjà |
| Audit | Événement `backtest.run` par `api/app/audit/` (`emit()`) : requête entière, empreinte des données lues, version du wheel et SHA du pricing qu'il embarque ; `POST /api/admin/replay/{id}` le rejoue (constat 15). Le journal des essais du Sharpe déflaté se compte dans ces événements |
| VaR | Série de P&L hypothétique quotidien et de VaR annoncée du [WP 09](09-portfolio-risk.md), passée aux tests de Kupiec et Christoffersen du wheel |
| Page | `/backtest` refaite et remise en ligne : onglets stratégie / couverture / modèle de risque, brut et net, intervalle sur chaque tuile, points de comparaison, rapport de qualité des données, marche en avant visible sur la courbe. En anglais, une source savante par notion, fichiers sous 230 lignes |

Les tranches se livrent une par une : un onglet dès que le lot du moteur qui le
nourrit est fait.

## 3. Critères d'acceptation

- Deux requêtes successives avec des dates de début différentes ne partagent
  pas leurs prix.
- Un run rejoué par `replay` redonne les mêmes chiffres.
- Aucun chiffre affiché sans convention ni incertitude.
- Les critères du [WP 11](11-backtest.md) restent tenus.
- Aucun calcul de backtest dans `api/app/` ni dans `include/` et `src/` : la
  lib de pricing ne sait pas ce qu'est un backtest.

## 4. Ce que la lib de pricing doit à l'autre dépôt

`quant-backtest` consomme `quantModeling` comme sous-projet CMake, à un commit
épinglé, pour son seul module de couverture. Les tests, bindings et benchmarks
sont déjà derrière des options éteintes par défaut ; le lot B0 de l'autre dépôt
vérifie que cela suffit. Si la lib ne se configure pas hors de la racine, ou
n'expose pas de cible utilisable, la correction se fait ici, par une issue
référencée des deux côtés.
