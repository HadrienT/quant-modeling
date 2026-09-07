# WP 99 — Travail perdu avec la VM

| | |
|---|---|
| **Dépend de** | rien — pure rédaction |
| **Bloque** | [07](07-pricing-workbench.md) en douce : l'atelier fonctionne sans, l'icône ⓘ ne s'affiche simplement pas |
| **Branche** | `recover/product-docs-popover` (partie code, faite), puis `content/product-docs` |

## Ce qui s'est passé

La VM portant la dernière grosse mise à jour a été supprimée avant que son
commit soit poussé. Le dépôt local était à `3dd68a7 Detailed products`, soit
l'état d'`origin/main`.

**Ce qui a été récupéré**, via un commit orphelin de *stash* (`79c7d9d`,
« WIP on main: 3dd68a7 ») retrouvé par `git fsck --lost-found`, plus les
fichiers non suivis restés dans l'arbre de travail :

| Fichier | État |
|---|---|
| `web/src/components/Formula.tsx` | complet — rendu KaTeX, dégradation propre sur formule invalide |
| `web/src/components/ProductInfo.tsx` | complet — déclencheur ⓘ, panneau flottant, fermeture au clic extérieur / `Escape` / défilement |
| `web/src/pages/pricing/productDocs.ts` | **partiel : 3 entrées sur 16** |
| `web/src/pages/Price.tsx` | complet — le composant est câblé sur les six sélecteurs |
| `web/src/styles.css` | complet — 82 lignes |
| `web/package.json`, `main.tsx` | complet — dépendance et import CSS de KaTeX |

Le tout est commité en `ce53b85`. La configuration des ports d'hôte
(`QM_API_PORT` / `QM_WEB_PORT`) et `etc/roadmap.md` ont été commités séparément
dans `d5f2b08` et `0854077`.

**Ce qui reste perdu** : le contenu rédactionnel des fiches produit au-delà des
trois présentes. Rien dans git ne permet de le retrouver — le `dist/` construit
sur la machine correspond au même état partiel.

## Ce qu'il faut refaire

`productDocs.ts` contient `option`, `barrier` et `autocall`. Manquent les
treize autres clés, plus deux qui n'ont pas encore d'emplacement :

| Catégorie | Clés à rédiger |
|---|---|
| Vanille (`instrument`) | `future`, `bond` |
| Exotiques (`exoticProduct`) | `digital`, `lookback`, `basket`, `rainbow` |
| Structurés (`structuredProduct`) | `mountain` |
| Volatilité (`volProduct`) | `variance-swap`, `volatility-swap`, `dispersion-swap` |
| FX (`fxProduct`) | `fx-forward`, `fx-option` |
| Matières premières (`commodityProduct`) | `commodity-forward`, `commodity-option` |

**Manque de câblage à corriger au passage** : le sélecteur `product`
(`vanilla` / `asian` / `american`) n'a pas de `ProductInfo`, alors que
l'asiatique et l'américaine sont les deux produits qui méritent le plus une
explication — moyenne arithmétique contre géométrique, control variate
géométrique, exercice anticipé et frontière d'exercice. Ajouter l'emplacement
et les deux fiches.

## Le format, et pourquoi il est bon

Chaque entrée porte quatre sections : **payoff** (KaTeX), **hypothèses**,
**comment c'est pricé ici**, **bon à savoir**.

La troisième est celle qui fait la valeur de l'exercice : elle ne décrit pas la
théorie du pricing en général, elle décrit **les moteurs de ce dépôt**. La fiche
`autocall` existante dit par exemple que le drapeau « knock-in continu »
n'applique pas de correction par pont brownien et se contente de vérifier la
barrière à chaque date d'observation — donc qu'il faut lire « continu » comme
« vérifié plus souvent », pas comme un suivi en temps continu. C'est le genre de
précision qu'on ne trouve nulle part ailleurs que dans le code, et c'est
exactement ce qui rend ces fiches utiles.

Corollaire : **elles périment comme une docstring.** Chaque fiche est à
revérifier contre la source du moteur qu'elle décrit dès que ce moteur change.

## Méthode de rédaction

1. Lire le moteur correspondant dans `src/engines/` et `src/instruments/`
   **avant** d'écrire la section « comment c'est pricé ici ». Ne rien affirmer
   qui ne soit vérifiable dans le code.
2. Section « bon à savoir » : privilégier les vérifications gratuites
   (`in + out = vanille`, parité call-put, bornes de monotonie) et les
   conventions implicites — elles sont utiles à l'utilisateur et elles sont
   exactement ce qu'on demande en entretien.
3. Signaler les limites au lieu de les taire. Une fiche qui dit ce que le
   moteur ne fait pas inspire plus confiance qu'une fiche qui ne promet que des
   succès.
4. Contenu en anglais, comme le reste de l'interface.

## Migration vers le nouveau front

Au [lot 07](07-pricing-workbench.md), le contenu est conservé tel quel ; seule
la mécanique change :

- Le popover positionné à la main devient un `Popover` Radix.
- Le déclencheur sort du `<label>` : un `<button>` dans un `<label>` est un
  contenu interactif imbriqué, interdit par le modèle de contenu HTML.
- `docKey` devient un champ du descripteur produit, ce qui permet au test
  d'exhaustivité du [lot 12](12-quality-testing.md) de repérer une clé morte ou
  une fiche orpheline.
- KaTeX passe en chargement `lazy` — ~270 Ko de CSS et de polices pour un
  panneau que la plupart des visiteurs n'ouvriront jamais.

## Critères d'acceptation

- [ ] Les seize produits activés ont une fiche.
- [ ] Chaque affirmation de la section « comment c'est pricé ici » est
      traçable à un fichier de `src/`.
- [ ] Toutes les formules KaTeX se rendent sans erreur (test qui parcourt le
      registre et appelle `renderToString`).
- [ ] Un `docKey` sans entrée fait échouer le test d'exhaustivité.

## Leçon d'exploitation

La perte tient à un commit resté local. Deux habitudes suffisent à l'éviter :
pousser la branche de travail en fin de session même inachevée, et ne jamais
laisser une machine éphémère porter seule plus d'une journée de travail.
