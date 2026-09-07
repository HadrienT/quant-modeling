# WP 01 — Design system

| | |
|---|---|
| **Dépend de** | [00](00-foundations.md) |
| **Bloque** | 03 (dure), 05 et 06 (les moteurs de rendu lisent ces tokens) |
| **Branche** | `web/01-design-system` |

## Objectif

Une seule source de vérité visuelle, en tokens CSS, validée pour le contraste et
le daltonisme, consommée aussi bien par les composants React que par les shaders
WebGL. Tout ce qui suit est mesuré, pas choisi à l'œil.

## Périmètre

**Dedans** : tokens, typographie, primitives de densité numérique, composants
shadcn de base, Storybook comme vitrine.
**Dehors** : le layout applicatif (→ [03](03-app-shell.md)), tout ce qui dessine
des données (→ [05](05-viz-3d.md), [06](06-viz-2d.md)).

## 1. Surfaces et encre

Le sombre est le défaut ([ADR-009](../decisions.md#adr-009--le-sombre-est-le-thème-par-défaut)).
Le clair est complet et maintenu — il sert aux captures et à l'impression.

| Rôle | Sombre | Contraste | Clair | Contraste |
|---|---|---|---|---|
| Plan de page | `#0b0e13` | — | `#f5f5f3` | — |
| Surface (carte, graphique) | `#141922` | — | `#fbfaf9` | — |
| Encre primaire | `#f2f5f8` | 16,1:1 | `#0b0e13` | 18,5:1 |
| Encre secondaire | `#a9b4c2` | 8,4:1 | `#4b5563` | 7,3:1 |
| Encre atténuée (axes, libellés) | `#7d8899` | 4,9:1 | `#6b7280` | 4,6:1 |
| Grille (filet) | `#212936` | 1,2:1 | `#e4e4e1` | 1,2:1 |
| Axe / ligne de base | `#38414f` | 1,7:1 | `#c3c2b7` | 1,7:1 |

Les deux encres atténuées passent le seuil AA de 4,5:1 pour du texte normal —
ce qui compte, parce que ce sont elles qui portent les graduations d'axe. Les
filets de grille sont volontairement sous 1,3:1 : une grille qu'on remarque est
une grille trop visible.

## 2. Palette de séries — validée, pas choisie

Ordre **fixe**, jamais cyclé. Une 9ᵉ série n'existe pas : elle devient
« Autres », ou on facette.

| Slot | Teinte | Sombre | Clair |
|---|---|---|---|
| 1 | bleu | `#3987e5` | `#2a78d6` |
| 2 | orange | `#d95926` | `#eb6834` |
| 3 | aqua | `#199e70` | `#1baf7a` |
| 4 | jaune | `#c98500` | `#eda100` |
| 5 | magenta | `#d55181` | `#e87ba4` |
| 6 | vert | `#008300` | `#008300` |
| 7 | violet | `#9085e9` | `#4a3aa7` |
| 8 | rouge | `#e66767` | `#e34948` |

Validé sur nos deux surfaces avec le script du référentiel dataviz :

```
sombre  #141922 : bande de clarté PASS · chroma PASS · CVD ΔE min 8,4 PASS
                  · vision normale ΔE min 19,3 PASS · contraste ≥3:1 PASS
clair   #fbfaf9 : CVD ΔE min 9,1 PASS · vision normale ΔE min 19,6 PASS
                  · contraste WARN sur aqua (2,7), jaune (2,08), magenta (2,58)
```

**Conséquence directe du WARN :** en thème clair, ces trois séries ne peuvent
pas être identifiées par la couleur seule. Elles portent obligatoirement un
libellé direct ou une vue tableau. Ce n'est pas une recommandation, c'est la
condition qui rend la palette utilisable.

Ces garanties valent pour les **paires adjacentes** (lignes, barres, empilements).
Pour les formes où toutes les paires se côtoient — nuage de points, bulles —
**plafonner à trois séries** ; au-delà, aucune des huit teintes ne tient les
seuils simultanément.

Ne jamais recolorer par le rang : la couleur suit l'entité. Filtrer un
portefeuille de six positions à trois ne doit pas repeindre les survivantes.

## 3. Le cas du P&L — la seule règle vraiment spécifique au domaine

Le réflexe rouge/vert est aussi le pire cas de daltonisme qui existe. La règle
se dédouble selon que le signe est **discret** ou **continu** :

| Cas | Encodage | Pourquoi |
|---|---|---|
| Signe discret : cellule de table, badge, tuile | Statut `good #0ca30c` / `critical #d03b3b`, **toujours** avec le signe `+`/`−` et une flèche | La convention métier est trop forte pour être ignorée, et le caractère de signe porte l'information indépendamment de la couleur |
| Échelle continue : heatmap de P&L, surface de greeks, matrice de stress | Divergent **bleu ↔ rouge**, gris neutre au zéro | Un dégradé rouge↔vert est illisible pour ~8 % des hommes ; le zéro doit être gris, jamais une teinte |

Les couleurs de statut (`good`, `warning #fab219`, `serious #ec835a`,
`critical`) sont **réservées** : jamais réutilisées comme « série 5 ». Toutes
passent 3:1 sur la surface sombre.

## 4. Magnitude — les rampes séquentielles

Rampe par défaut : **une seule teinte**, bleu, `#cde2fb` → `#0d366b`.

Et surtout, l'interdit qui va compter au [lot 05](05-viz-3d.md) : **pas de
colormap arc-en-ciel** (turbo, jet, rainbow). Deux raisons, la seconde propre à
la 3D :

1. Une rampe arc-en-ciel n'est pas monotone en clarté : elle fabrique des
   frontières visuelles là où la donnée est lisse, et en aplatit là où elle
   saute.
2. Sur une surface **éclairée**, l'ombrage module déjà la clarté. Si la colormap
   module aussi la clarté, les deux signaux se battent et on ne lit plus ni la
   forme ni la valeur. Sur une surface 3D, **la hauteur porte déjà la
   magnitude** : la couleur est un encodage redondant, et une rampe mono-teinte
   suffit largement. La précision se lit sur les isolignes, pas sur la teinte.

## 5. Typographie

- **Interface** : une sans système (`system-ui, -apple-system, "Segoe UI",
  sans-serif`). Supprimer Space Grotesk et le chargement Google Fonts de
  `web/index.html` — deux `preconnect` et une requête bloquante pour une police
  décorative, sur un outil qui affiche surtout des chiffres.
- **Chiffres** : une chasse fixe (JetBrains Mono ou la mono système),
  auto-hébergée, sous-ensemble latin, `font-display: swap`. Aucune requête
  vers un CDN tiers.
- `font-variant-numeric: tabular-nums` sur **toute colonne de nombres** et sur
  les graduations d'axe. Sans ça, les chiffres dansent d'une ligne à l'autre et
  une table de prix devient illisible. Les grands nombres isolés (tuile de
  métrique) gardent les chiffres proportionnels.

Échelle typographique restreinte : 5 tailles, pas 12. La densité vient du
`line-height` et de l'espacement, pas d'une multiplication des tailles.

## 6. Primitives de densité numérique

Ce sont ces composants, plus que les boutons, qui donnent au produit son
caractère d'outil. Chacun avec une story et un test.

| Composant | Rôle |
|---|---|
| `NumberCell` | Un nombre : chasse fixe tabulaire, précision par type de grandeur (prix 4 décimales, vol en points, greeks en notation adaptée), signe explicite, alignement à droite |
| `Uncertainty` | `12,3456 ± 0,0021` — la valeur et son erreur standard **inséparables**. Une erreur relative au-delà d'un seuil bascule l'affichage en avertissement |
| `DeltaBadge` | Variation signée, règle du §3, signe et flèche toujours présents |
| `Metric` | Tuile : libellé, valeur, unité, variation, note de bas de tuile. Pas de graphique décoratif dedans |
| `MetricRow` | Rangée de 3 à 5 `Metric`, grille alignée, jamais un mur de 12 tuiles |
| `Freshness` | « calculé il y a 4 min » / « données du 06/09 » — grise ce qu'il qualifie au-delà d'un seuil de péremption |
| `Provenance` | Puce « marché » vs « saisi » : la question la plus importante de tout l'écran de pricing |
| `EngineTag` | Étiquette du moteur ayant produit un chiffre (`analytic`, `mc`, `pde`, `binomial`…) |

**Règle transverse : jamais un prix Monte-Carlo sans son erreur standard.**
L'API renvoie déjà `mc_std_error` et les `*_std_error` des greeks ; le front
actuel ne les affiche pas systématiquement. Un chiffre MC sans barre d'erreur
est une affirmation non étayée.

## 7. Composants shadcn à installer

Au fil des besoins, pas en bloc : `button`, `input`, `select`, `combobox`
(`command` + `popover`), `dialog`, `sheet`, `tabs`, `tooltip`, `popover`,
`toast` (sonner), `table`, `skeleton`, `badge`, `separator`, `scroll-area`,
`switch`, `slider`, `dropdown-menu`, `alert`.

Deux réécritures immédiates par équivalence :

- La modale d'auth artisanale de [`App.tsx`](../../web/src/App.tsx) → `Dialog`
  (piège de focus, `aria-modal`, `Escape`, restitution du focus : tout ce qui
  manque aujourd'hui).
- Le popover `ProductInfo` positionné à la main (`getBoundingClientRect`,
  fermeture au scroll) → `Popover` Radix, qui gère la détection de collision et
  reste ancré pendant le scroll.

## 8. Le pont vers WebGL

Les shaders ne lisent pas les variables CSS. Il faut un pont explicite dans
`shared/styles/tokens.ts` :

```ts
readThemeTokens(): { series: string[]; sequential: string[]; surface: string; ... }
```

qui résout les variables calculées sur `document.documentElement` et
**réagit au changement de thème** (observer sur l'attribut `data-theme` +
`matchMedia('(prefers-color-scheme: dark)')`). Sans ça, basculer le thème
laisse la surface 3D dans l'ancienne palette — voir
[interdépendances §3.1](../dependencies.md#31-les-tokens-de-design--sharedstylesthemecss--wp-01).

## Critères d'acceptation

- [ ] `scripts/validate_palette.js` passe sur les deux surfaces, sortie
      archivée dans le repo pour que la vérification soit rejouable.
- [ ] Storybook présente tokens, primitives numériques et composants shadcn
      dans les deux thèmes.
- [ ] Aucune valeur hexadécimale en dur ailleurs que dans `theme.css` (règle
      eslint au [lot 12](12-quality-testing.md)).
- [ ] Aucune requête vers `fonts.googleapis.com` dans le HTML servi.
- [ ] Basculer le thème met à jour un canevas WebGL de démonstration.
- [ ] Test axe sans violation sur la page Storybook des composants.

## Pièges

- Un thème sombre n'est pas un thème clair inversé. Les pas sombres de la
  palette sont **choisis**, et validés sur la surface sombre.
- Ne pas laisser un composant shadcn conserver son apparence par défaut : le
  code est dans le repo pour être adapté aux tokens, pas pour être collé tel
  quel.
- Ne pas noyer les tokens dans des alias (`--color-button-primary-hover-bg`
  dérivé de `--color-brand-500`) : deux niveaux au maximum, sinon plus personne
  ne sait ce qui pilote quoi.
