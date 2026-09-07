# WP 11 — Backtest

| | |
|---|---|
| **Dépend de** | [03](03-app-shell.md), [02](02-api-layer.md), [06](06-viz-2d.md) |
| **Bloque** | rien |
| **Branche** | `web/11-backtest` |

## Objectif

Optimiser une allocation sur une fenêtre passée, la rejouer, et comparer au
S&P 500 — honnêtement. La page existe déjà
([`Backtest.tsx`](../../web/src/pages/Backtest.tsx), 762 lignes en un seul
fichier : formulaire, appel, graphiques et métriques mélangés) ; il s'agit de la
redécouper et de resserrer sa lecture statistique.

## Route de l'API

`POST /api/backtest/run` : tickers, fenêtre d'optimisation, capital initial,
bornes de poids, fréquence de rebalancement. Retourne allocation, valeurs de
portefeuille, valeurs S&P 500, métriques et **avertissements**.

## 1. Formulaire

- Sélection de tickers en `Combobox` multiple avec puces, pas une saisie libre
  séparée par des virgules.
- Plage de dates avec préréglages (5 ans, 10 ans, depuis 2000) et validation
  croisée : `opt_start < opt_end`, historique suffisant, `min_share ≤ max_share`.
- Capital, bornes de poids, fréquence de rebalancement avec unités explicites.
- Configuration dans l'URL : un backtest se partage.

## 2. Exécution

Un backtest est long. Il lui faut un état d'exécution réel : progression ou au
minimum un indicateur non ponctuel, annulation, et affichage du temps écoulé.
Le résultat reste en cache — relancer à l'identique ne doit pas recalculer.

## 3. Résultats

- **Courbe d'equity portefeuille vs S&P 500, indexée base 100.** Deux
  capitaux différents sur deux axes verticaux seraient exactement l'anti-patron
  interdit au [lot 06](06-viz-2d.md) ; la base 100 est la bonne réponse.
- **Drawdown** en panneau lié sous la courbe, axe des temps partagé, crosshair
  synchronisé.
- **Allocation** en barres horizontales triées, avec prix de début, prix de fin
  et rendement par ligne. Le champ `weight` est déjà là — pas de camembert.
- Tuiles de métriques : rendement total et annualisé, drawdown maximum, Sharpe,
  Sharpe optimal, alpha, beta, plus haut et plus bas historiques.
- **`alpha` et `beta` sont nullables** dans la réponse. Aujourd'hui rien ne
  distingue « non calculable » de zéro. Afficher « n/d » avec la raison.
- **Les `warnings` de l'API sont affichés en évidence**, pas en petit en bas.
  Un ticker sans historique suffisant change l'interprétation de tout le
  résultat.

## 4. Honnêteté statistique — la partie qui compte

Une courbe qui monte, sans le contexte ci-dessous, est une démonstration de
surajustement. Ce bloc n'est pas cosmétique : c'est ce qui fait la différence
entre une page qui impressionne et une page qui convainc.

- **Distinguer visuellement la fenêtre d'optimisation de la fenêtre de test.**
  Une bande grisée sur la période in-sample. Sans elle, on présente une
  performance obtenue en connaissant l'avenir.
- Rappeler en clair ce que le backtest **n'inclut pas** : coûts de transaction,
  slippage, biais du survivant sur l'univers, dividendes selon la source.
- Sharpe affiché avec son taux sans risque et sa convention d'annualisation.
- **Comparaison de plusieurs runs** : sauvegarder localement plusieurs
  exécutions et superposer leurs courbes. C'est ce qui rend visible la
  sensibilité aux paramètres — et donc la fragilité éventuelle du résultat.

## Fichiers supprimés

`web/src/pages/Backtest.tsx` — découpé en `features/backtest/` : formulaire,
requête, graphiques, métriques, comparaison.

## Critères d'acceptation

- [ ] Aucun graphique à deux axes verticaux ; la comparaison est en base 100.
- [ ] La fenêtre d'optimisation est visuellement distincte sur la courbe.
- [ ] Les avertissements de l'API sont visibles sans défilement.
- [ ] `alpha`/`beta` nuls affichent « n/d » et sa raison.
- [ ] Un backtest en cours est annulable et son résultat est mis en cache.
- [ ] Deux runs se superposent pour comparaison.
- [ ] Aucun fichier de la feature ne dépasse ~200 lignes.

## Pièges

- Ne pas présenter le Sharpe optimal (in-sample) et le Sharpe réalisé côte à
  côte sans les distinguer nettement : le premier est un maximum obtenu sur les
  données, le second une mesure. Les confondre est la faute classique.
- Les séries de prix ont des trous (jours fériés, suspensions) : la courbe ne
  doit pas les interpoler silencieusement.
- Une fenêtre trop courte donne des métriques instables. Avertir sous un seuil
  d'observations plutôt que d'afficher un Sharpe calculé sur trois mois.
