# WP 04 — Auth & session

| | |
|---|---|
| **Dépend de** | [03](03-app-shell.md), [02](02-api-layer.md) |
| **Bloque** | 09 (un portefeuille serveur exige une session) |
| **Branche** | `web/04-auth-session` |

## Objectif

Une session utilisable au clavier, honnête sur son expiration, et qui ne
dégrade pas l'expérience des visiteurs non connectés. Accessoirement : sortir
le stockage des comptes de Google Cloud Storage, parce que sans ça
l'auto-hébergement ne marche pas.

## Ce qu'il y a aujourd'hui

`AuthProvider` ([`web/src/auth/AuthContext.tsx`](../../web/src/auth/AuthContext.tsx))
tient `token` et `username` en `useState`, persiste le jeton dans
`localStorage`, et le valide au montage via `getMe`. La modale est un `<div>`
dans un portail, sans `role="dialog"`, sans `aria-modal`, sans piège de focus,
sans restitution du focus à la fermeture, et `Escape` ne la ferme pas.

Trois défauts de fond, du plus au moins grave :

1. **Le stockage des comptes est dans GCS.** `api/app/auth.py` écrit
   `auth/users.json` dans un bucket, et `api/app/portfolio_storage.py` y range
   les portefeuilles. Sur un serveur auto-hébergé sans compte GCP, la connexion
   et les portefeuilles ne fonctionnent pas du tout.
2. **`JWT_SECRET` a une valeur par défaut** : `"change-me-in-production"`. Si la
   variable n'est pas fournie, n'importe qui peut forger un jeton valide pour
   n'importe quel utilisateur. À transformer en échec au démarrage.
3. **`GET /api/auth/me` répond 200 avec `username: ""`** quand le jeton est
   invalide, au lieu d'un 401. Le front interprète la chaîne vide comme
   « déconnecté » mais **conserve le jeton invalide** dans `localStorage` :
   il sera renvoyé à chaque requête suivante jusqu'à ce que l'utilisateur se
   reconnecte.

## Tâches front

### 1. Dialogue accessible

Remplacer la modale par le `Dialog` du [lot 01](01-design-system.md) : piège de
focus, `Escape`, restitution du focus au déclencheur, `aria-labelledby`, erreurs
annoncées via `role="alert"`. Connexion et inscription dans le même dialogue,
avec `react-hook-form` + zod (l'API n'impose aucune règle de mot de passe —
c'est à trancher, voir §serveur).

### 2. La session est de l'état serveur

`useMe()` en requête TanStack Query, pas un `useState` dans un contexte. Le
contexte n'expose plus que les actions. Après connexion ou déconnexion, une
seule invalidation remet toute l'application d'aplomb.

### 3. Expiration explicite

Le jeton dure 72 h (`JWT_EXPIRE_HOURS`). Un intercepteur transforme tout 401 en
fin de session : purge du cache utilisateur, notification « session expirée »,
réouverture du dialogue **en conservant le contexte de la page** (on ne perd pas
un formulaire de pricing à moitié rempli parce que le jeton a expiré).

### 4. Mode anonyme préservé

[`localPortfolios.ts`](../../web/src/auth/localPortfolios.ts) reproduit déjà le
contrat de l'API sur `localStorage` — c'est la bonne idée du code actuel, à
conserver telle quelle. Ce qu'il faut y ajouter :

- La **migration** : à la première connexion, proposer d'envoyer les
  portefeuilles locaux vers le compte. Aujourd'hui ils restent orphelins.
- Un bandeau qui dit clairement « ces portefeuilles sont sur cet appareil
  uniquement ».
- Une frontière stricte : les composants de portefeuille parlent à un port
  `PortfolioRepository`, dont il existe deux implémentations (locale, serveur).
  Ils n'ont pas à connaître l'état de la session.

### 5. Routes protégées

Aucune route entièrement fermée : le pricing, le marché, les stratégies et le
backtest restent utilisables sans compte. Seul le portefeuille **serveur**
demande une session, et il retombe sur le mode local plutôt que de bloquer.

## Tâches serveur (bloquantes)

| Sujet | Détail |
|---|---|
| **Sortir de GCS** | Portefeuilles et comptes vers du stockage local : SQLite via SQLAlchemy, ou des fichiers JSON sur un volume monté si l'on veut rester minimal. Le point important est de mettre une interface `Storage` devant, pour que GCS reste possible sans être obligatoire. Concerne aussi [WP 09](09-portfolio-risk.md). |
| **`JWT_SECRET`** | Refuser de démarrer si la variable est absente ou vaut la valeur par défaut. Un secret par défaut est pire qu'un secret manquant : il ne se remarque pas. |
| **`/me` → 401** | Renvoyer 401 sur jeton invalide plutôt que 200 avec un nom vide. |
| **Politique de mot de passe** | Longueur minimale, limitation du débit sur `/login`. `bcrypt` est déjà utilisé pour le hachage, c'est bien ; il n'y a rien contre le bourrage d'identifiants. |
| **Stockage du jeton** | `localStorage` est exfiltrable par XSS. Le remplacer par un cookie `httpOnly` + `SameSite=Lax` est le bon choix ; il impose une politique CSRF et un changement d'API. À arbitrer explicitement — si l'on garde `localStorage`, l'écrire comme un risque accepté, et durcir la CSP en conséquence ([WP 14](14-deploy-selfhost.md)). |

## Critères d'acceptation

- [ ] Parcours complet inscription → connexion → déconnexion au clavier seul.
- [ ] Test axe sans violation sur le dialogue ouvert.
- [ ] Un jeton expiré déclenche une réauthentification sans perdre l'état de la
      page en cours (test e2e).
- [ ] L'API refuse de démarrer sans `JWT_SECRET`.
- [ ] Connexion et portefeuilles fonctionnent sur une machine **sans aucune
      identification GCP**.
- [ ] Les portefeuilles locaux sont proposés à la migration à la première
      connexion.

## Pièges

- Ne pas mettre le jeton dans un store client synchronisé avec le cache : une
  seule source, et c'est le lieu de stockage.
- L'inscription renvoie 201 et l'ouverture de session : ne pas enchaîner un
  `login` derrière un `register`.
- Le mode anonyme doit rester le chemin **par défaut**, pas un mode dégradé. La
  majorité des visiteurs ne créeront jamais de compte.
