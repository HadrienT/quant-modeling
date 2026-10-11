# WP 24 — Backtest : la moitié de quant-modeling

| | |
|---|---|
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

- **La page `/backtest` est masquée** depuis le 11 octobre 2026 : elle
  affichait des chiffres faux (alpha quotidien affiché « 0.00 », taux sans
  risque d'aujourd'hui appliqué au passé, indice sans dividendes face à des
  titres avec). Elle est retirée du menu, de l'accueil et du sitemap ; l'URL
  répond « not found ».
- **L'ancien code n'est pas repris.** `api/app/routers/backtest.py` et
  `web/src/features/backtest/` restent dans l'arbre jusqu'à leur remplacement,
  pour que le contrat OpenAPI et le typecheck ne bougent pas entre-temps. Ne
  pas les corriger, ne pas les étendre.
- La route `POST /api/backtest/run` répond encore. Plus rien ne l'appelle.

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
