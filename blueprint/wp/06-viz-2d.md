# WP 06 — Visualisation 2D

| | |
|---|---|
| **Dépend de** | [01](01-design-system.md) |
| **Bloque** | 08, 09, 10, 11 |
| **Branche** | `web/06-viz-2d` |

## Objectif

Deux moteurs, deux métiers, une seule identité visuelle
([ADR-003](../decisions.md#adr-003--trois-librairies-de-graphiques-trois-métiers)) :

| Moteur | Pour quoi | Pourquoi celui-là |
|---|---|---|
| `lightweight-charts` | Séries temporelles : price tape, courbe d'equity, drawdown | Canvas, dizaines de milliers de points, pan/zoom/crosshair natifs, c'est le rendu que les gens du métier ont sous les yeux |
| `visx` | Analytique : payoff, profils de greeks, courbes de taux, smile, distribution de P&L, heatmaps | SVG, peu de points mais beaucoup d'annotations, entièrement stylable par nos tokens |

Les deux lisent les mêmes tokens que la 3D. Un bleu de série est le même bleu
partout, quelle que soit la librairie qui l'a dessiné.

## 1. Règles qui s'appliquent à tout graphique

Elles sont non négociables et vérifiées en revue :

- **Un seul axe des ordonnées.** Jamais deux échelles verticales. Deux
  grandeurs d'ordres différents → deux graphiques, ou une base 100 commune. Le
  cas qui va se présenter : « prix du sous-jacent et P&L sur le même
  graphique ». La réponse est deux panneaux empilés à axe des temps partagé.
- **Couche de survol par défaut.** Crosshair + infobulle sur les lignes et
  aires, infobulle par marque sur les barres et les points. Un graphique HTML
  est interactif ; s'en priver est un choix, et ce n'est pas le nôtre.
- **Légende dès deux séries**, plus étiquetage direct jusqu'à quatre. L'identité
  ne repose jamais sur la couleur seule — c'est aussi ce qui rend utilisable en
  thème clair les trois séries sous 3:1 signalées au
  [lot 01 §2](01-design-system.md#2-palette-de-séries--validée-pas-choisie).
- **Vue tableau** accessible depuis chaque graphique. C'est le chemin
  d'accessibilité, et accessoirement celui qu'on utilise pour copier des
  chiffres.
- Marques fines, grille et axes en retrait, aucune valeur écrite sur chaque
  point. Le texte porte les tokens d'encre, jamais la couleur de la série.
- **Zéro visible** quand le signe a un sens (P&L, greeks). Un axe tronqué sur un
  graphique de P&L exagère visuellement l'écart.

## 2. Catalogue de graphiques

| Composant | Moteur | Consommé par | Notes |
|---|---|---|---|
| `PriceSeriesChart` | lightweight | 08, 11 | Chandelier ou ligne, volume en panneau lié |
| `EquityCurveChart` | lightweight | 11 | Portefeuille vs S&P 500 ; **indexé base 100**, sinon deux échelles |
| `DrawdownChart` | lightweight | 11 | Aire sous zéro, axe partagé avec la courbe d'equity |
| `PayoffChart` | visx | 07, 10 | Payoff à maturité **et** valeur à *t*, breakevens annotés, legs individuelles en trait fin |
| `GreekProfileChart` | visx | 07, 10 | Un greek en fonction du spot ; petits multiples pour en voir cinq, jamais cinq axes |
| `RatesCurveChart` | visx | 08 | Zéro-coupon vs forward, marqueurs sur les points d'instrument |
| `SmileChart` | visx | 08 | Coupe de la surface 3D ; **doit partager échelles et couleurs avec le [lot 05](05-viz-3d.md)** |
| `SurfaceHeatmap` | visx | 05 (repli), 08 | Rampe séquentielle mono-teinte, trous rendus comme trous |
| `StressMatrix` | visx | 09 | Spot × vol, rampe **divergente** bleu ↔ rouge, zéro gris |
| `DistributionChart` | visx | 09, 11 | Histogramme de P&L, VaR et ES annotées en ligne verticale |
| `ConvergenceChart` | visx | 07, 15 | Erreur MC en fonction du nombre de chemins, **échelles log-log**, pente de référence en 1/√N tracée |
| `AllocationChart` | visx | 11 | Barres horizontales triées. Pas de camembert |

Le `ConvergenceChart` mérite une mention : c'est le graphique qui montre qu'on
sait ce qu'on fait. Tracer l'erreur mesurée contre la pente théorique 1/√N, et
voir le gain du QMC s'en écarter, vaut plus qu'un tableau de prix.

## 3. Infrastructure partagée

`shared/viz/` contient ce que les deux moteurs utilisent :

- Échelles, ticks « ronds », domaines avec marge (`nice`).
- Formateurs par grandeur : prix, pourcentage, points de vol, temps en années
  vs dates, notation compacte pour les notionnels.
- `useChartTheme()` — lit les tokens, réagit au changement de thème.
- `ChartFrame` : titre, sous-titre, légende, actions (export PNG, export CSV,
  bascule vue tableau), zone de tracé responsive, états chargement / vide /
  erreur. **Tout graphique est enveloppé dedans**, ce qui garantit que ces
  états existent partout sans avoir à y penser.
- `useCrosshairSync()` : plusieurs graphiques à axe des temps partagé bougent
  ensemble (equity + drawdown, spot + P&L).

## 4. Fichiers supprimés

- [`web/src/components/PnlChart.tsx`](../../web/src/components/PnlChart.tsx) (352 lignes)
- [`web/src/components/ChartHoverCard.tsx`](../../web/src/components/ChartHoverCard.tsx)
- [`web/src/utils/chartUtils.ts`](../../web/src/utils/chartUtils.ts)
- `plotly.js`, `react-plotly.js`, `@types/plotly.js`, `@types/react-plotly.js`
  du `package.json` — **à faire dans ce lot**, c'est là qu'on récupère les
  5,3 Mo.

## Critères d'acceptation

- [ ] Chaque composant a une story dans les deux thèmes, avec les états
      chargement, vide et erreur.
- [ ] Aucune couleur en dur dans un composant de graphique.
- [ ] Aucun graphique à double axe des ordonnées (vérifié en revue).
- [ ] Chaque graphique a une vue tableau atteignable au clavier.
- [ ] `plotly` a disparu de `package.json` et de `package-lock.json`.
- [ ] Le bundle de graphiques 2D reste sous le budget du
      [lot 13](13-perf-budget.md).
- [ ] Une série de 10 000 points reste fluide au pan et au zoom (mesuré).

## Pièges

- `lightweight-charts` a son propre système de thème : il faut lui **pousser**
  nos tokens à chaque changement de thème, il ne lit pas le CSS.
- `visx` est une boîte à outils, pas une librairie de graphiques : sans
  discipline, chaque composant réinvente ses axes. D'où `ChartFrame` et les
  échelles partagées, à écrire **avant** le premier graphique, pas après le
  troisième.
- Ne pas recolorer par le rang au filtrage : la couleur suit la position, pas sa
  place dans la liste.
- Les petits multiples battent presque toujours l'empilement de séries. Cinq
  greeks : cinq petits graphiques alignés, pas un graphique à cinq courbes.
