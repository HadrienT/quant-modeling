# quant-modeling — notes pour Claude Code

Librairie de pricing de dérivés en **C++20**, exposée jusqu'à un produit
utilisable : `include/quantModeling` + `src` → bindings **pybind11** →
API **FastAPI** → front **React/Vite**. ~28 000 LOC C++, 25 fichiers de tests
GoogleTest, CI GitHub Actions.

Le découpage C++ est celui d'une lib de desk et il faut le préserver :
**payoff** (`instruments/`), **modèle** (`models/`), **méthode numérique**
(`engines/`), **orchestration** (`pricers/` avec registry + adapters). Aucun
engine ne doit brancher sur un type de produit ; aucun instrument ne doit lire
de donnée de marché.

## La chaîne, couche par couche

| Couche | Où | Ce qu'elle contient |
|---|---|---|
| Cœur C++ | `include/quantModeling/`, `src/` | `core` (types, timegrid, results), `market`, `instruments`, `models`, `engines` (analytic / tree / pde / mc), `pricers`, `utils` (Sobol, pont brownien, control variates, greeks) |
| Bindings | `bindings/python/` | pybind11 → wheel `quantmodeling` |
| API | `api/app/` | FastAPI : `routers/` (pricing, market, portfolio, backtest, auth, local-vol), `local_vol/` (nettoyage des quotes, surface IV, Dupire), auth JWT, cache |
| Front | `web/src/` | React 18 + Vite + TypeScript |
| CLI | `main.cpp` | binaire de démo |

Les données de marché viennent de **yfinance** et **BigQuery** (price tape,
chaînes d'options), les courbes de taux de **FRED**.

## Commandes

| | |
|---|---|
| `cmake --preset default && cmake --build build` | build C++ (Ninja, Release, tests ON) |
| `ctest --test-dir build --output-on-failure` | tests C++ — ou `scripts/make.sh` qui enchaîne les trois |
| `cmake --preset asan` | build sanitizers (address + UB) |
| `cmake --preset release` | `-march=native` + benchmarks google-benchmark |
| `scripts/build_wheel.sh` | wheel pybind11 dans `dist/` |
| `scripts/run_api.sh` | venv + wheel + `uvicorn --reload` |
| `docker compose up --build` | stack complète (API + front avec HMR) |
| `cd web && npm run dev` | front seul |
| `cd web && npx tsc --noEmit` | typecheck du front |
| `cd web && npm run lint` | eslint |
| `cd web && npm run build` | build de production Vite |

Les ports hôte du `docker compose` sont paramétrables — `QM_API_PORT` (défaut
8010) et `QM_WEB_PORT` (défaut 5180) — parce que plusieurs projets tournent sur
le même serveur auto-hébergé. Copier `.env.placeholder` en `.env`.

## Suivi du travail — GitHub Issues, pas de markdown de handoff

Le « JIRA » du projet, ce sont les **GitHub Issues du repo**. `gh` est
authentifié (compte `HadrienT`).

- **Au démarrage d'une session** : `gh issue list --state open`.
- **Issue traitée** → `gh issue close <n> --comment "fait dans <sha>"`.
- **Jamais** de fichier markdown de passation entre sessions. Une tâche qui
  survit à la session est une issue.
- Les gros morceaux de **conception** vivent dans `blueprint/wp/*.md` ; l'issue
  y renvoie, elle ne les remplace pas.
- `blueprint/README.md` porte le graphe de dépendances entre lots de travail :
  le consulter avant de démarrer un lot, pour vérifier que ses prérequis sont
  faits.

## Documents de référence

| Fichier | Rôle |
|---|---|
| `blueprint/` | Spécification du chantier en cours (réécriture du front). Lots de travail, dépendances, critères d'acceptation. |
| `etc/roadmap.md` | Stratégie six mois côté quant : vol stochastique calibrée, AAD, Monte-Carlo GPU, capstone xVA. Document de stratégie, pas de spec. |
| `etc/todo.md` | Checklist « desk grade » — ce qui manque pour ressembler à une lib de production. |
| `etc/structure.md` | Arborescence cible du module de pricing. |

**Règle de la roadmap qu'il ne faut pas contourner : arrêter d'ajouter des
produits.** Le catalogue est déjà plus large que nécessaire ; la valeur
marginale d'un 25ᵉ payoff est nulle. La profondeur (calibration, greeks
adjoints, GPU) bat la largeur.

## Conventions

- **Conversation avec le mainteneur : en français.** Code, identifiants,
  commentaires, messages de commit : en anglais. `blueprint/` et `etc/` sont en
  français.
- **Commits : passer par une branche, jamais directement sur `main`.** Terminer
  les messages par `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
- **C++** : `clang-format --style=file` est vérifié en CI et en pre-commit — un
  fichier mal formaté casse le build. **Python** : `black`.
- Le hook pre-push lance `cmake + ctest` : un push avec des tests C++ rouges
  n'aboutit pas.
- Tout nouvel engine ou instrument arrive avec son test dans `tests/`, et de
  préférence un test de *propriété* (parité call-put, `in + out = vanille`,
  bornes de monotonie, ordre de convergence mesuré en log-log) plutôt qu'une
  simple égalité numérique.
- Le front est en cours de réécriture (voir `blueprint/`). Ne pas investir dans
  l'ancien code de `web/src/` : les fichiers destinés à disparaître sont
  listés dans `blueprint/wp/00-foundations.md`.

## Déploiement

Le projet est **auto-hébergé**, pas sur Google Cloud — le coût GCP était le
motif de la bascule. `cloudbuild.yaml` et la cible Cloud Run de
`web/Dockerfile.prod` sont l'héritage de l'ancien déploiement : les traiter
comme du legacy, ne pas construire dessus. La cible actuelle est le
`docker compose` derrière un reverse proxy sur le serveur (voir
`blueprint/wp/13-deploy-selfhost.md`).

Attention au piège classique de Vite : les variables `VITE_*` sont **inlinées
au build**, pas lues au runtime. Changer `VITE_API_BASE` impose de rebuilder
l'image, ou de passer par le mécanisme d'injection runtime décrit dans le lot
de déploiement.
