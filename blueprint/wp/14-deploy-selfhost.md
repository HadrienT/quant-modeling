# WP 14 — Déploiement auto-hébergé

| | |
|---|---|
| **Dépend de** | [13](13-perf-budget.md), [02](02-api-layer.md) |
| **Bloque** | rien |
| **Branche** | `web/14-deploy` |

## Objectif

Servir la stack depuis le serveur personnel, sans Google Cloud. `cloudbuild.yaml`
et la cible Cloud Run de `web/Dockerfile.prod` deviennent du legacy : on ne
construit pas dessus.

## 1. Le problème de configuration à résoudre en premier

**Vite inline les variables `VITE_*` à la compilation.** Elles ne sont pas lues
au démarrage du conteneur. Trois conséquences immédiates :

1. Changer l'URL de l'API impose de **reconstruire l'image**.
2. `VITE_API_KEY=${API_KEY}` dans `docker-compose.yml` place la clé d'API **en
   clair dans le JavaScript servi au navigateur**.
3. En production `Dockerfile.prod` ne transmet pas cette variable : l'en-tête
   `X-API-KEY` n'est jamais envoyé, et nginx ne l'injecte pas non plus. Les
   routes portant `Depends(require_api_key)` — `/market/*`, `/api/backtest/run` —
   répondraient 401 dès que `API_KEY` est défini côté API.

La clé fuit en développement et ne protège rien en production. La correction
tient en deux points :

- **Configuration au runtime** : le serveur sert un petit `/config.json` (ou
  injecte un `window.__APP_CONFIG__` dans l'`index.html` au démarrage) que
  l'application lit avant son premier appel. Une seule image, plusieurs
  environnements.
- **Suppression de `VITE_API_KEY`** ([ADR-008](../decisions.md#adr-008--la-clé-dapi-sort-du-navigateur)).
  Le navigateur n'authentifie que par JWT ; si une clé serveur-à-serveur reste
  nécessaire, c'est le reverse proxy qui l'ajoute, hors de portée du client.

## 2. Image de production

- Conserver le multi-étage existant (wheel C++ → build Vite → nginx + uvicorn) :
  il est correct. Le durcir : utilisateur non root, `HEALTHCHECK`, versions
  épinglées.
- Retirer du `Dockerfile.prod` les `ARG VITE_*` devenus inutiles une fois la
  configuration passée au runtime.
- Étiqueter l'image avec le SHA du commit ; `COMMIT_SHA` alimente déjà `/health`
  et le pied de page.

## 3. nginx

Le `nginx.conf.template` actuel est déjà correct sur l'essentiel (limitation de
débit par IP, blocage des chemins de scanners, repli SPA, cache des assets). À
compléter :

- **Compression** : `gzip` et, si le module est disponible, `brotli`. Absents
  aujourd'hui — c'est le gain le plus immédiat sur le poids transféré.
- `Cache-Control: no-store` sur `index.html` et `/config.json`, immuable sur
  `/assets/` (déjà en place).
- **En-têtes de sécurité** : `Content-Security-Policy`,
  `X-Content-Type-Options: nosniff`, `Referrer-Policy`, `Permissions-Policy`,
  `Strict-Transport-Security`.

La CSP mérite attention : elle doit autoriser WebGL et les *blob workers* si
three.js en utilise, tout en interdisant les ressources externes. C'est
cohérent avec le choix du [lot 05](05-viz-3d.md) de générer l'environnement
d'éclairage plutôt que de télécharger une HDRI, et avec l'auto-hébergement des
polices ([lot 01](01-design-system.md)). Une CSP qui n'autorise aucune origine
tierce est possible ici — et ça vaut la peine de le viser.

## 4. Compose de production

Un `docker-compose.prod.yml` distinct du fichier de développement :

- Images construites, pas de montage de volume de code, pas de HMR.
- Volumes nommés pour les données persistantes — **y compris le stockage qui
  remplace GCS** pour les comptes et les portefeuilles
  ([WP 04](04-auth-session.md)).
- Reverse proxy en frontal (Caddy ou Traefik) pour TLS automatique, plutôt que
  de gérer les certificats à la main.
- Redémarrage automatique, limites de ressources, rotation des logs — le volume
  `./logs/quantmodeling` grossit sans limite aujourd'hui.
- Ports d'hôte paramétrables : `QM_API_PORT` / `QM_WEB_PORT` sont déjà en place
  parce que plusieurs projets partagent le serveur.

## 5. Sauvegardes

Une fois les données sorties de GCS, plus personne ne les sauvegarde à votre
place. Un `cron` de dump vers un stockage hors machine, et **une restauration
testée au moins une fois** — une sauvegarde jamais restaurée n'est pas une
sauvegarde.

## 6. CI/CD

Le workflow actuel ne construit que le C++. À ajouter : build et publication de
l'image (GHCR), puis déclenchement du déploiement sur le serveur (webhook ou
`pull` périodique). Le rollback consiste à repointer sur l'étiquette
précédente.

## Critères d'acceptation

- [ ] Une seule image sert le développement et la production, l'URL d'API étant
      lue au runtime.
- [ ] `grep -r "API_KEY" dist/` ne renvoie rien.
- [ ] Aucune requête vers un domaine tiers depuis la page (vérifié dans
      l'onglet réseau).
- [ ] CSP active, sans violation en console sur toutes les pages, WebGL compris.
- [ ] gzip/brotli actifs, vérifiés sur les en-têtes de réponse.
- [ ] La stack redémarre proprement après un `reboot` du serveur.
- [ ] Une restauration de sauvegarde a été effectuée avec succès.
- [ ] Aucune identification GCP requise pour faire tourner l'application.

## Pièges

- La CSP casse silencieusement le WebGL si `worker-src` est trop strict :
  tester la page de surface avec la CSP active, pas seulement la page d'accueil.
- `envsubst` dans `entrypoint.sh` remplace **toutes** les variables du modèle :
  échapper les `$` destinés à nginx, sinon `$remote_addr` disparaît.
- Ne pas confondre la variable de build `VITE_COMMIT_SHA` avec la variable de
  runtime `COMMIT_SHA` : les deux existent et servent à des endroits différents.
