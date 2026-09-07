# WP 00 — Fondations & outillage

| | |
|---|---|
| **Dépend de** | rien |
| **Bloque** | 01, 02, 12 — donc tout |
| **Branche** | `web/00-foundations` |

## Objectif

Poser l'ossature dans laquelle tous les autres lots viendront se ranger, sans
écrire une seule ligne de produit. À la fin de ce lot l'application affiche
toujours l'ancien front : rien n'est visible, tout est différent en dessous.

## Périmètre

**Dedans** : versions et outils, arborescence, configuration TypeScript,
Tailwind, lint et format, CI front, conteneurisation de dev.
**Dehors** : tout composant visuel (→ [01](01-design-system.md)), tout appel
réseau (→ [02](02-api-layer.md)).

## Tâches

### 1. Socle de dépendances

- Monter **Vite** et **React 19** aux dernières majeures ; épingler la version
  de Node dans `.nvmrc` et dans les deux `Dockerfile` (aujourd'hui `node:20`).
- `package.json` : ajouter `engines.node` et `packageManager` pour que la CI,
  le conteneur et la machine du mainteneur ne divergent pas.
- Retirer `plotly.js`, `react-plotly.js` et leurs `@types` **à la fin** du lot
  08, pas maintenant — l'ancien front en dépend encore.

### 2. TypeScript intransigeant

Le `tsconfig.json` actuel a `strict: true` et s'arrête là. Ajouter :

```jsonc
"noUncheckedIndexedAccess": true,   // z[i][j] devient number | undefined
"exactOptionalPropertyTypes": true, // { a?: number } ≠ { a: number | undefined }
"noImplicitOverride": true,
"noFallthroughCasesInSwitch": true,
"verbatimModuleSyntax": true
```

`noUncheckedIndexedAccess` compte particulièrement ici : le code manipule des
grilles `number[][]` avec des trous, et c'est exactement la classe de bug que
ce drapeau attrape.

Alias de chemins `@/*` → `src/*` (dans `tsconfig.json` **et** `vite.config.ts`,
les deux sont nécessaires).

### 3. Arborescence

```
web/src/
├── app/                    # composition : routeur, providers, layout
│   ├── routes/
│   ├── providers/
│   └── layout/
├── features/               # une feature = un dossier, aucune ne connaît les autres
│   ├── pricing/
│   ├── market/
│   ├── portfolio/
│   ├── strategies/
│   └── backtest/
├── shared/                 # ne connaît aucune feature
│   ├── api/                # schema.gen.ts, client, queryKeys, hooks
│   ├── ui/                 # composants shadcn + primitives maison
│   ├── viz/                # moteurs de rendu 2D et 3D
│   ├── products/           # catalogue produit (voir dependencies.md §3.4)
│   ├── payoff/             # fonctions pures de payoff
│   ├── format/             # formatage nombres, devises, dates, pourcentages
│   ├── hooks/
│   └── styles/
└── main.tsx
```

Le sens de dépendance `shared ← features ← app` est
[expliqué ici](../dependencies.md#5-le-sens-de-dépendance-à-ne-jamais-inverser)
et rendu obligatoire par lint au lot 12.

### 4. Tailwind v4

- `@tailwindcss/vite`, configuration CSS-first : les tokens sont déclarés dans
  `shared/styles/theme.css` via `@theme`, pas dans un fichier JS.
- `tailwind-merge` + `clsx` derrière un helper `cn()` — la convention shadcn.
- Initialiser shadcn/ui (`components.json`), en pointant l'alias sur
  `@/shared/ui`. **Ne pas ajouter les composants maintenant**, ils viennent au
  fil des besoins dans le lot 01.

### 5. Lint, format, et le rendre bloquant

- `prettier` avec `prettier-plugin-tailwindcss` (tri des classes utilitaires).
  Le repo est en tabulations — conserver.
- eslint : ajouter `eslint-plugin-import` (ordre et restrictions de chemins),
  `eslint-plugin-jsx-a11y`, `eslint-plugin-testing-library`.
- **Corriger les 11 erreurs eslint existantes** dans `Market.tsx` et
  `usePricing.ts` — pas par confort, parce qu'un lint rouge tolérable rend le
  lint inutile pour tout le monde ensuite. Les fichiers seront supprimés plus
  tard ; en attendant ils doivent passer.
- `npm run lint` doit sortir en code 0 avec zéro avertissement.

### 6. Outillage de test installé (mais pas encore utilisé)

Vitest + `@testing-library/react` + `jsdom` + MSW v2 + Playwright + Storybook.
Un test bidon vert dans chaque harnais, pour prouver que la configuration
fonctionne. Le contenu réel est au [lot 12](12-quality-testing.md).

### 7. CI

Nouveau job `web` dans `.github/workflows/ci.yml`, avec cache npm :

```
npm ci → tsc --noEmit → eslint → vitest run → vite build
```

Le workflow actuel ne teste **que** le C++ : un front cassé passe la CI.

### 8. Conteneur de développement

`web/Dockerfile` fait `COPY web/ ./` après le `npm ci`, ce qui invalide le
cache de couche à chaque changement de source alors que le volume monte de
toute façon le code par-dessus. Simplifier, et vérifier que le HMR fonctionne
à travers le montage (`CHOKIDAR_USEPOLLING` est déjà là).

## Critères d'acceptation

- [ ] `npx tsc --noEmit` passe avec les cinq drapeaux ajoutés.
- [ ] `npm run lint` : 0 erreur, 0 avertissement.
- [ ] `npm run test`, `npm run build`, `npm run storybook` réussissent.
- [ ] La CI GitHub échoue si l'on introduit volontairement une erreur de type
      dans `web/` (vérifié par un commit jetable).
- [ ] `docker compose up` sert l'application, HMR compris.
- [ ] L'application affiche exactement la même chose qu'avant le lot.

## Pièges

- **Tailwind v4 n'est pas Tailwind v3.** Plus de `tailwind.config.js`, plus de
  `@tailwind base`. Toute recette trouvée en ligne datée de v3 est à traduire.
- **React 19 + `@types/react`** : vérifier que `react-plotly.js`, encore
  présent, ne casse pas le typecheck. Si c'est le cas, `skipLibCheck` reste
  activé — ne pas monter le mur maintenant, Plotly part au lot 08.
- Ne pas céder à la tentation d'améliorer un composant existant « au passage ».
  Ce lot ne touche pas au produit.
