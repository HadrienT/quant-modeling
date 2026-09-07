# WP 02 — Couche API & données

| | |
|---|---|
| **Dépend de** | [00](00-foundations.md), et côté serveur des décisions listées au §6 |
| **Bloque** | 03 (douce), 04, 07, 08, 09, 11, 14 |
| **Branche** | `web/02-api-layer` |

## Objectif

Remplacer [`web/src/api/client.ts`](../../web/src/api/client.ts) — 459 lignes de
`fetch` manuel, de types recopiés et d'en-têtes dupliqués — par une couche où
les types viennent du serveur, où le cache est explicite, et où une erreur a
toujours la même forme.

## Ce qui ne va pas aujourd'hui

- Le helper `headers()` existe **et n'est utilisé que par la moitié des
  fonctions** : `priceVanilla`, `getMarketHistory`, `getTickers`,
  `getIVSurface`, `getRatesCurve`, `getCleanedIVSurface` et
  `getLocalVolSurface` reconstruisent chacune leur objet d'en-têtes à la main.
  Sept copies de trois lignes, avec des différences — `priceVanilla` n'envoie
  ni la clé d'API ni le jeton.
- Les types (`IVSurfaceResponse`, `PricingResponse`, `Portfolio`…) sont écrits
  à la main. Rien ne les confronte aux modèles Pydantic.
- `API_BASE` vaut `http://localhost:8000` par défaut alors que le compose
  injecte `/api` : la valeur par défaut n'est jamais la bonne configuration.
- Gestion d'erreur : `res.text()` puis `new Error(texte)`. Le détail JSON de
  FastAPI est affiché brut à l'utilisateur.
- Aucune annulation. Changer de ticker trois fois lance trois requêtes dont
  la dernière arrivée gagne — pas forcément la dernière demandée.

## Tâches

### 1. Types générés

- `openapi-typescript` → `shared/api/schema.gen.ts`, **commité**
  ([ADR-005](../decisions.md#adr-005--types-générés-depuis-lopenapi-avec-test-de-dérive)).
- Script `npm run api:types` qui interroge le `/openapi.json` d'une API locale.
- Job CI `api-contract` : démarrer l'API, régénérer, `git diff --exit-code`.
  Le message d'échec doit dire quoi faire, pas seulement échouer.
- Le fichier généré est en `.gitattributes` `linguist-generated` et exclu du
  lint.

### 2. Client typé

`shared/api/client.ts`, une seule fonction de requête :

- Base d'URL lue depuis la configuration **runtime** (voir
  [WP 14](14-deploy-selfhost.md)), plus depuis `import.meta.env`.
- Jeton injecté à un seul endroit.
- `AbortSignal` propagé systématiquement.
- Timeout par défaut, plus long et configurable pour le pricing Monte-Carlo
  (une VaR sur un portefeuille peut être longue).
- Erreurs normalisées en un type `ApiError { status, code, message, detail? }` :
  parse le `{detail: ...}` de FastAPI, distingue réseau / 4xx / 5xx / abandon,
  et fournit un message présentable **plus** le détail technique pour les logs.

### 3. TanStack Query

- `QueryClient` avec des valeurs par défaut assumées : pas de `refetchOnWindowFocus`
  sur des données de marché coûteuses, `staleTime` par famille de données
  (surface de vol : minutes ; prix : à la demande ; portefeuille : après
  mutation).
- **`shared/api/queryKeys.ts`** — fabrique hiérarchique, jamais de tableau
  littéral dispersé
  ([interdépendances §3.3](../dependencies.md#33-les-clés-de-cache--sharedapiquerykeysts--wp-02)).
- Un hook par opération, nommé d'après le domaine et non d'après la route :
  `useIvSurface`, `usePriceOption`, `usePortfolio`, `useRunBacktest`.
- Invalidations écrites **avec** la mutation qui les provoque, jamais dans le
  composant appelant.

### 4. Mocks issus du même schéma

MSW v2, handlers construits sur `schema.gen.ts` pour qu'un mock ne puisse pas
mentir sur la forme. Ils servent trois usages : Storybook, tests de composant,
et développement du front sans API démarrée.

Des fixtures **réalistes**, pas `{ npv: 42 }` : une surface IV avec ses trous,
un portefeuille de dix positions hétérogènes, un backtest avec avertissements.
Une bonne fixture est celle qui aurait attrapé un bug.

### 5. Suppression

`web/src/api/client.ts` disparaît quand la dernière page a migré. Jusque-là il
reste, mais **aucun nouveau code ne l'importe**.

## Critères d'acceptation

- [ ] La CI échoue si un modèle Pydantic change sans régénération des types.
- [ ] Aucun `fetch(` en dehors de `shared/api/` (règle eslint).
- [ ] Une requête en vol est annulée quand son composant est démonté ou quand
      son paramètre change (test explicite).
- [ ] Une erreur 500 de l'API produit un message lisible, pas un JSON brut.
- [ ] Storybook et les tests tournent sans API démarrée.
- [ ] `VITE_API_KEY` n'apparaît nulle part dans le bundle de production
      (vérifié par un `grep` sur `dist/`).

## Pièges

- **Ne pas ré-encapsuler TanStack Query** dans un « `useApi()` maison » : c'est
  le réflexe qui reproduit exactement le problème actuel, une couche de plus
  qui masque ce que fait la vraie librairie.
- `staleTime: 0` partout annule l'intérêt du cache ; `staleTime: Infinity`
  affiche des données périmées sans le dire. Choisir par famille, et l'écrire
  en commentaire à côté de la valeur.
- Les surfaces sont volumineuses : ne pas les sérialiser dans un cache
  persistant naïvement.

## 6. Décisions serveur qui bloquent ce lot

À ouvrir en issues immédiatement, elles ont leur propre délai.

1. **Authentification des routes** — trancher entre publique et JWT, route par
   route, et supprimer `VITE_API_KEY`
   ([ADR-008](../decisions.md#adr-008--la-clé-dapi-sort-du-navigateur)).
   L'état actuel est incohérent : `/market/*` et `/api/backtest/run` exigent
   `X-API-KEY`, `/price/*` n'exige rien.
2. **`operationId` explicites** sur chaque route — sinon les noms générés sont
   du bruit du type `price_option_vanilla_price_option_vanilla_post`.
3. **Réponses typées partout.** `/health` renvoie un `dict` nu ; tout ce qui
   n'a pas de `response_model` est un `unknown` côté front.
4. **Format d'erreur unique**, avec un code machine en plus du message humain.
5. **Pricing par lot** : `POST /api/portfolios/{id}/price` price toutes les
   positions en synchrone. Sur cinquante positions Monte-Carlo, cela dépassera
   le timeout. Prévoir la progression ou un job — bloque
   [WP 09](09-portfolio-risk.md).
