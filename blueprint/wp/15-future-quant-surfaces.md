# WP 15 — Surfaces quant à venir

| | |
|---|---|
| **Dépend de** | [05](05-viz-3d.md), [09](09-portfolio-risk.md), et de la [roadmap quant](../../etc/roadmap.md) |
| **Bloque** | rien |
| **Branche** | une par écran, quand le sujet C++ correspondant existe |

## Objectif

Ce lot ne se construit **pas maintenant**. Il existe pour une seule raison :
garantir que l'architecture posée par les lots 00 à 14 accueillera ces écrans
sans redécoupage. Chaque section dit ce qu'il faudra afficher et sur quel
mécanisme existant il s'appuiera.

La règle qui le gouverne : **on ne construit pas l'écran avant que le calcul
existe.** Une page de calibration sans calibrateur est une maquette, et une
maquette dans un dépôt finit par se faire prendre pour une fonctionnalité.

## 1. Rapport de calibration — *chantiers 0 et 1 de la roadmap*

Quand le framework de calibration et Heston / SABR existeront.

| À afficher | Mécanisme réutilisé |
|---|---|
| Paramètres calibrés, avec bornes et statut de convergence | `Metric`, `NumberCell` |
| **RMSE en points de volatilité implicite, pas en prix** | `Uncertainty` |
| Marché vs modèle, par tranche de maturité, surimposés | `SmileChart` ([06](06-viz-2d.md)) |
| Carte des résidus sur la grille strike × maturité | Rampe **divergente**, zéro gris — un résidu est signé |
| Pire point, nombre d'itérations, durée, graine | Tuiles |
| Condition de Feller ($2\kappa\theta > \xi^2$) satisfaite ou non | Statut, avec la nuance qu'elle est rarement respectée en pratique |

Le résidu se lit en **points de vol implicite** parce que c'est l'unité dans
laquelle un trader raisonne, et parce qu'un résidu en prix écrase les ailes.

## 2. AAD vs bump-and-reprice — *chantier 2*

L'écran qui porte le résultat central du sujet : *toutes les sensibilités pour
quelques fois le coût d'un prix, indépendamment de leur nombre.*

- Tableau comparatif sur un panier à cinquante sous-jacents : coût relatif,
  précision, nombre de paramètres.
- **Courbe coût en fonction du nombre de paramètres** : bump linéaire, adjoint
  plat. C'est *le* graphique du sujet ; il tient en un panneau et il se
  comprend en trois secondes.
- Écart entre greeks adjoints et greeks par différences finies, avec les barres
  d'erreur Monte-Carlo des seconds — c'est en général l'AAD qui a raison, et le
  graphique doit le montrer sans le prétendre.

## 3. Banc GPU — *chantier 3*

- CPU mono-fil / CPU multi-fils / 1 × V100 / 2 × V100.
- **Métrique principale : temps pour atteindre une erreur cible**, pas chemins
  par seconde. La roadmap est explicite là-dessus, et l'écran doit imposer la
  bonne métrique plutôt que la plus flatteuse.
- Reproductibilité bit-à-bit affichée comme un contrôle passant ou échouant,
  indépendamment du nombre de GPU.
- Matériel exact et versions affichés à côté des chiffres, sans quoi le
  benchmark n'est pas citable.

## 4. Profils d'exposition xVA — *chantier 4*

Le capstone, et l'écran le plus payant du lot.

- **EE, EPE, ENE, PFE 95 % et 99 % en fonction du temps** — enveloppe de
  quantiles, `visx`.
- **La surface exposition × temps × chemin en 3D** : c'est exactement le
  `SurfaceGrid` du [lot 05](05-viz-3d.md), sans une ligne de moteur à écrire.
  C'est la raison pour laquelle le moteur 3D est spécifié comme générique dès
  le départ.
- Décomposition CVA / DVA / FVA / MVA par netting set.
- Effet du collatéral : seuil, MTA, période de marge, avec et sans, superposés.
- Sensibilités du CVA par mode adjoint — le point de convergence de tout le
  reste.

Réutilise l'agrégation de risque et les netting sets du
[lot 09](09-portfolio-risk.md).

## 5. Tableau de bord de validation

Transversal, et utile bien avant les autres :

- Écarts contre **QuantLib** sur tout ce qui existe des deux côtés.
- Reproduction des valeurs de référence publiées (Albrecher, Andersen,
  Longstaff-Schwartz).
- **Ordres de convergence mesurés** : Euler faible 1 et fort 1/2, Milstein
  fort 1, Crank-Nicolson en $O(\Delta t^2)$ — pente en log-log, pas une
  égalité numérique. Réutilise le `ConvergenceChart` du
  [lot 06](06-viz-2d.md).
- Parités et bornes de non-arbitrage, en statut vert/rouge.

Alimenté par la sortie de la CI C++, pas recalculé dans le navigateur.

## Ce que les autres lots doivent prévoir dès maintenant

Aucune de ces contraintes ne coûte quoi que ce soit si elle est respectée dès le
départ, et toutes coûtent cher à rattraper :

1. Le moteur de surface est **générique** — grille abstraite, pas « surface de
   vol » ([lot 05](05-viz-3d.md)).
2. Le `ConvergenceChart` en log-log existe dès le [lot 06](06-viz-2d.md).
3. La rampe divergente est définie dès le [lot 01](01-design-system.md) : les
   résidus et les expositions signées en dépendent.
4. Les tuiles de métriques acceptent une **unité** et une **incertitude** dès le
   [lot 01](01-design-system.md) : un RMSE en points de vol n'est pas un
   pourcentage.
5. Le routeur accepte l'ajout d'une section sans réorganiser la navigation
   ([lot 03](03-app-shell.md)).
