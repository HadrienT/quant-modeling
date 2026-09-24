# Décisions techniques

Format ADR allégé : le choix, la raison, ce qu'on a écarté et pourquoi. Une
décision n'est pas un dogme — mais la révoquer demande d'écrire ici pourquoi,
pas de la contourner en douce dans un coin du code.

---

## ADR-001 — Tailwind CSS v4 + shadcn/ui

**Décision.** Tailwind v4 (configuration CSS-first via `@theme`) comme moteur de
style, shadcn/ui comme base de composants — c'est-à-dire du code copié dans le
repo, pas une dépendance, s'appuyant sur les primitives Radix pour
l'accessibilité.

**Pourquoi.** Le besoin réel n'est pas « avoir de jolis boutons », c'est
d'obtenir gratuitement les comportements que le front actuel n'a pas et qui
sont longs à écrire correctement : piège de focus dans les dialogues,
navigation clavier des `Select` et `Combobox`, `aria-*` corrects, restitution
du focus, gestion du `Escape`, positionnement des popovers avec détection de
collision. Radix les fournit. Et comme shadcn livre le source dans le repo,
l'apparence reste entièrement modifiable : on n'hérite pas d'un thème.

**Écarté.**
- *CSS Modules + design system maison* : il faudrait réécrire Radix. Le budget
  passerait dans la mécanique des composants au lieu du produit.
- *MUI / Mantine* : rendu reconnaissable et générique, personnalisation qui
  finit en lutte contre la librairie, bundle plus lourd.

---

## ADR-002 — three.js + React Three Fiber pour la 3D

