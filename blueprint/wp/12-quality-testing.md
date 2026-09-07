# WP 12 — Qualité & tests

| | |
|---|---|
| **Dépend de** | [00](00-foundations.md) pour l'outillage ; **continu** sur tous les lots |
| **Bloque** | [13](13-perf-budget.md) |
| **Branche** | `web/12-quality` puis contributions dans chaque lot |

## Objectif

`web/` contient aujourd'hui **zéro test**. Le seul filet est `tsc`, et le lint
n'est pas bloquant. Ce lot fournit l'outillage et les tests transverses ; les
tests métier sont livrés **par chaque lot**, pas rattrapés à la fin.

## 1. Pyramide

| Niveau | Outil | Ce qu'on y met |
|---|---|---|
| Unitaire | Vitest | Fonctions pures : payoff, breakevens, géométrie de surface, formateurs, adaptateurs d'API, clés de cache. C'est là que doit vivre l'essentiel |
| Composant | Vitest + Testing Library + MSW | Un composant avec ses états chargement / vide / erreur / succès. Interrogé par rôle et par texte, jamais par classe CSS |
| Intégration | RTL + MSW | Un parcours dans une feature : remplir un formulaire de pricing, obtenir un résultat, changer de moteur |
| Bout en bout | Playwright | Cinq parcours seulement, sur la vraie stack `docker compose` |
| Visuel | Playwright screenshots | Storybook et les vues canoniques de la surface 3D |

Cinq parcours e2e, pas plus : pricer une vanille, afficher une surface de vol,
créer un portefeuille et le valoriser, lancer un backtest, se connecter et se
déconnecter. Un e2e coûte cher à maintenir ; au-delà de cinq on paie sans
gagner.

## 2. Les tests qui attrapent des bugs réels ici

Le domaine offre des vérifications gratuites qu'il serait absurde de ne pas
écrire :

- **Parité call-put** sur les prix rendus par l'écran.
- **`in + out = vanille`** sur les barrières, via la comparaison A/B du
  [lot 07](07-pricing-workbench.md).
- **Monotonie et convexité en strike** sur une grille de prix.
- **Variance totale croissante en maturité** sur les surfaces affichées.
- **Aller-retour d'export** : portefeuille → JSON → portefeuille identique.
- **Décroissance de l'erreur MC en 1/√N** sur le graphique de convergence.

Ces tests portent sur ce que l'écran **affiche**. Ils échouent aussi bien sur un
bug de pricing que sur un bug d'unité ou de formatage — ce qui est précisément
ce qu'on veut d'un test de front.

## 3. Tests de discipline

Des tests qui vérifient l'architecture, dans l'esprit du test de dérive de
protocole utilisé sur `agenticenv-chat`.

| Test | Ce qu'il empêche |
|---|---|
| **Dérive OpenAPI** | Que `schema.gen.ts` prenne du retard sur les modèles Pydantic ([ADR-005](../decisions.md#adr-005--types-générés-depuis-lopenapi-avec-test-de-dérive)) |
| **Sens de dépendance** | `import/no-restricted-paths` : `shared/` n'importe pas `features/`, `features/x` n'importe pas `features/y` |
| **Pas de `fetch` sauvage** | Aucun `fetch(` hors de `shared/api/` |
| **Pas de couleur en dur** | Aucun hexadécimal hors de `theme.css` |
| **Exhaustivité du catalogue** | Chaque produit activé a un schéma, au moins un moteur, un point d'entrée valide ; chaque `docKey` référence une entrée existante |
| **Routes exhaustives** | Chaque route déclarée a un composant et un titre |
| **Pas de `console.log`** | En dehors du logger partagé |

Ils sont peu coûteux et ce sont eux qui empêchent l'architecture de se dissoudre
en six mois.

## 4. Accessibilité

- `jest-axe` sur chaque story et chaque composant significatif, en test
  automatique.
- `eslint-plugin-jsx-a11y` bloquant.
- Un parcours Playwright **au clavier seul**.
- Vérifications manuelles consignées : lecteur d'écran sur le dialogue d'auth
  et sur la table de positions ; navigation dans le mode contraste forcé.

## 5. Régression visuelle

Playwright sur Storybook, dans les deux thèmes. Pour la 3D : les quatre vues
préréglées du [lot 05](05-viz-3d.md) sur une surface synthétique, avec une
tolérance de comparaison — le rendu GPU n'est pas déterministe au pixel près
d'une machine à l'autre, il faut donc figer la machine (conteneur de CI) et
tolérer un écart.

## 6. CI

Job `web` : `typecheck → lint → test → build → e2e → a11y → visuel`, sur toute
pull request. Rapport de couverture publié, **sans seuil obligatoire** : un
seuil de couverture pousse à écrire des tests qui couvrent sans vérifier.

## Critères d'acceptation

- [ ] Chaque lot livré arrive avec ses tests ; aucun lot n'est fusionné sans.
- [ ] Les cinq parcours e2e passent sur `docker compose`.
- [ ] Les sept tests de discipline sont actifs et échouent quand on les viole
      volontairement.
- [ ] Zéro violation axe sur les composants et les pages principales.
- [ ] La CI échoue sur un avertissement eslint.
- [ ] La suite unitaire tourne en moins de trente secondes en local.

## Pièges

- Ne pas tester l'implémentation. Un test qui casse quand on renomme une
  variable interne est un test qui coûte sans protéger.
- MSW doit servir les **mêmes** fixtures que Storybook, sinon on maintient deux
  jeux de données divergents.
- Une régression visuelle trop stricte devient une alarme qu'on finit par
  ignorer, ce qui est pire que pas d'alarme du tout.
