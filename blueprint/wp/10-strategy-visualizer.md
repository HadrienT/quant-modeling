# WP 10 — Visualiseur de stratégies

| | |
|---|---|
| **Dépend de** | [03](03-app-shell.md), [06](06-viz-2d.md), [07](07-pricing-workbench.md) |
| **Bloque** | rien |
| **Branche** | `web/10-strategies` |

## Objectif

La page d'accueil (`/`) : construire une stratégie multi-legs et voir
immédiatement son payoff, sa valeur avant maturité et ses greeks. C'est la
première chose qu'un visiteur voit — elle doit être compréhensible en cinq
secondes et rester utile ensuite.

## 1. Moteur de payoff — dans `shared/payoff/`

Fonctions **pures**, sans React, testées unitairement, réutilisées par le
[lot 07](07-pricing-workbench.md) et par cette page
([interdépendances §5](../dependencies.md#5-le-sens-de-dépendance-à-ne-jamais-inverser)).

Ce que le moteur calcule :

- Payoff à maturité de la combinaison de legs.
- **Valeur à un instant $t$ intermédiaire** — la courbe qui apprend vraiment
  quelque chose, parce qu'elle montre l'effet du temps et de la volatilité que
  le payoff à maturité masque.
- Greeks de la position agrégée en fonction du spot.
- Points remarquables : **breakevens** (résolus numériquement, pas approximés
  sur la grille de tracé), gain maximum, perte maximum, et le cas non borné
  traité explicitement plutôt que par un grand nombre.

## 2. Constructeur de legs

- Ajout d'une leg par le catalogue produit du [lot 07](07-pricing-workbench.md) :
  call, put, sous-jacent, forward — pas un type de leg réinventé ici.
- Par leg : sens, quantité, strike, maturité, prime, avec une carte compacte et
  réordonnable.
- **Préréglages** : straddle, strangle, call/put spread, butterfly, condor,
  collar, risk reversal, calendar spread, covered call, protective put. Chacun
  paramétré depuis le spot courant, pas depuis des constantes.
- Charger un ticker réel remplit spot, taux et volatilité implicite depuis les
  données du [lot 08](08-market-data.md) — la passerelle entre la page
  pédagogique et les vraies données.

## 3. Graphique

`PayoffChart` du [lot 06](06-viz-2d.md) :

- Payoff à maturité en trait plein, valeur à $t$ en trait plus fin, legs
  individuelles en trait très fin optionnel.
- **Zéro toujours visible** ; zones de profit et de perte en aplat très léger
  (bleu/rouge divergent, jamais rouge/vert).
- Breakevens, strikes et spot courant annotés directement sur le graphique.
- **Édition des strikes par glissement sur le graphique**, avec saisie
  numérique équivalente à côté. Le glissement est agréable ; il ne doit jamais
  être le seul moyen — c'est une exigence d'accessibilité, pas de confort.
- Petits multiples des greeks sous le payoff, axe des abscisses partagé et
  crosshair synchronisé.

## 4. Métriques

`MetricRow` : coût net (débit ou crédit), gain maximum, perte maximum,
breakevens, ratio risque/rendement, probabilité de profit sous la mesure
risque-neutre — avec la mention explicite qu'il s'agit d'une probabilité
risque-neutre et non réelle, sans quoi le chiffre est trompeur.

## 5. Partage

Toute la stratégie tient dans l'URL ([lot 03](03-app-shell.md)) : construire un
condor et envoyer le lien.

## Fichiers supprimés

`web/src/pages/Visualize.tsx` (221 lignes), `visualize/LegCard.tsx`,
`visualize/MetricsPanel.tsx`, `visualize/payoff.ts`, `visualize/strategies.ts`,
`visualize/types.ts`.

## Critères d'acceptation

- [ ] Les breakevens sont exacts sur des cas à solution analytique connue
      (straddle, call spread), vérifiés par test unitaire.
- [ ] Un payoff non borné (straddle vendu) affiche « non borné », pas un grand
      nombre.
- [ ] Les strikes sont modifiables au clavier autant qu'à la souris.
- [ ] Charger un ticker réel remplit spot, taux et volatilité.
- [ ] La valeur à $t$ coïncide avec le payoff à maturité quand $t \to T$
      (test de cohérence).
- [ ] L'URL restitue la stratégie complète.

## Pièges

- Chercher les breakevens en balayant la grille de tracé donne un résultat qui
  dépend de la résolution du graphique. Les résoudre numériquement.
- Ne pas dupliquer les formules de Black-Scholes déjà présentes côté C++ : soit
  on appelle l'API, soit on assume une implémentation front pour la réactivité
  immédiate — mais alors un test compare les deux sur une grille de cas, sinon
  les deux dérivent.
- Une position dont les legs ont des maturités différentes n'a pas de « payoff à
  maturité » unique. Le cas calendar spread doit être traité, pas ignoré.
