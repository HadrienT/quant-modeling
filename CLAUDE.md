# quant-modeling — notes pour Claude Code

Librairie de pricing de dérivés en **C++20**, exposée jusqu'à un produit
utilisable : `include/quantModeling` + `src` → bindings **pybind11** →
API **FastAPI** → front **React/Vite**. ~43 000 LOC C++ (`include/` + `src/`),
~70 fichiers de tests GoogleTest, CI GitHub Actions.

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
| API | `api/app/` | FastAPI : `routers/` (pricing, market, portfolio, backtest, auth, local-vol, assistant, admin), `local_vol/` (nettoyage des quotes, surface IV, Dupire), `assistant/` (chat LLM du scripting, voir plus bas), `audit/` (événements d'audit vers le Kafka de `quant-platform`), `valuation.py` + `replay.py` (enregistrement et replay des valorisations), `telemetry.py` (OpenTelemetry), auth JWT, cache |
| Front | `web/src/` | React 18 + Vite + TypeScript |
| CLI | `main.cpp` | binaire de démo |

## Dépôts voisins

Ce dépôt n'est pas seul sur le serveur ; trois dépôts frères, chacun avec son
propre `CLAUDE.md`, portent ce qui n'est pas du pricing :

| Dépôt | Rôle pour quant-modeling |
|---|---|
| `~/data-ingest` | **Toute l'ingestion des données de marché** (Airflow → Postgres). quant-modeling ne fait que lire cette base (`PGHOST=data-ingest-postgres` via le réseau Docker `dataplatform`). Une donnée manquante se corrige là-bas, pas ici |
| `~/quant-platform` ([GitHub](https://github.com/HadrienT/quant-platform)) | **Destination des logs, événements d'audit et de la télémétrie** : Kafka, Postgres d'audit append-only, OpenTelemetry Collector → Prometheus / Loki / Tempo, Grafana. Contrat producteur ↔ plateforme : `~/quant-platform/docs/contract.md`. Côté ici, le producteur est `api/app/audit/` (`emit()`, `record_fallback()`), conception dans `blueprint/wp/18-observability.md` |
| `~/AgenticEnv` | `llama-server` de l'assistant de scripting (voir plus bas) |

Branchement vers `quant-platform` (WP 18, fait) : en **prod**, les événements
d'audit partent dans son Kafka (`QM_AUDIT_SINK=kafka`, `kafka:9092`) et les
métriques / traces dans son OTel Collector (`otel-collector:4318`), par le
réseau `dataplatform` ; si la plateforme est absente, les événements attendent
dans `LOG_DIR/spool` et repartent seuls, le site n'en dépend jamais. En **dev**,
le transport reste le spool : `dataplatform` mène à la plateforme **de
production**, dont la base d'audit est en ajout seul — ne jamais y envoyer
d'événement de test (un broker jetable pour les essais). Chaque pricing émet un
`pricing.valuation` (`api/app/valuation.py`) que `POST /api/admin/replay/{id}`
sait rejouer. Tout nouveau log ou événement métier passe par `api/app/audit/`
(`emit()`, `record_fallback()`), toute nouvelle métrique par
`api/app/telemetry.py` — jamais un ticker, un utilisateur ou une IP comme
étiquette. Les logs applicatifs vont sur stdout et dans `LOG_DIR/api.jsonl`
(`api/app/logging_utils.py`), avec le `trace_id`. Exploitation :
`deploy/RUNBOOK.md` §9.

## Données de marché

Les données de marché viennent de **la base Postgres alimentée par
`~/data-ingest`** (cours, chaînes d'options, rendements de dividende, courbes
FRED). **Tout nouveau chemin de données passe par la base, sans repli vers Yahoo
Finance** : une donnée absente ou trop ancienne y est une erreur explicite (les
chemins de pricing des scripts passent par `api/app/market_snapshot.py`, qui
n'importe pas `yfinance`). Les replis en direct **déjà en place** dans les
anciens endpoints (`vol_surface.py`, `routers/local_vol_pricing.py`,
`routers/simulation.py`) sont **conservés à dessein**, pour ne pas casser l'API
tant que `data-ingest` n'est pas fiable : ne pas les étendre, ne pas les
retirer sans décision du mainteneur. Le dossier `notebooks/` est un bac à sable
personnel, hors périmètre. Plus de BigQuery — le projet est entièrement hors cloud.

## Assistant de scripting

**Assistant de scripting** (`api/app/assistant/`, route `POST /api/assistant/scripting/chat`,
chat de la page `/scripting`) : il parle au `llama-server` de `~/AgenticEnv`, dont
l'URL est `QM_LLM_BASE_URL` (défaut `http://127.0.0.1:8000/v1` hors Docker ;
`http://172.17.0.1:8001/v1` dans les fichiers compose, via le socket
`llama-bridge` d'AgenticEnv). Connexion (JWT) obligatoire : la prod est publique
et la route consomme le GPU. Tout script écrit par le modèle est vérifié par le
vrai parseur avant d'être proposé. Le prompt décrit le langage : **quand le
langage change (nouvelle fonction, nouvelle syntaxe), mettre à jour
`assistant/prompt.py`** — `pytest` valide ses exemples contre le
parseur et échoue s'ils ne passent plus.

## Commandes

| | |
|---|---|
| `cmake --preset default && cmake --build build` | build C++ (Ninja, Release, tests ON) |
| `ctest --test-dir build --output-on-failure -j16` | tests C++ — ou `scripts/make.sh` qui enchaîne les trois. Chaque `TEST()` tourne dans son propre process (`gtest_discover_tests`), donc `-j` parallélise sans partage d'état ; 16 mesuré à ~1,7 Go au-dessus de ce qui tourne déjà sur la machine (`NPROC=n scripts/make.sh` pour changer) |
| `cmake --preset asan` puis `ctest --test-dir build-asan -j8` | sanitizers (address + UB) — ou `scripts/make_asan.sh`. ASan triple grosso modo la mémoire par process ; 8 mesuré à ~8,4 Go au-dessus de la ligne de base, une part plus significative de la marge qu'avec le preset par défaut (`NPROC=n scripts/make_asan.sh` pour changer) |
| `cmake --preset release` | `-march=native` + benchmarks google-benchmark |
| `cmake --preset cuda && cmake --build build-cuda` | backend CUDA (WP 19, les deux V100) dans `build-cuda/` ; nvcc prend `g++-13` comme hôte (CUDA 12.4 refuse gcc 14). `ctest --test-dir build-cuda -L gpu` pour les seuls tests GPU (la CI n'a pas de GPU et ne les construit pas) ; `build-cuda/qm_gpu_bench` pour le benchmark en temps pour une erreur donnée. Côté Python : `QM_ENABLE_CUDA=1 scripts/run_api.sh` (ou `scripts/dev_install.sh`, `scripts/build_wheel.sh`) pour un module avec GPU ; l'image de prod le fait d'office (`QM_CUDA=ON`) |
| `scripts/build_wheel.sh` | wheel pybind11 dans `dist/` |
| `scripts/run_api.sh` | venv + wheel + `uvicorn --reload` |
| `pytest` | tests Python de l'API (`api/tests/` : assistant de scripting avec le vrai parseur et un faux LLM, snapshot de marché, piste d'audit, producteur Kafka sur un faux broker, replay des valorisations), depuis la racine (`pytest.ini`). Lancés par la CI (job `pytest`, avec le vrai wheel) |
| `docker compose up --build` | stack complète (API + front avec HMR) |
| `cd web && npm run dev` | front seul |
| `cd web && npx tsc --noEmit` | typecheck du front |
| `cd web && npm run lint` | eslint |
| `cd web && npm run build` | build de production Vite |
| `python scripts/gen_openapi.py web/openapi.json && cd web && npm run api:types:local` | régénère `web/openapi.json` et `schema.gen.ts` après un changement de route/schéma API (ADR-005). `gen_openapi.py` stub le module pybind11 — pas besoin d'une API qui tourne. Les deux fichiers sont commités ; le job CI `api-contract` échoue si le diff n'est pas vide |

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
| `blueprint/` | Spécification des lots de travail : la réécriture du front (WP 00–14, fusionnée dans `main`), puis les chantiers du cœur C++ (WP 16 scripting, WP 17 AAD). Dépendances, critères d'acceptation. |
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
- **C++** : `clang-format --style=file`, **version 18** (celle de la CI, job
  `format-check` ; le pre-commit est épinglé sur la même). Un fichier mal
  formaté fait échouer la CI. `clang-format` n'est pas installé sur la machine :
  `uvx --from clang-format==18.1.3 clang-format -i $(git ls-files '*.cpp' '*.hpp')`.
  **Python** : `black`.
- `.pre-commit-config.yaml` (clang-format, black, et `cmake + ctest` en
  pre-push) **n'est pas installé par défaut** : `.git/hooks` est vide tant qu'on
  n'a pas lancé `pre-commit install -t pre-commit -t pre-push`. Sans lui, rien
  n'arrête un push avec des tests rouges hormis la CI : lancer `scripts/make.sh`
  avant de pousser du C++.
- Tout nouvel engine ou instrument arrive avec son test dans `tests/`, et de
  préférence un test de *propriété* (parité call-put, `in + out = vanille`,
  bornes de monotonie, ordre de convergence mesuré en log-log) plutôt qu'une
  simple égalité numérique.
- Le front réécrit est dans `main` ; l'ancien code a disparu (`web/src/` =
  `app/`, `features/`, `shared/`). Règles vérifiées par ESLint et
  `web/tests/discipline.test.ts` : une feature n'importe pas une autre feature,
  `shared/` n'importe ni `features/` ni `app/`, un fichier de `features/` ne
  dépasse pas 230 lignes (extraire un composant plutôt que relever le plafond).

## Déploiement

Le projet est **auto-hébergé**, pas sur Google Cloud — le coût GCP était le
motif de la bascule. La cible : `docker-compose.prod.yml` sur le serveur perso,
exposé en HTTPS par un **tunnel Cloudflare** (aucun port ouvert).

Deux dossiers, un seul dépôt git (`git worktree`) : `~/quant-modeling` est le
**développement** (branches, commits, PRs — c'est là que travaillent les sessions
Claude) ; `~/quant-modeling-prod` est **le site en ligne, toujours sur `main`**,
et ne sert qu'à `./scripts/deploy.sh` (qui fait le `git pull` de `main`, reconstruit
l'image et redémarre). Circuit normal : branche → PR → merge dans `main` → deploy
depuis le dossier prod. Il n'y a plus de branche de déploiement (`web/14-deploy` et
`web/rewrite` ont été supprimées). Ne jamais éditer ni committer depuis
`~/quant-modeling-prod`. Procédure complète dans `deploy/RUNBOOK.md`, conception
dans `blueprint/wp/14-deploy-selfhost.md`. `web/Dockerfile.prod` est le socle
(multi-étage wheel C++ → build Vite → nginx + uvicorn).

Attention au piège classique de Vite : les variables `VITE_*` sont **inlinées
au build**, pas lues au runtime. Changer `VITE_API_BASE` impose de rebuilder
l'image, ou de passer par le mécanisme d'injection runtime décrit dans le lot
de déploiement.
