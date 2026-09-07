# WP 05 — Visualisation 3D

| | |
|---|---|
| **Dépend de** | [01](01-design-system.md) (douce : seulement les tokens de couleur) |
| **Bloque** | [08](08-market-data.md), [15](15-future-quant-surfaces.md) |
| **Branche** | `web/05-viz-3d` |

## Objectif

Un moteur de surface WebGL réutilisable — pas un composant « surface de vol ».
Il consomme une grille normalisée et ne sait rien de la finance : la volatilité
implicite, la volatilité locale, une nappe de P&L et plus tard un profil
d'exposition sont le même objet mathématique.

C'est la pièce de démonstration du projet. Elle se développe **isolée dans
Storybook** sur des surfaces synthétiques, sans API ni shell — donc en parallèle
de tout le reste
([voie C](../dependencies.md#4-ce-qui-peut-avancer-en-parallèle)).

## Stack

`three.js` + `@react-three/fiber` + `@react-three/drei` +
`@react-three/postprocessing`
([ADR-002](../decisions.md#adr-002--threejs--react-three-fiber-pour-la-3d)).

## 1. Contrat d'entrée

```ts
type SurfaceGrid = {
  x: Float64Array;        // ex. strikes — pas nécessairement régulier
  y: Float64Array;        // ex. maturités — pas nécessairement régulier
  z: Float64Array;        // x.length * y.length, NaN = trou
  axes: { x: AxisSpec; y: AxisSpec; z: AxisSpec };  // libellé, unité, format
};
```

Les réponses de l'API (`IVSurfaceResponse`, `SurfaceGridResponse`, profils à
venir) s'y ramènent par un adaptateur dans `shared/viz/adapters/`. Le moteur ne
voit jamais un type d'API
([interdépendances §3.5](../dependencies.md#35-la-grille-de-surface--sharedvizsurfacegrid--wp-05)).

**Les trous sont des trous.** L'API renvoie `Array<Array<number | null>>` : un
`null` est un strike sans cotation. Le moteur les rend comme **absence de
matière** (triangles écartés), jamais comme une valeur interpolée. Une surface
lisse là où il n'y a pas de donnée est un mensonge sur la qualité du marché — et
la qualité de la donnée est précisément le sujet difficile de ce projet.

**Grilles non régulières.** Strikes et maturités sont espacés irrégulièrement.
La géométrie est construite dans un espace paramétrique normalisé ; ce sont les
**graduations d'axe** qui portent les vraies valeurs. Ne pas déformer la grille
pour la rendre carrée : le sourire de volatilité aux ailes serait faussé.

## 2. Géométrie

- `BufferGeometry` construite à la main : positions, normales et indices dans
  des `Float32Array` / `Uint32Array`, une seule allocation, réutilisée entre
  deux mises à jour de la même dimension.
- Normales calculées **analytiquement** depuis la grille (différences finies sur
  les voisins), pas via `computeVertexNormals()` — sinon les trous polluent les
  normales de leurs voisins et l'éclairage montre des artefacts en bord de trou.
- Trous : les quads dont un sommet est `NaN` ne sont pas indexés. Coût nul au
  rendu, contrairement à un `discard` en shader.
- Fonctions pures dans `shared/viz/geometry/`, testables sans WebGL — c'est là
  que vivent les tests unitaires du lot.

## 3. Matière et couleur

- Colormap **mono-teinte** issue des tokens du [lot 01](01-design-system.md),
  échantillonnée dans une texture LUT 1D de 256 pixels.
  **Pas de turbo, pas de jet, pas d'arc-en-ciel** — la raison est développée au
  [lot 01 §4](01-design-system.md#4-magnitude--les-rampes-séquentielles) : sur
  une surface éclairée, l'ombrage module déjà la clarté, et une colormap qui la
  module aussi rend illisibles la forme et la valeur en même temps. La hauteur
  porte déjà la magnitude ; la couleur est redondante, et c'est très bien.
- **Isolignes dans le fragment shader** — c'est ce qui remplace la précision
  qu'on n'attend plus de la teinte. Anti-aliasées via `fwidth()` sur la valeur
  interpolée, pas de géométrie supplémentaire, épaisseur constante à l'écran
  quel que soit le zoom. Espacement des niveaux aligné sur les graduations de
  l'axe vertical.
- **Grille de données en surimpression**, discrète : elle montre où sont les
  vraies cotations par rapport à la surface interpolée. Activable, désactivée
  par défaut.
- Mode comparaison : deux surfaces, soit côte à côte avec caméras synchronisées,
  soit — plus intéressant — une **surface de différence** (vol locale − vol
  implicite) rendue avec la rampe divergente bleu ↔ rouge et zéro gris. C'est la
  vue qui dit quelque chose que ni l'une ni l'autre ne dit seule.

## 4. Éclairage et post-traitement

- Trois lumières : principale, remplissage, contre-jour, sur un environnement
  généré **procéduralement** (`Lightformer` de drei). Pas de HDRI téléchargée :
  la CSP du [lot 14](14-deploy-selfhost.md) interdit les ressources externes, et
  un fichier de plusieurs mégaoctets ruinerait le budget du
  [lot 13](13-perf-budget.md).
- Tone mapping ACES, sortie en sRGB.
- Post-traitement : **SMAA et un peu d'occlusion ambiante, rien d'autre.**
  Pas de bloom. Le bloom fait rayonner les pics et suggère une intensité que la
  donnée n'a pas — c'est joli et c'est faux. La règle du lot : *le
  post-traitement ne modifie jamais la lisibilité d'une valeur.*
- Ombre portée douce sur le plan de base, qui aide énormément à percevoir le
  relief.

## 5. Interaction

| Geste | Effet |
|---|---|
| Glisser | Orbite, avec amortissement, angle polaire borné (jamais sous la surface) |
| Molette | Zoom, bornes min/max |
| Survol | `raycast` → lecture exacte `(K, T, σ)` dans une étiquette ancrée au point, valeur **non interpolée** quand le curseur est sur un nœud |
| Clic | Pose un plan de coupe |
| Double-clic | Recadre sur la surface |
| Clavier | Flèches pour orbiter, `+`/`−` pour zoomer, `R` pour réinitialiser |

**Plans de coupe** — la fonctionnalité qui rend la 3D utile plutôt que
décorative : figer une maturité donne le **smile**, figer un strike donne la
**structure par terme**. Le plan s'affiche en 3D et la coupe correspondante est
tracée en 2D à côté par le [lot 06](06-viz-2d.md). C'est l'endroit exact où les
deux moteurs de rendu doivent partager leurs échelles et leurs couleurs.

**Vues préréglées** avec transition animée : trois-quarts (défaut), face
(smile), profil (terme), dessus (qui dégénère en heatmap). Elles servent aussi
de vues canoniques pour la régression visuelle et pour les captures du README.

**Axes 3D** : graduations et libellés lisibles quelle que soit l'orientation,
orientés vers la caméra, désencombrés quand ils se chevauchent, unité affichée
une seule fois.

## 6. Performance

- `frameloop="demand"` : on ne rend que lorsque quelque chose bouge. Sans cela
  la page consomme un cœur GPU en permanence, ventilateur compris, sur un écran
  statique.
- Libération explicite des géométries, matières et textures au démontage — la
  fuite mémoire GPU classique de R3F.
- Chargement `lazy` du bundle three.js, avec un cadre de la bonne taille pendant
  le chargement pour éviter le saut de mise en page.
- Le seuil de taille de grille au-delà duquel on sous-échantillonne est mesuré,
  pas deviné.

## 7. Dégradation — obligatoire

Une page qui n'affiche rien quand WebGL manque est une page cassée. Trois cas :

| Situation | Comportement |
|---|---|
| WebGL2 indisponible ou contexte perdu | Bascule sur la heatmap 2D du [lot 06](06-viz-2d.md), avec un message expliquant pourquoi |
| `prefers-reduced-motion` | Pas de transition de caméra ni de rotation automatique ; l'interaction reste |
| Lecteur d'écran / clavier seul | Un canevas WebGL n'est pas accessible, point. La **vue tableau** de la grille est obligatoire, atteignable par un bouton, jamais cachée derrière une préférence |

## 8. Export

Rendu hors écran en haute résolution vers un PNG. Sert aux captures du README —
la démonstration visuelle du projet — et à la régression visuelle du
[lot 12](12-quality-testing.md).

## Critères d'acceptation

- [ ] Une surface synthétique (une gaussienne, un sourire analytique) se rend
      correctement dans Storybook sans API.
- [ ] Une grille avec 15 % de trous les affiche comme absence, sans artefact
      d'éclairage sur les bords.
- [ ] Le survol renvoie la valeur exacte du nœud, vérifié contre la donnée
      source dans un test.
- [ ] Basculer le thème met à jour colormap, fond et éclairage sans remonter le
      composant.
- [ ] `frameloop` retombe à zéro image par seconde après trois secondes
      d'inactivité (mesuré).
- [ ] Aucune fuite : monter/démonter cent fois laisse le nombre de géométries
      three.js stable.
- [ ] Le repli 2D s'affiche quand on force la perte du contexte WebGL.
- [ ] Les quatre vues préréglées produisent des captures stables en régression
      visuelle.

## Pièges

- **Ne pas interpoler les trous.** C'est la tentation permanente parce que le
  résultat est plus joli. C'est aussi le seul moyen de rendre le graphique faux.
- Le pont CSS → uniforms doit réagir au thème
  ([lot 01 §8](01-design-system.md#8-le-pont-vers-webgl)) ; c'est l'oubli
  classique.
- `OrbitControls` sans borne polaire permet de passer sous la surface et de
  perdre complètement le repère.
- Les étiquettes HTML ancrées en 3D (`drei/Html`) coûtent cher : quelques-unes,
  pas une par nœud.
- Ne pas laisser la 3D devenir la seule façon de lire la donnée. Toute
  information qu'on ne peut obtenir qu'en faisant tourner une surface à la
  souris est une information mal présentée.