**Décision.** Les surfaces (volatilité implicite, volatilité locale, et plus
tard les profils d'exposition) sont rendues en WebGL avec `three.js` piloté par
`@react-three/fiber`, `@react-three/drei` pour les contrôles et helpers, et
`@react-three/postprocessing` pour le post-traitement.

**Pourquoi.** Demande explicite d'un rendu 3D le plus abouti possible, coût
accepté. Ce que ça débloque et qu'aucune librairie de graphiques ne donne :
colormap et éclairage calculés **en shader** (donc gratuits, quelle que soit la
finesse de la grille), plans de coupe interactifs, arêtes et lignes de niveau
superposées, transitions animées entre deux surfaces, et un `raycast` pour lire
la valeur exacte sous le curseur. Sur des grilles denses c'est aussi le seul
chemin qui reste fluide.

**Écarté.**
- *Plotly `surface`* : c'est l'existant. 5,3 Mo, rendu daté, quasi impossible à
  accorder au design system, interaction limitée à ce que Plotly expose.
- *ECharts GL* : plus léger, honnête, mais on retombe sur « configurer une
  librairie » au lieu de contrôler le rendu — c'est exactement ce que la
  demande écarte.

**Coût assumé.** ~600 Ko de JS pour three.js et son écosystème, chargés
uniquement sur les routes qui affichent une surface (voir
[WP 13](wp/13-perf-budget.md)), et l'obligation d'un chemin de repli quand
WebGL est indisponible.

---

## ADR-003 — Trois librairies de graphiques, trois métiers

**Décision.**

| Usage | Outil |
|---|---|
| Surfaces et volumes 3D | three.js / R3F ([ADR-002](#adr-002--threejs--react-three-fiber-pour-la-3d)) |
| Séries temporelles financières : price tape, courbe d'equity, drawdown | `lightweight-charts` (TradingView) |
| Graphiques analytiques : payoff, profils de greeks, courbes de taux, distributions, smile | `visx` (primitives d3 rendues en React) |

**Pourquoi ne pas en prendre une seule.** Une série de prix et un diagramme de
payoff n'ont rien en commun. `lightweight-charts` dessine sur canvas, encaisse
des dizaines de milliers de points avec pan/zoom/crosshair natifs, et ressemble
à ce que les gens ont sous les yeux toute la journée — le réécrire en SVG
serait absurde. À l'inverse un payoff a peu de points mais beaucoup
d'annotations (breakevens, strikes, zones de profit, legs individuelles) :
là on veut du SVG stylé par les mêmes tokens que le reste, donc `visx`.

**Coût.** Trois APIs à connaître, mais chacune sur un périmètre net, et le
total reste très en dessous de Plotly seul. Toutes trois sont chargées en
`lazy`.

---

## ADR-004 — Greenfield dans `web/`, pas de dossier parallèle

**Décision.** La nouvelle arborescence (`src/app/`, `src/features/`,
`src/shared/`) est créée **dans** `web/`, à côté de l'ancienne, et l'ancienne
est supprimée route par route au fil des lots. Pas de `web-next/`.

**Pourquoi.** Un dossier parallèle imposerait de dupliquer le `docker-compose`,
le Dockerfile, la CI et le proxy, et surtout il autorise la coexistence
indéfinie — c'est le scénario où les deux fronts vivent un an. Ici la
suppression est forcée : chaque lot déclare les fichiers qu'il tue, et le lot
n'est pas terminé s'ils sont encore là.

**Garde-fou.** Le routeur monte les nouvelles routes au fur et à mesure ; tant
qu'une page n'est pas migrée, c'est l'ancien composant qui est monté. À aucun
moment l'application n'est cassée.

---

## ADR-005 — Types générés depuis l'OpenAPI, avec test de dérive

**Décision.** `web/src/shared/api/schema.gen.ts` est généré par
`openapi-typescript` depuis le `/openapi.json` de FastAPI, **commité**, et un
job CI le régénère et échoue si le diff n'est pas vide.

**Pourquoi.** Les types du client actuel sont écrits à la main et personne ne
les confronte jamais aux modèles Pydantic : un champ renommé côté API passe
inaperçu jusqu'au `undefined` en production. Générer résout le problème ;
committer le résultat garde le front buildable sans API démarrée ; le test de
dérive empêche que le fichier généré devienne obsolète en silence.

C'est le même dispositif que le test de dérive de protocole utilisé sur
`agenticenv-chat` — il a fait ses preuves.

**Écarté.** *Génération à la volée au build* : rend le build dépendant d'un
service démarré, casse la CI et les builds hors ligne.

---

## ADR-006 — TanStack Query pour l'état serveur, Zustand pour le reste, l'URL pour la vue

**Décision.** Trois natures d'état, trois emplacements, aucun mélange :

- **État serveur** (prix, surfaces, portefeuilles, backtests) → TanStack Query.
  Cache, déduplication, invalidation, retry, annulation, `stale` visible.
- **État de vue** (produit sélectionné, ticker, paramètres du formulaire,
  onglet actif) → **paramètres d'URL typés**. Une session de pricing devient
  une URL qu'on colle dans un message.
- **État client transverse** (thème, palette de commandes, slots de comparaison)
  → Zustand, un store minuscule.

**Pourquoi.** Le front actuel met tout dans du `useState` de page — d'où
`usePricing.ts` à 654 lignes. La règle qui coupe le problème : *si la donnée
vient du serveur, elle n'a pas à vivre dans un `useState`.*

---

## ADR-007 — TanStack Router

**Décision.** TanStack Router plutôt que React Router.

**Pourquoi.** La conséquence directe de l'ADR-006 : l'état de vue vit dans
l'URL, donc la validation et le typage des *search params* deviennent
structurants. TanStack Router les valide par schéma (zod) et les type de bout
en bout, avec préchargement des données de route. Avec React Router il faudrait
écrire cette couche à la main sur chaque page.

**Écarté.** *React Router v7* : plus répandu, parfaitement viable, mais les
search params y restent des chaînes non typées. Si la migration s'avère
coûteuse, c'est le repli — l'architecture des lots n'en dépend pas.

---

## ADR-008 — La clé d'API sort du navigateur

**Décision.** Suppression de `VITE_API_KEY`. Le navigateur n'authentifie plus
que par le JWT utilisateur ; c'est le reverse proxy qui porte la clé d'API
serveur-à-serveur si on la conserve.

**Pourquoi.** Voir [README §3](README.md#3-le-point-de-sécurité-à-traiter-avant-tout-le-reste) :
telle quelle, la clé est inlinée dans le bundle par Vite (donc publique) et
absente en production (donc inopérante). Un secret servi à tous les visiteurs
n'est pas un secret.

**Conséquence côté API.** Il faut trancher, route par route, entre « publique »
et « authentifiée par JWT ». Le mélange actuel est incohérent : `/market/*` et
`/api/backtest/run` exigent la clé, `/price/*` n'exige rien.

---

## ADR-009 — Le sombre est le thème par défaut

**Décision.** Thème sombre par défaut, thème clair complet et maintenu, respect
de `prefers-color-scheme` au premier chargement, choix explicite persisté
ensuite.

**Pourquoi.** L'outil est un poste de travail regardé longtemps, les surfaces
3D et les colormaps rendent nettement mieux sur fond sombre, et c'est
l'attendu du domaine. Le thème clair reste obligatoire : il sert aux captures
d'écran, à l'impression et aux environnements lumineux.

**Rupture assumée.** La palette actuelle (crème, orange, dégradés radiaux) est
abandonnée. Elle est chaleureuse mais elle ne dit pas « outil financier », et
elle rend la lecture de tableaux denses difficile.

---

## ADR-010 — `exactOptionalPropertyTypes` retiré des cinq drapeaux stricts

**Décision.** Le `tsconfig.json` du front active quatre des cinq drapeaux
demandés par le [WP 00](wp/00-foundations.md) : `noUncheckedIndexedAccess`,
`noImplicitOverride`, `noFallthroughCasesInSwitch`, `verbatimModuleSyntax`.
`exactOptionalPropertyTypes` est **retiré**.

**Pourquoi.** Le drapeau ne distingue `{ a?: T }` de `{ a: T | undefined }`.
En React, chaque prop optionnelle à laquelle on passe une valeur qui peut être
`undefined` (`table={maybeUndefined}`, `error={query.error}`) devient une
erreur, ce qui force soit un `| undefined` explicite sur chaque prop de chaque
composant, soit une construction conditionnelle de props. Le coût est réparti
sur tout le code de composant pour un gain de détection de bug quasi nul dans ce
projet. Le drapeau qui compte vraiment ici — celui qui attrape les accès aux
grilles à trous — est `noUncheckedIndexedAccess`, conservé.

**Écarté.** *Garder le drapeau et suffixer `| undefined` partout* : bruit de
lecture permanent sur des centaines de props pour rien.

---

## ADR-011 — Le producteur d'audit branché sur `quant-platform` (WP 18b, 18d, 18e)

Les arbitrages de fond du WP 18 (Kafka malgré le volume, deux dépôts, base
d'audit dédiée, deux chemins parallèles, perte bornée côté producteur, JSON
Schema avant Avro, un seul broker, IP hachée à l'émission) sont ceux de son
[§11](wp/18-observability.md#11-décisions), D1 à D9 ; côté plateforme, le
contrat est canonique dans `~/quant-platform/docs/contract.md`. Cet ADR ne
consigne que ce que l'implémentation côté API a tranché.

**Décisions.**

1. **Un replay a quatre issues, pas trois.** Le WP en prévoit trois
   (`reproduced`, `drifted_inputs`, `drifted_code`). Mêmes données, même code,
   prix différent n'entre dans aucune : c'est un pricing non déterministe à
   graine fixée, « un bug à corriger, pas une tolérance à élargir » (WP 18e).
   Le ranger dans l'une des trois cacherait précisément ce bug ; il a donc sa
   propre issue, `not_reproduced`. La tolérance est relative 1e-12 : un replay
   refait le même calcul, tout écart plus grand est réel.
2. **`drifted_code` compare le couple (SHA de l'API, SHA du wheel)**, pas le
   seul wheel : un changement de `pricing_service.py` (l'aiguillage d'un produit
   vers un engine) change un prix autant qu'un changement du C++.
3. **Le modèle et l'engine enregistrés sont ceux qui ont tourné**, déduits par
   le registre `valuation.PRODUCTS` à l'image de l'aiguillage de
   `pricing_service.py`, pas ceux que la requête demandait : une américaine
   demandée en `mc` est pricée sur un arbre binomial, et l'enregistrement le
   dit. Le routeur et le replay passent par ce même registre.
4. **Les entrées saisies par l'utilisateur ne sont pas des `market_inputs`.**
   Une valorisation manuelle (spot, vol tapés) a une liste vide ; ses entrées
   sont dans `request`, verbatim. Seules les données lues en base (le local-vol
   du scripting : spot, dividende, chaîne d'options) y figurent, avec date,
   source, statut et empreinte.
5. **En développement, le transport reste le spool.** Le réseau `dataplatform`
   mène à la plateforme **de production**, dont la base d'audit est
   append-only : un événement de test y resterait pour toujours. La prod envoie
   à Kafka par défaut ; le dev seulement si on le pointe explicitement vers un
   broker jetable.
6. **Une panne de Kafka produit deux lignes de log, pas une par événement** :
   l'entrée en panne et le rétablissement (avec le nombre d'événements passés
   par le spool). Sinon une panne pendant du trafic noierait Loki.

**Écarté.** *Élargir la tolérance du replay pour absorber le non-déterminisme*
(voir 1) ; *recalculer les `market_inputs` depuis la requête* (une donnée
révisée en base serait invisible) ; *le transport Kafka par défaut en dev*
(voir 5).
