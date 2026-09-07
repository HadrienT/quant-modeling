# Interdépendances

Le graphe visuel est dans [`README.md §5`](README.md#5-graphe-de-dépendances).
Ce fichier dit **de quoi** chaque dépendance est faite, ce qui permet de savoir
laquelle est réellement bloquante et laquelle peut être court-circuitée par un
contrat écrit à l'avance.

---

## 1. Chemin critique

```
00 Fondations → 01 Design system ─┐
                                  ├→ 03 Shell → toutes les pages
00 Fondations → 02 Couche API ────┘
```

Rien d'autre n'est sur le chemin critique. Tout retard sur `00`, `01`, `02` ou
`03` décale l'intégralité du chantier ; un retard sur n'importe quel autre lot
ne décale que lui.

**Corollaire de séquencement :** ne pas commencer une page tant que `03` n'est
pas livré. C'est l'erreur classique — on écrit une page contre un layout
provisoire, puis on la réécrit.

## 2. Matrice

`dure` = impossible de commencer avant. `douce` = on peut commencer contre un
contrat figé (type, token, clé de cache) et brancher le vrai plus tard.

| WP | Dépend de | Nature | Ce qui transite exactement |
|---|---|---|---|
| 00 Fondations | — | — | — |
| 01 Design system | 00 | dure | Tailwind configuré, alias de chemins, Storybook |
| 02 Couche API | 00 | dure | TS strict, script de génération, MSW installé |
| 02 Couche API | *API FastAPI* | **externe** | `/openapi.json` stable, ADR-008 tranché |
| 03 Shell | 01 | dure | Tokens, `Button`, `Dialog`, `Command`, `Toast` |
| 03 Shell | 02 | douce | `QueryClient` et frontière d'erreur ; le shell peut se monter sur des données factices |
| 04 Auth | 03 | dure | Dialog du design system, emplacement dans la barre de navigation |
| 04 Auth | 02 | dure | `useLogin` / `useMe`, injection du jeton dans le client |
| 05 Viz 3D | 01 | douce | Uniquement les tokens de couleur et la colormap ; le moteur est autonome |
| 06 Viz 2D | 01 | dure | Tokens et échelles partagées avec la 3D pour rester cohérent |
| 07 Pricing | 03, 02 | dure | Route, layout, hooks `usePrice*` |
| 07 Pricing | 99 Travail perdu | douce | Contenu des fiches produit ; l'atelier fonctionne sans, l'icône ⓘ ne s'affiche simplement pas |
| 08 Marché | 03, 02 | dure | Route, hooks `useIvSurface` / `useRatesCurve` / `usePriceHistory` |
| 08 Marché | 05, 06 | dure | C'est la page qui consomme les deux moteurs de rendu |
| 09 Portefeuille | 03, 02, 04 | dure | Un portefeuille serveur exige une session |
| 09 Portefeuille | 07 | dure | Réutilise les formulaires produit pour ajouter une position |
| 09 Portefeuille | 06 | douce | Graphiques de risque ; la table seule est déjà utile |
| 10 Stratégies | 03, 06 | dure | Route, tracé du payoff |
| 10 Stratégies | 07 | dure | Réutilise le moteur de payoff et le catalogue produit |
| 11 Backtest | 03, 02, 06 | dure | Route, `useBacktest`, courbe d'equity |
| 12 Qualité | 00 | dure | Vitest, MSW, Playwright installés dès les fondations |
| 12 Qualité | *tous* | continue | Chaque lot livre ses propres tests ; `12` fournit l'outillage et les tests de discipline |
| 13 Perf | 12 | dure | Il faut une mesure automatisée avant de fixer un budget |
| 14 Déploiement | 13, 02 | dure | Budgets tenus, config runtime en place |
| 15 Surfaces futures | 05, 09 | dure | Réutilise le moteur de surface et l'agrégation de risque |
| 15 Surfaces futures | *roadmap quant* | **externe** | Rien à afficher tant que la calibration / l'AAD / le GPU n'existent pas |

## 3. Les artefacts partagés — c'est là que ça casse

Une dépendance douce ne tient que si le contrat est écrit **avant**. Voici les
cinq objets qui traversent les frontières de lots ; toute modification de l'un
d'eux se répercute partout et doit passer par une issue, pas par un commit
discret.

### 3.1 Les tokens de design — `shared/styles/theme.css` · WP 01

Consommés par : tous les lots visuels, **y compris les moteurs de rendu**. La
3D et la 2D lisent les mêmes variables CSS pour leurs couleurs, sinon un
graphique change de bleu selon la librairie qui l'a dessiné.

*Piège :* WebGL ne lit pas les variables CSS. Il faut un pont explicite
(`readThemeTokens()` qui résout les variables calculées et alimente les
uniforms), et il doit réagir au changement de thème.

### 3.2 `schema.gen.ts` — WP 02

Consommé par : 07, 08, 09, 11 et tout le reste. Généré, jamais édité à la main.
**Une modification de modèle Pydantic côté `api/` est une modification du
front.** Le test de dérive ([ADR-005](decisions.md#adr-005--types-générés-depuis-lopenapi-avec-test-de-dérive))
est ce qui rend cette phrase vraie plutôt que pieuse.

### 3.3 Les clés de cache — `shared/api/queryKeys.ts` · WP 02

Consommées par : toutes les pages. Une fabrique centralisée et hiérarchique,
pas des tableaux littéraux dispersés — sans quoi personne ne sait plus quoi
invalider après un `POST`.

*Cas concret :* pricer un portefeuille (`09`) doit invalider ses positions,
son résumé de risque et sa VaR, mais surtout **pas** les surfaces de marché.

### 3.4 Le descripteur de produit — `features/pricing/catalog/` · WP 07

Consommé par : 07 (formulaire), 09 (ajout de position), 10 (legs de stratégie),
99 (fiches pédagogiques). Un produit = une entrée décrivant son schéma zod, ses
engines compatibles, ses valeurs par défaut, son libellé et sa clé de doc.

C'est **l'artefact le plus structurant du front**. Aujourd'hui la même
information est éparpillée entre `productRegistry.ts`, `types.ts`,
`usePricing.ts`, `catalog.ts` (portefeuille) et six composants `*Fields.tsx` —
avec des libellés qui divergent déjà entre la page de pricing et celle du
portefeuille. Un seul catalogue, quatre consommateurs.

### 3.5 La grille de surface — `shared/viz/SurfaceGrid` · WP 05

Consommée par : 05 (rendu), 08 (IV et local vol), 15 (profils d'exposition).
Forme normalisée `{ x[], y[], z[][], holes }`, indépendante de l'API — les
trois réponses serveur (`IVSurfaceResponse`, `SurfaceGridResponse`, futurs
profils) s'y ramènent par un adaptateur.

*Piège :* l'API renvoie `Array<Array<number | null>>`. Les `null` sont des
trous réels (strike sans quote) et **ne doivent pas** être interpolés
silencieusement — un trou affiché comme une bosse lisse est un mensonge sur la
donnée. Le moteur doit les rendre comme absence.

## 4. Ce qui peut avancer en parallèle

| Voie | Lots | Condition |
|---|---|---|
| A — Socle | 00 → 01 → 03 | Séquentiel, c'est le chemin critique |
| B — Données | 02 | Démarre dès `00`, en parallèle de `01` |
| C — Moteur 3D | 05 | Démarre dès que les tokens de couleur existent ; se développe **isolé dans Storybook** sur des surfaces synthétiques, sans API ni shell |
| D — API | ADR-008, harmonisation des routes | Côté `api/`, indépendant du front |
| E — Contenu | 99 fiches produit | Pure rédaction, aucune dépendance de code |

Les voies C et E sont les plus faciles à confier ou à traiter par à-coups :
elles ne bloquent personne et personne ne les bloque.

## 5. Le sens de dépendance à ne jamais inverser

```
shared/  ←  features/  ←  app/
```

`shared/` ne connaît aucune feature. `features/x` ne connaît pas
`features/y` — si deux features ont besoin de la même chose, cette chose
descend dans `shared/`. Seul `app/` connaît tout le monde.

Les deux cas où la tentation sera forte, et la réponse :

- **09 Portefeuille a besoin des formulaires de 07 Pricing.** Réponse : le
  catalogue produit (§3.4) et les composants de formulaire vivent dans
  `shared/products/`, pas dans `features/pricing/`. `07` en est un
  consommateur comme les autres, pas le propriétaire.
- **10 Stratégies a besoin du moteur de payoff.** Même réponse :
  `shared/payoff/`, fonctions pures, testées unitairement, sans React.

Une règle eslint (`import/no-restricted-paths`) rend l'inversion impossible
plutôt que déconseillée — voir [WP 12](wp/12-quality-testing.md).

## 6. Dépendances sortant du front

Ces points ne sont pas dans `web/` mais bloquent des lots. À ouvrir en issues
dès maintenant, ils ont un délai propre.

| Sujet | Bloque | Détail |
|---|---|---|
| Trancher l'authentification des routes ([ADR-008](decisions.md#adr-008--la-clé-dapi-sort-du-navigateur)) | 02, 04, 14 | Aujourd'hui `/market/*` exige `X-API-KEY`, `/price/*` n'exige rien |
| Stabiliser `/openapi.json` | 02 | Réponses typées partout, plus de `dict` nu ; `operationId` explicites pour des noms de hooks lisibles |
| Erreurs normalisées côté API | 02, 03 | Un format d'erreur unique ; aujourd'hui le front fait `res.text()` et affiche la chaîne brute |
| Pricing par lot / progression | 09 | Pricer 50 positions en un `POST` synchrone tiendra mal ; prévoir le streaming ou un job |
| Injection de configuration au runtime | 14 | `VITE_*` est inliné au build — changer d'URL d'API impose aujourd'hui de rebuilder l'image |
| Roadmap quant | 15 | Calibration, AAD, GPU, xVA — voir [`../etc/roadmap.md`](../etc/roadmap.md) |
