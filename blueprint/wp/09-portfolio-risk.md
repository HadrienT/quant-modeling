# WP 09 — Portefeuille & risque

| | |
|---|---|
| **Dépend de** | [03](03-app-shell.md), [02](02-api-layer.md), [04](04-auth-session.md), [07](07-pricing-workbench.md), [06](06-viz-2d.md) |
| **Bloque** | [15](15-future-quant-surfaces.md) (l'agrégation de risque y est réutilisée) |
| **Branche** | `web/09-portfolio-risk` |

## Objectif

Passer d'un pricing unitaire à un portefeuille : positions, agrégats de risque,
stress, VaR. C'est l'écran qui fait passer le projet de « calculateur » à
« outil de desk ».

## Routes de l'API concernées

CRUD portefeuille et positions, plus `POST .../price` (pricing par lot),
`POST .../stress` et `POST .../var`.

## 1. Table de positions

`TanStack Table` + `TanStack Virtual` :

- Tri, filtrage, groupement par catégorie ou sous-jacent, épinglage de la
  colonne d'instrument, redimensionnement, choix des colonnes persisté.
- Virtualisation : la table doit tenir mille lignes sans ralentir.
- Ligne de totaux **épinglée en bas**, toujours visible.
- Toutes les valeurs numériques via `NumberCell` (chasse fixe tabulaire) ;
  P&L via `DeltaBadge` avec signe explicite
  ([lot 01 §3](01-design-system.md#3-le-cas-du-pl--la-seule-règle-vraiment-spécifique-au-domaine)).
- Sélection multiple → actions en lot (repricer, supprimer, dupliquer,
  inverser le sens).
- Colonnes : instrument, sens, quantité, prix d'entrée, prix courant, NPV, P&L,
  les cinq greeks, moteur, fraîcheur.

## 2. Ajout de position

Réutilise **le catalogue et les formulaires du [lot 07](07-pricing-workbench.md)**,
sans les redéfinir. Aujourd'hui `portfolio/catalog.ts` (428 lignes) duplique
`pricing/productRegistry.ts` avec des libellés qui divergent déjà — c'est
exactement ce que le catalogue partagé supprime
([interdépendances §3.4](../dependencies.md#34-le-descripteur-de-produit--featurespricingcatalog--wp-07)).

Chemin supplémentaire à ouvrir : *envoyer au portefeuille* depuis l'atelier de
pricing, qui transporte la configuration courante.

## 3. Pricing par lot

Le point technique délicat du lot. `POST /api/portfolios/{id}/price` price tout
en synchrone : sur cinquante positions Monte-Carlo, la requête dépassera le
timeout du proxy avant d'aboutir.

Côté front : progression par position, résultats affichés au fil de l'eau,
annulation possible, et une position en échec n'invalide pas les autres.
Côté serveur : voir [WP 02 §6](02-api-layer.md#6-décisions-serveur-qui-bloquent-ce-lot) —
il faut au minimum une réponse incrémentale.

## 4. Risque agrégé

- Rangée `MetricRow` : NPV total, P&L, delta, gamma, vega, theta, rho,
  positions valorisées sur total. `positions_priced / positions_total` est
  important : un agrégat calculé sur une partie du portefeuille doit le dire.
- **Attribution** : quelle position porte quelle part de chaque greek. Barres
  horizontales triées, pas de camembert.
- Profils de greeks du portefeuille en fonction du spot, en petits multiples.
- Concentration par sous-jacent et par catégorie.

## 5. Stress et VaR

- **Matrice de stress** spot × vol : heatmap divergente bleu ↔ rouge, zéro gris.
  Bumps prédéfinis (±5 %, ±10 %, ±20 % de spot ; ±5, ±10 points de vol ; ±50,
  ±100 pb de taux) plus des bumps personnalisés.
- Clic sur une cellule → décomposition par position.
- **VaR / ES** : niveau de confiance et horizon paramétrables, méthode affichée
  (le champ `method` de la réponse), histogramme de P&L avec VaR et ES
  annotées en lignes verticales.
- **Toujours afficher la méthode et l'horizon à côté du chiffre.** Une VaR sans
  son quantile et son horizon ne veut rien dire, et c'est la première question
  qu'on posera.

## 6. Persistance

- Sélecteur de portefeuille, renommage, duplication, suppression avec
  confirmation.
- Mode anonyme via `PortfolioRepository` local ([lot 04](04-auth-session.md)),
  avec migration à la connexion.
- Import / export **JSON** (aller-retour fidèle) et **CSV** (lecture par un
  humain ou un tableur). L'export est la fonctionnalité la plus demandée d'un
  outil de risque et la plus vite oubliée.

## Fichiers supprimés

`web/src/pages/Portfolio.tsx` (394 lignes), `portfolio/catalog.ts` (428),
`portfolio/AddPositionForm.tsx`, `portfolio/PositionTable.tsx`,
`portfolio/PositionDetail.tsx` (351), `portfolio/RiskPanel.tsx`,
`components/PortfolioBuilder.tsx`.

## Critères d'acceptation

- [ ] Mille positions restent fluides au tri et au défilement.
- [ ] Le pricing d'un portefeuille affiche une progression et reste annulable.
- [ ] Une position en échec n'empêche pas les autres d'afficher leur résultat.
- [ ] Les agrégats indiquent combien de positions ils couvrent réellement.
- [ ] La VaR affiche toujours méthode, confiance et horizon.
- [ ] Export JSON puis import redonne un portefeuille identique (test
      d'aller-retour).
- [ ] Le mode anonyme est pleinement fonctionnel sans compte.
- [ ] Aucun fichier de catalogue produit propre à cette feature.

## Pièges

- **Sommer des greeks de sous-jacents différents n'a pas de sens.** Un delta
  total agrégé sur AAPL et sur le pétrole est un nombre sans signification.
  Agréger par sous-jacent, et ne présenter un total global qu'après
  normalisation explicite — ou pas du tout.
- Les greeks Monte-Carlo ont une erreur standard : elle se propage à l'agrégat
  et doit rester visible.
- Un portefeuille dont les positions ont été valorisées à des instants
  différents n'est pas cohérent : afficher l'horodatage le plus ancien, et
  avertir au-delà d'un seuil.
