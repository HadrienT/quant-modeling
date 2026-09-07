# WP 03 — Shell applicatif

| | |
|---|---|
| **Dépend de** | [01](01-design-system.md) (dure), [02](02-api-layer.md) (douce) |
| **Bloque** | 04, 07, 08, 09, 10, 11 — toutes les pages |
| **Branche** | `web/03-app-shell` |

## Objectif

La coquille dans laquelle les pages se montent : routage typé, mise en page,
navigation, thème, notifications, gestion des erreurs et des chargements. Une
page ne doit jamais avoir à réinventer un de ces éléments.

## Tâches

### 1. Routage typé

TanStack Router ([ADR-007](../decisions.md#adr-007--tanstack-router)). Ce qu'on
en attend concrètement :

- Routes en fichiers sous `app/routes/`, chargées en `lazy` par route.
- **Search params validés par schéma zod et typés de bout en bout.** C'est le
  cœur de l'affaire : une session de pricing, un ticker et un onglet de surface,
  une configuration de backtest deviennent des URL qu'on partage et qu'on met
  en favori. L'application actuelle perd tout état au rechargement.
- Préchargement au survol des liens de navigation.
- Redirection `/` → `/visualize` conservée pour ne pas casser les liens
  existants ; les chemins actuels (`/market`, `/price`, `/portfolio`,
  `/backtest`, `/about`) sont préservés.

### 2. Mise en page

- Barre de navigation dense, alignée à gauche, thème + session à droite. La
  « pilule » flottante actuelle avec `backdrop-filter` mange de la hauteur
  utile et floute ce qui passe dessous — un tableau de nombres devient illisible
  en défilant.
- La marque **navigue** vers `/` ; aujourd'hui elle fait
  `window.location.reload()`, ce qui recharge toute l'application.
- Densité en largeur pleine avec une largeur maximale de lecture pour le texte
  seulement. Un tableau de positions ou une surface doivent prendre l'écran.
- Pied de page : version de build (`VITE_COMMIT_SHA`), lien vers le dépôt, et
  état de l'API (`/health`) — un point vert/rouge, la seule chose qui répond
  vraiment à « pourquoi rien ne charge ».

### 3. Palette de commandes ⌘K

`cmdk` via le composant `command` de shadcn. Pas un gadget : c'est le moyen de
naviguer vite dans un outil à sept écrans et cent produits.

Entrées : aller à une page, rechercher un ticker, sélectionner un produit à
pricer, basculer le thème, ouvrir un portefeuille récent, copier l'URL de l'état
courant.

### 4. Thème

- `data-theme` sur `<html>`, sombre par défaut, `prefers-color-scheme` au
  premier chargement, choix explicite persisté ensuite.
- **Script bloquant en tête de `index.html`** pour poser l'attribut avant le
  premier rendu — sinon on voit un flash clair à chaque chargement.
- Notifier le pont WebGL du [lot 01](01-design-system.md#8-le-pont-vers-webgl).

### 5. États : chargement, vide, erreur

Aujourd'hui il n'y a rien de tout ça. Il faut les trois, comme composants
partagés, avec des stories :

- **Chargement** : squelettes qui ont la forme du contenu attendu, pas un
  spinner centré. Une table charge des lignes grises, une surface charge un
  cadre à la bonne taille — sinon la page saute quand la donnée arrive.
- **Vide** : distinguer « aucun résultat pour ce filtre » de « rien à afficher
  encore » de « il faut se connecter ». Chacun avec l'action qui débloque.
- **Erreur** : frontières d'erreur par route, message issu du type `ApiError`
  du [lot 02](02-api-layer.md), bouton *Réessayer* branché sur l'invalidation
  de la requête, détail technique repliable. Une erreur dans un graphique ne
  doit pas emporter la page.

### 6. Notifications

`sonner` : réservé aux résultats d'action utilisateur (portefeuille
enregistré, position supprimée, backtest terminé). **Pas** pour les erreurs de
chargement — celles-ci s'affichent là où la donnée manque, pas dans un coin.

### 7. Raccourcis clavier

`⌘K` palette, `⌘/` aide, `Escape` ferme, `g` puis `p` navigation rapide. Une
feuille de raccourcis dans l'aide. Tous les dialogues restituent le focus.

## Fichiers supprimés à l'issue du lot

- [`web/src/App.tsx`](../../web/src/App.tsx) — remplacé par `app/`
- Les sections de navigation, `auth-*` et `build-footer` de
  [`web/src/styles.css`](../../web/src/styles.css)

## Critères d'acceptation

- [ ] Toutes les routes accessibles au clavier seul, focus visible partout.
- [ ] Aucun flash de thème clair au chargement en thème sombre.
- [ ] Une erreur simulée dans une page affiche la frontière d'erreur, pas un
      écran blanc.
- [ ] Les URL portent l'état de vue : coller l'URL d'un collègue reproduit son
      écran (testé sur au moins une page).
- [ ] Test axe sans violation sur le shell.
- [ ] Le JS initial (shell seul, hors page) reste sous le budget du
      [lot 13](13-perf-budget.md).

## Pièges

- Ne pas mettre de données dans le shell : le shell affiche des enfants, il ne
  charge rien à part `/health`.
- La palette de commandes doit être chargée en `lazy` — elle embarque un index
  de recherche.
- Attention aux `Suspense` trop hauts : un seul `Suspense` racine fait
  disparaître toute la page quand un widget charge.
