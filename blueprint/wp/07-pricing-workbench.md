# WP 07 — Atelier de pricing

| | |
|---|---|
| **Dépend de** | [03](03-app-shell.md), [02](02-api-layer.md), [06](06-viz-2d.md) ; [99](99-recovered-work.md) en douce |
| **Bloque** | [09](09-portfolio-risk.md), [10](10-strategy-visualizer.md) — les deux réutilisent son catalogue |
| **Branche** | `web/07-pricing-workbench` |

## Objectif

L'écran central du produit. Choisir un instrument, le paramétrer, choisir un
moteur, obtenir un prix **avec son incertitude**, comprendre ce qu'on vient de
calculer, et pouvoir envoyer l'URL à quelqu'un.

## Ce qu'il faut démonter

[`usePricing.ts`](../../web/src/pages/pricing/usePricing.ts) contient
**88 appels à `useState`** dans une seule fonction : tous les paramètres de tous
les produits de toutes les catégories coexistent en permanence en mémoire,
qu'ils soient pertinents ou non pour le produit sélectionné. Ajouter un produit
veut dire ajouter des `useState` au même endroit, et six composants `*Fields`
lisent ce même objet géant.

Le problème n'est pas la longueur, c'est le modèle : **le formulaire est écrit
en dur au lieu d'être dérivé du produit.**

## 1. Le catalogue produit — l'artefact structurant du front

Une entrée par produit, dans `shared/products/`, qui remplace ce qui est
aujourd'hui éclaté entre `productRegistry.ts`, `types.ts`, `usePricing.ts`, les
six `*Fields.tsx` et le `catalog.ts` du portefeuille — avec des libellés qui
divergent déjà entre les deux pages.

```ts
type ProductDescriptor = {
  key: ProductKey;
  category: Category;
  label: string;
  enabled: boolean;              // remplace le drapeau de productRegistry
  schema: ZodSchema;             // paramètres, bornes, valeurs par défaut
  engines: EngineCapability[];   // quels moteurs, avec quelles options
  endpoint: OperationId;         // route de pricing, typée depuis l'OpenAPI
  docKey?: string;               // → productDocs (lot 99)
  greeks: GreekName[];           // ce que ce produit renvoie réellement
};
```

