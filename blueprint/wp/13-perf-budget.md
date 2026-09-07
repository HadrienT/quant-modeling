# WP 13 — Performance & budgets

| | |
|---|---|
| **Dépend de** | [12](12-quality-testing.md) — il faut mesurer avant de budgéter |
| **Bloque** | [14](14-deploy-selfhost.md) |
| **Branche** | `web/13-perf` |

## Point de départ

```
dist/assets/index-*.js    5 341 kB  │ gzip: 1 604 kB   ← un seul fichier
dist/assets/index-*.css      51 kB  │ gzip:    13 kB
```

Un chunk unique de 5,3 Mo. Plotly en représente l'essentiel, et il est chargé
même sur les pages qui n'affichent aucun graphique. Vite émet déjà
l'avertissement ; personne ne l'a traité.

## Budgets cibles

| Ressource | Budget (gzip) | Comment on y arrive |
|---|---|---|
| Chunk initial (shell + route d'accueil) | **≤ 180 kB** | React + routeur + shell ; aucune librairie de graphique |
| CSS initial | ≤ 20 kB | Tailwind purgé |
| Chunk par route | ≤ 120 kB | `lazy` par route |
| three.js + R3F (route surface uniquement) | ≤ 250 kB | Chargé à l'affichage de la première surface |
| lightweight-charts | ≤ 60 kB | Routes à séries temporelles |
| visx (uniquement les modules utilisés) | ≤ 80 kB | Imports granulaires, jamais le paquet parapluie |
| KaTeX + polices | chargé à la demande | Uniquement à l'ouverture d'une fiche produit |
| **Total sur la route la plus lourde** | **≤ 550 kB** | à comparer aux 1 604 kB actuels |

Ces budgets sont **vérifiés en CI** avec `size-limit`. Un budget qu'on ne mesure
pas est un vœu.

## Tâches

### 1. Découpage

- `lazy` par route, avec préchargement au survol du lien de navigation.
- Frontières explicites pour les trois moteurs de graphiques : ils ne sont
  jamais dans le chunk commun.
- `manualChunks` pour isoler le socle (React, routeur) du reste — il change
  rarement, il doit rester en cache navigateur longtemps.
- Vérifier avec `rollup-plugin-visualizer` que rien ne remonte par accident
  dans le chunk commun (un `import type` mal écrit suffit).

### 2. Suppressions

- Plotly et ses types au [lot 06](06-viz-2d.md) : c'est le gros du gain.
- Google Fonts au [lot 01](01-design-system.md) : deux `preconnect` et une
  feuille bloquante en moins, et une dépendance réseau tierce en moins.
- Polices auto-hébergées, sous-ensemble latin, `woff2` seul.

### 3. Runtime

- `frameloop="demand"` sur la 3D ([lot 05](05-viz-3d.md)) : c'est autant une
  mesure de performance qu'une mesure d'autonomie de batterie.
- Virtualisation des tables au-delà de cent lignes.
- Surfaces et grandes séries : transférées en tableaux typés autant que
  possible, jamais dupliquées dans l'état React.
- Mémoïsation **mesurée**, pas réflexe. React 19 en fait beaucoup ; un
  `useMemo` qui coûte plus que ce qu'il évite est un bruit de plus à lire.

### 4. Métriques

- Web Vitals (LCP, INP, CLS) collectés et envoyés à un point d'entrée de l'API,
  pas à un service tiers — le projet est auto-hébergé.
- Marques de performance autour des opérations coûteuses : construction de la
  géométrie de surface, valorisation d'un portefeuille, rendu d'une grande
  table.
- Un tableau de bord de perf simple, en interne, alimenté par ces marques.

### 5. Chargement perçu

- Squelettes à la forme du contenu, jamais de spinner plein écran
  ([lot 03](03-app-shell.md)).
- Cadres à taille réservée pour les graphiques : aucun saut de mise en page
  quand la donnée arrive (c'est le CLS).
- Préchargement des données de route au survol, via le routeur.

## Critères d'acceptation

- [ ] `size-limit` en CI, échec au dépassement.
- [ ] Chunk initial sous 180 kB gzip.
- [ ] Aucune librairie de graphique dans le chunk initial (vérifié par
      inspection du rapport de bundle).
- [ ] Aucune requête réseau vers un domaine tiers en production.
- [ ] CLS ≈ 0 sur les pages à graphiques.
- [ ] La 3D ne consomme pas de GPU au repos (mesuré).
- [ ] Le rapport de bundle est archivé à chaque build pour comparaison.

## Pièges

- Un budget fixé sans mesure d'origine est arbitraire : mesurer d'abord,
  budgéter ensuite, et documenter la mesure de départ.
- `import { Something } from "@visx/visx"` annule tout le découpage. Importer
  depuis les sous-paquets.
- Le préchargement au survol peut doubler le trafic sur une barre de navigation
  dense : le limiter aux liens principaux.
