# WP 08 — Données de marché

| | |
|---|---|
| **Dépend de** | [03](03-app-shell.md), [02](02-api-layer.md), [05](05-viz-3d.md), [06](06-viz-2d.md) |
| **Bloque** | rien |
| **Branche** | `web/08-market-data` |

## Objectif

La page vitrine. C'est ici que la surface 3D vit, et c'est la capture d'écran
que le README montrera. C'est aussi la page où le sujet le plus difficile du
projet — la qualité de la donnée — doit devenir visible au lieu d'être caché.

## Routes de l'API concernées

| Route | Contenu |
|---|---|
| `GET /market/tickers` | Univers disponible |
| `GET /market/prices/history` | Historique de prix, par plage |
| `GET /market/iv/surface` | Surface de vol implicite **brute** (`mid` / `bid` / `ask`) |
| `GET /api/local-vol/iv-surface` | Surface **nettoyée**, avec `n_clean_quotes` et `cleaning_summary` |
| `GET /api/local-vol/surface` | Surface de **volatilité locale** (Dupire) |
| `GET /market/rates/curve` | Courbe zéro ou forward (Treasury / SOFR / FedFunds) |

## 1. Onglet Prix

- Sélecteur de ticker en `Combobox` avec recherche — la liste actuelle est un
  `<select>` sur l'univers entier.
- `PriceSeriesChart` ([lot 06](06-viz-2d.md)) : plages 1M / 3M / 1A / 5A / max,
  crosshair, volume dans un panneau lié.
- Tuiles `Metric` : dernier prix, variation, plus haut et plus bas de la
  période, volatilité réalisée sur 20 et 60 jours. La volatilité réalisée est
  le pont naturel vers l'onglet suivant : on compare l'implicite à ce qui s'est
  réellement produit.
- `Freshness` sur la dernière date de donnée. Une price tape muette depuis
  trois jours doit se voir.

## 2. Onglet Volatilité — la pièce maîtresse

Trois surfaces sur la même page, avec une navigation qui raconte le pipeline :

```
quotes brutes  →  nettoyage  →  surface implicite lissée  →  Dupire  →  vol locale
```

- **Rendu 3D** ([lot 05](05-viz-3d.md)), avec les trous affichés comme trous.
  Sur la surface brute, ces trous *sont l'information* : ils montrent où le
  marché ne cote pas.
- **Comparaison** brute vs nettoyée, et implicite vs locale, en **surface de
  différence** (rampe divergente, zéro gris). La différence entre vol implicite
  et vol locale est un objet financier réel, pas une curiosité graphique.
- **Coupes** : figer une maturité donne le smile, figer un strike donne la
  structure par terme, tracés en 2D à côté par le [lot 06](06-viz-2d.md).
- **Panneau de qualité de données** — la partie que la plupart des projets
  évitent, et donc celle qui distingue :
  - `n_clean_quotes` et `cleaning_summary` de l'API affichés en clair, pas
    enfouis.
  - Nombre de quotes rejetées et pourquoi.
  - Couverture de la grille : quel pourcentage de nœuds a une vraie cotation.
  - **Contrôles de non-arbitrage** : variance totale croissante en maturité
    (calendrier), convexité en strike (papillon). Une violation s'affiche en
    statut `warning` sur le nœud concerné. Ce sont des tests gratuits et très
    discriminants ; les rendre visibles montre qu'on sait ce qu'on regarde.

## 3. Onglet Taux

- Courbes zéro et forward superposées, marqueurs sur les points d'instrument.
- Sélecteur Treasury / SOFR / FedFunds, et `fixed_period_years` pour le
  forward.
- Comparaison de deux courbes et **spread** en panneau lié — jamais en second
  axe des ordonnées.

## 4. Comportement de cache

Les surfaces sont coûteuses côté serveur (nettoyage, calibration, Dupire). Le
`staleTime` est long et **l'ancienneté est affichée**. Un bouton *Recalculer*
explicite ; pas de rechargement automatique au retour sur l'onglet.

## Fichiers supprimés

`web/src/pages/Market.tsx` (373 lignes), `market/PriceTab.tsx`,
`market/VolTab.tsx`, `market/RatesTab.tsx` (394 lignes), `market/types.ts`.

## Critères d'acceptation

- [ ] Les trois surfaces se rendent en 3D, avec repli 2D si WebGL manque.
- [ ] Le panneau de qualité affiche le résumé de nettoyage de l'API.
- [ ] Les violations d'arbitrage calendrier et papillon sont détectées et
      localisées sur la grille.
- [ ] Une surface avec beaucoup de trous ne les interpole pas.
- [ ] Coupes et surface 3D partagent la même échelle de couleur et les mêmes
      valeurs (vérifié par test sur une grille synthétique).
- [ ] L'ancienneté de chaque donnée est visible sans survol.

## Pièges

- Ne pas afficher une surface nettoyée sans dire qu'elle a été nettoyée. La
  bascule brut/nettoyé doit être permanente et évidente.
- Les tickers illiquides donnent des surfaces quasi vides : c'est un cas
  normal, il lui faut un état dédié — pas une erreur, pas un graphique vide.
- Les contrôles d'arbitrage se font sur la **variance totale** ($w = \sigma^2 T$),
  pas sur la volatilité : c'est l'erreur classique et elle produit de fausses
  alertes.