Quatre consommateurs, une définition : atelier de pricing, ajout de position au
portefeuille ([09](09-portfolio-risk.md)), legs de stratégie
([10](10-strategy-visualizer.md)), fiches pédagogiques
([99](99-recovered-work.md)). Voir
[interdépendances §3.4](../dependencies.md#34-le-descripteur-de-produit--featurespricingcatalog--wp-07).

Le drapeau `enabled` est conservé : c'est une bonne idée du code actuel
(`structured`, `volatility`, `fx`, `commodity` et la vol locale sont
volontairement désactivés parce que non vérifiés). Un produit désactivé reste
visible et grisé, avec **la raison** au survol — aujourd'hui il est grisé sans
explication.

## 2. Formulaires dérivés du schéma

- `react-hook-form` + `zod`, le schéma venant du descripteur.
- Seuls les champs du produit sélectionné existent : changer de produit
  démonte les champs précédents, il n'y a plus d'état résiduel.
- Validation au niveau du champ **et** croisée : barrière au-dessus du spot
  pour un *up-and-out*, knock-in sous le coupon pour un autocall, poids de
  panier qui somment à 1, maturité positive. L'API renvoie ces erreurs
  aujourd'hui ; les attraper avant l'aller-retour est plus rapide et plus clair.
- Unités explicites dans le champ (vol en %, maturité en années, taux en %) et
  conversion vers la convention de l'API au bord — pas au milieu du formulaire.
- Valeurs par défaut sensées et cohérentes entre produits.

## 3. Moteurs et modèles

Une **matrice de capacités** produit × moteur, dérivée du descripteur : le
sélecteur ne propose que ce qui est réellement disponible, et grise le reste
avec la raison. Aujourd'hui le verrouillage est écrit à la main
(`ModelSelector.tsx` : `const analyticLocked = engine === "analytic"`).

Les options qui n'ont de sens que pour certains moteurs — nombre de chemins,
pas de temps, graine, pont brownien, réduction de variance, Sobol — apparaissent
**sous le moteur choisi**, pas dans un bloc permanent.

La **graine** est un champ de premier plan, pas une option avancée : un prix
Monte-Carlo non reproductible n'est pas vérifiable, et c'est exactement le
genre de détail qui distingue un outil sérieux.

## 4. Panneau de résultats

C'est là que se joue la crédibilité de l'écran.

- Prix en évidence, **et son erreur standard à côté**, jamais séparés
  (`Uncertainty` du [lot 01](01-design-system.md)). L'API renvoie
  `mc_std_error` et les `*_std_error` par greek : les afficher tous.
- Greeks en tableau dense, chasse fixe tabulaire, avec leur propre incertitude
  quand elle existe.
- `EngineTag` et `Provenance` : quel moteur, quelle source de volatilité
  (marché ou saisie), quelle heure de calcul.
- `diagnostics` de l'API affiché, pas ignoré.
- Analytique obligataire (`bond_analytics` : duration, convexité, DV01) dans son
  propre bloc pour les produits de taux.
- **Graphique de convergence** ([lot 06](06-viz-2d.md)) : erreur en fonction du
  nombre de chemins, en log-log, avec la pente 1/√N de référence. Sur un moteur
  Monte-Carlo, c'est la preuve visuelle que l'estimateur se comporte bien.
- Profils de payoff et de greeks en fonction du spot, à côté du prix ponctuel.

## 5. Comparaison A/B

Deux configurations côte à côte, avec la différence calculée : même produit sur
deux moteurs (analytique vs Monte-Carlo vs arbre vs EDP), ou deux jeux de
paramètres.

C'est la fonctionnalité qui transforme la page en **outil de validation** : voir
le Monte-Carlo converger vers l'analytique sur une vanille, ou vérifier
`in + out = vanille` sur une barrière, en deux clics. Ces vérifications gratuites
sont ce qui montre qu'on pense en financier et pas seulement en développeur.

## 6. État dans l'URL

Toute la configuration vit dans les search params typés du
[lot 03](03-app-shell.md). Conséquences : on met une session de pricing en
favori, on la partage, le bouton retour du navigateur fonctionne, et le
rechargement ne perd rien. Un bouton *Copier le lien*.

Presets nommés, stockés localement, pour les configurations récurrentes.

## 7. Fiches produit

Reprise et généralisation du travail récupéré
([lot 99](99-recovered-work.md)) : `Formula` (KaTeX), `ProductInfo`,
`productDocs`. Trois évolutions :

- Le popover positionné à la main devient un `Popover` Radix
  ([lot 01 §7](01-design-system.md#7-composants-shadcn-à-installer)).
- Le déclencheur sort du `<label>` : un `<button>` à l'intérieur d'un `<label>`
  est un contenu interactif imbriqué, que le modèle de contenu HTML interdit.
- KaTeX est chargé en `lazy` : ~270 Ko de CSS et de polices pour un popover que
  la plupart des visiteurs n'ouvriront pas.

## Fichiers supprimés

Tout `web/src/pages/pricing/` et `web/src/pages/Price.tsx` — soit
`usePricing.ts`, `types.ts`, `productRegistry.ts`, `ResultsPanel.tsx`,
`ModelSelector.tsx`, `CommonFields.tsx`, `OptionFields.tsx`, `ExoticFields.tsx`,
`BondFields.tsx`, `FutureFields.tsx`, `FXFields.tsx`, `CommodityFields.tsx`,
`StructuredFields.tsx`, `VolatilityFields.tsx`. `productDocs.ts` **migre**, il
n'est pas supprimé.

## Critères d'acceptation

- [ ] Ajouter un nouveau produit ne touche qu'un fichier de descripteur.
- [ ] Aucun prix Monte-Carlo affiché sans son erreur standard.
- [ ] Changer de produit ne laisse aucun paramètre du produit précédent dans
      l'état (test).
- [ ] Une contrainte invalide (barrière du mauvais côté) est signalée avant
      l'envoi.
- [ ] L'URL restitue exactement l'écran (test e2e).
- [ ] La comparaison A/B montre le Monte-Carlo converger vers l'analytique sur
      une vanille, dans les barres d'erreur.
- [ ] Une même graine donne deux fois le même prix.
- [ ] Le catalogue est consommé par le lot 09 sans duplication.

## Pièges

- Ne pas reconstruire un `usePricing` géant sous un autre nom. Si un fichier de
  cette feature dépasse ~200 lignes, le découpage est raté.
- Le catalogue appartient à `shared/products/`, **pas** à `features/pricing/` —
  sinon le portefeuille devra importer une feature
  ([interdépendances §5](../dependencies.md#5-le-sens-de-dépendance-à-ne-jamais-inverser)).
- Le contenu des fiches décrit **les moteurs de ce dépôt**, pas la théorie
  générale. Il périme quand un moteur change : le traiter comme une docstring.
