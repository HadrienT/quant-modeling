# Runbook — quant-modeling auto-hébergé (blueprint WP 14)

Le site est servi **depuis le serveur perso**, joignable en HTTPS sur
`https://tramonihadrien.com` **via un tunnel Cloudflare** : aucun port
n'est ouvert sur la box, l'IP résidentielle reste masquée, le certificat TLS est
géré par Cloudflare.

```
Navigateur ─HTTPS→ Cloudflare ─tunnel sortant→ conteneur cloudflared ─http→ conteneur quant-modeling
                                                                          (nginx → uvicorn)
                                                                              │
                                                              data-ingest-postgres (prix + macro)
     volumes : qm_data (comptes + portefeuilles, JSON)   ·   qm_logs
```

Tout se passe dans le **worktree dédié** `~/quant-modeling-prod` (branche
`main`), isolé de `~/quant-modeling` où d'autres sessions travaillent.

---

## Le modèle de maintenance (à lire en premier)

Il y a **deux dossiers**, chacun son rôle. Ils partagent le même dépôt git
(`git worktree`) — pas de duplication, presque pas d'espace disque en plus.

| Dossier | Rôle | Ce que tu y fais |
|---|---|---|
| `~/quant-modeling` | **Développement**. Branches, éditions, `npm run dev`, commits, PRs. Les sessions Claude travaillent ici. | tout le dev |
| `~/quant-modeling-prod` | **Le site en ligne, uniquement.** Toujours sur `main`. | `./scripts/deploy.sh` — c'est à peu près tout |

**Pourquoi un dossier séparé ?** Pour que le site en prod ne soit jamais cassé
par une édition de dev en cours. Ce n'est pas plus compliqué : c'est la
séparation dev/prod standard sur une machine. Le conteneur qui tourne est
construit à partir d'une **image**, il ne lit plus les fichiers du dossier une
fois démarré — seul `deploy.sh` s'en sert (pour reconstruire l'image).

### La boucle quotidienne : une commande

```bash
cd ~/quant-modeling-prod && ./scripts/deploy.sh
```

`deploy.sh` fait tout : `git pull` de `main` → reconstruit l'image en
l'étiquetant avec le SHA du commit → `up -d` → attend que `/health` réponde.
Idempotent, tu peux le relancer sans risque.

### D'où vient le nouveau code ?

La réécriture (blueprint WP 00–14) **est fusionnée dans `main`** ; les branches
`web/rewrite` et `web/14-deploy` ont été supprimées. Un seul tronc désormais.

- **Développer** : une branche dans `~/quant-modeling` → commit → `gh pr create`
  vers `main` → merge.
- **Mettre en ligne** : `cd ~/quant-modeling-prod && ./scripts/deploy.sh`.

C'est tout. (La branche `core/dates-conventions`, si elle existe encore, est du
travail C++ séparé qui devra être rebasé sur `main`.)

### Ce qui tourne tout seul

- **Reboot** : `systemd` relance la stack (§5) — rien à faire.
- **Sauvegardes** : le timer `systemd` fait un dump chaque nuit (§5) — rien à
  faire une fois installé.
- **Auto-guérison** : `restart: unless-stopped` sur les conteneurs — si l'app
  ou le tunnel plante, Docker les relance.

### Le seul fichier non versionné : `.env`

`~/quant-modeling-prod/.env` contient les secrets (JWT, mot de passe PG, jeton du
tunnel) et **n'est nulle part dans git**. Garde-en une copie hors du dossier :

```bash
cp ~/quant-modeling-prod/.env ~/quant-modeling-prod-env.bak   # à refaire si tu changes .env
```

Ce n'est pas critique si tu le perds (JWT régénérable, mot de passe PG dans
`~/data-ingest/.env`, jeton récupérable dans le dashboard Cloudflare), juste
pénible.

### Si le worktree part en vrille

Il ne peut pas vraiment : `~/quant-modeling-prod/.git` est juste un petit
fichier qui pointe vers le vrai dépôt. En cas de doute, on le recrée en 2 s
sans toucher au site (qui tourne depuis l'image) :

```bash
git worktree remove --force ~/quant-modeling-prod
git worktree add ~/quant-modeling-prod <branche-de-déploiement>
cp ~/quant-modeling-prod-env.bak ~/quant-modeling-prod/.env
cd ~/quant-modeling-prod && ./scripts/deploy.sh
```

**Deux règles :** ne jamais `rm -rf ~/quant-modeling-prod` (utiliser
`git worktree remove`), et ne pas y faire `git checkout` d'une branche au hasard
(la prod suit UNE branche).

---

## Prérequis serveur (une fois)

- Docker + plugin `docker compose` v2, l'utilisateur dans le groupe `docker`.
- Le réseau docker partagé avec la stack data-ingest :
  ```bash
  docker network inspect dataplatform >/dev/null 2>&1 || docker network create dataplatform
  ```
- La stack `~/data-ingest` up (Postgres des prix/macro). Sans elle, l'app démarre
  quand même mais les pages *Market* affichent une erreur.
- L'accès au **compte Cloudflare qui gère déjà `tramonihadrien.com`** (le domaine
  y est déjà délégué — cf. §2).

---

## §1 — Le worktree

Déjà créé si tu lis ceci depuis `~/quant-modeling-prod`. Pour le recréer :

```bash
cd ~/quant-modeling
git fetch origin
git worktree add ~/quant-modeling-prod main
cd ~/quant-modeling-prod
```

Pour le supprimer proprement un jour : `git worktree remove ~/quant-modeling-prod`
(jamais `rm -rf`).

---

## §2 — Cloudflare : c'est déjà fait, presque rien à toucher

`tramonihadrien.com` est **déjà géré par Cloudflare** (le DNS est délégué). Rien
à faire côté registrar. La zone sert aujourd'hui un site Google Sites (apex +
`www`) et la messagerie Google Workspace (enregistrements `MX`).

**Cible : `tramonihadrien.com` (et `www`) doit afficher l'app quant-modeling.**

### 2.1 — Réglages de la zone (dashboard Cloudflare)

| Réglage | Où | Valeur |
|---|---|---|
| Mode SSL/TLS | SSL/TLS → Overview | **Full (strict)** |
| Always Use HTTPS | SSL/TLS → Edge Certificates | **On** |
| Rocket Loader | Speed → Optimization → Content Optimization | **Off** (casse les SPA) |

### 2.2 — L'ancien site

Les enregistrements qui font pointer `tramonihadrien.com` / `www` vers Google
Sites (4 `A` en `216.239.x` + le `CNAME www → ghs.googlehosted.com`) seront
**remplacés automatiquement** quand tu ajoutes les hôtes publics du tunnel
(§3.3) — Cloudflare te proposera d'écraser l'enregistrement existant, tu
acceptes. **Ne touche pas** aux `MX` (ta messagerie Gmail), au `TXT`
`google-site-verification`, ni au `CNAME _domainconnect`.

---

## §3 — Créer le tunnel (dans le dashboard Cloudflare)

Approche par **jeton** : rien à installer, rien à authentifier en ligne de
commande, aucun fichier de secret à gérer — juste un copier-coller.

### 3.1 — Créer le tunnel

1. <https://one.dash.cloudflare.com> → **Networks → Tunnels → Create a tunnel**.
2. Type **Cloudflared**. Nom : `quant-modeling`. **Save tunnel**.
3. L'écran « Install and run a connector » affiche une commande du type
   `cloudflared service install eyJhI...`. **Copie juste le long jeton** (la
   partie après `install`, `eyJ...`) — pas toute la commande.

### 3.2 — Coller le jeton

Dans `~/quant-modeling-prod/.env` :

```
CLOUDFLARE_TUNNEL_TOKEN=eyJhI...            # le jeton copié
```

### 3.3 — Ajouter les hôtes publics

Toujours dans la page du tunnel, onglet **Public Hostname → Add a public
hostname**, deux fois :

| Subdomain | Domain | Path | Type | URL |
|---|---|---|---|---|
| *(vide)* | `tramonihadrien.com` | *(vide)* | HTTP | `app:8080` |
| `www` | `tramonihadrien.com` | *(vide)* | HTTP | `app:8080` |

Cloudflare crée/écrase les enregistrements DNS tout seul (accepte l'écrasement
de l'ancien `A`/`CNAME`). `app:8080` = le conteneur de l'app, joint par
`cloudflared` sur le réseau docker interne.

> `www` et l'apex montreront la même app. Pour rediriger `www` → apex plus tard :
> **Rules → Redirect Rules**, 30 secondes.

---

## §4 — Premier déploiement

```bash
cd ~/quant-modeling-prod
cp .env.placeholder .env
```

Édite `.env` :

| Variable | Valeur |
|---|---|
| `CLOUDFLARE_TUNNEL_TOKEN` | le jeton copié en §3.1 (`eyJ...`) |
| `JWT_SECRET` | `openssl rand -hex 32` (nouveau) — ou recopie celui de `~/quant-modeling/.env` pour garder les sessions existantes |
| `PGPASSWORD` | **identique** à `~/data-ingest/.env` |
| `QM_WEB_PORT` | `8091` (port de debug local, lié à 127.0.0.1 uniquement) |

(`CORS_ALLOW_ORIGINS` peut rester vide : le défaut couvre `tramonihadrien.com` +
`www`. Le `COMMIT_SHA` est posé par `deploy.sh`. Si le jeton n'est pas encore
là, `deploy.sh` ne lance que `app` — c'est normal, relance-le après.)

Puis :

```bash
docker compose -f docker-compose.prod.yml config >/dev/null   # valide la syntaxe
./scripts/deploy.sh                                            # build + up -d + attente santé
```

Vérifications locales (marchent même avant que le DNS Cloudflare soit actif) :

```bash
curl -sf http://127.0.0.1:8091/health        # {"status":"ok","version":"<sha>"}
curl -s  http://127.0.0.1:8091/config.json   # {"apiBase":"","commitSha":"<sha>"}

# critère WP14 : la clé d'API ne fuit pas dans le bundle
docker compose -f docker-compose.prod.yml exec app \
  sh -c 'grep -rn API_KEY /usr/share/nginx/html || echo "OK — aucune occurrence"'

# le tunnel est connecté
docker compose -f docker-compose.prod.yml logs cloudflared | grep -i "Registered tunnel connection"
```

Une fois la zone Cloudflare active, **depuis n'importe où** :

```bash
curl -sf https://tramonihadrien.com/health
```

Puis ouvre `https://tramonihadrien.com` dans un navigateur et contrôle :

- Console : **aucune violation CSP** (teste aussi la page *Products* et la page
  *surface WebGL*, pas seulement l'accueil).
- Onglet Réseau : **aucune requête vers un domaine tiers**.
- `curl -sI -H 'Accept-Encoding: gzip' https://tramonihadrien.com/ | grep -i content-encoding`
  → `gzip`.

---

## §5 — Démarrage au boot + sauvegardes

### Redémarrage automatique

```bash
sudo cp deploy/quant-modeling.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now quant-modeling.service
sudo systemctl enable docker          # docker lui-même au boot
```

`data-ingest` doit aussi revenir au boot. Si ce n'est pas déjà le cas, crée une
unit analogue pour `~/data-ingest` (même modèle : `Type=oneshot`,
`RemainAfterExit=yes`, `ExecStart=/usr/bin/docker compose up -d`).

### Sauvegarde quotidienne (volume `qm_data`)

```bash
sudo cp deploy/quant-modeling-backup.service deploy/quant-modeling-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now quant-modeling-backup.timer
systemctl list-timers quant-modeling-backup.timer      # prochaine exécution
```

Par défaut ça écrit dans `/var/backups/quant-modeling` (local). **Fais mieux :**
pointe-le vers une **destination hors-machine** (NAS monté, disque USB, cible
rclone) — une sauvegarde sur le même disque ne protège pas d'une panne de disque :

```bash
echo 'BACKUP_DEST=/mnt/nas/quant' | sudo tee /etc/quant-modeling-backup.conf
sudo systemctl restart quant-modeling-backup.service     # test immédiat
```

### Restauration — À TESTER (critère WP 14)

```bash
# 1. fabrique une sauvegarde
./scripts/backup_data.sh /tmp/qmbak

# 2. restaure-la dans un volume jetable et inspecte (les données live ne bougent pas)
./scripts/restore_data.sh /tmp/qmbak/qm_data-$(date +%F).tgz --into scratch
docker volume rm qm_data_restore_check      # nettoyage

# 3. (le jour d'un vrai incident) restauration en place :
#    ./scripts/restore_data.sh /mnt/offsite/quant/qm_data-AAAA-MM-JJ.tgz
```

---

## §6 — Test reboot

```bash
sudo reboot
# … au retour, SANS rien lancer à la main :
curl -sf https://tramonihadrien.com/health
```

Si ça répond `ok`, les critères « redémarre proprement après reboot » et
« aucune action manuelle » sont validés.

---

## §7 — Durcissement Cloudflare (recommandé)

Dans le dashboard de la zone :

- **Security → WAF → Managed rules** : activer le jeu gratuit.
- **Security → Bots → Bot Fight Mode** : On.
- **Security → WAF → Rate limiting rules** : une règle sur
  `URI Path contains /api/auth/` → *Block* au-delà de ~10 req/min par IP.
- **Caching → Configuration** : *Browser Cache TTL = Respect Existing Headers*
  (nginx pose déjà `immutable` sur `/assets/` et `no-store` sur `index.html` /
  `/config.json`).

---

## Opérations courantes

Depuis `~/quant-modeling-prod`. `dc` = `docker compose -f docker-compose.prod.yml`.

| Tâche | Commande |
|---|---|
| **Déployer une mise à jour** | `./scripts/deploy.sh` (fait le `git pull` lui-même) |
| Voir l'état | `dc ps` |
| Logs de l'app | `dc logs -f app` |
| Logs du tunnel | `dc --profile tunnel logs -f cloudflared` |
| Santé (local / public) | `curl -s http://127.0.0.1:8091/health` · `curl -s https://tramonihadrien.com/health` |
| Rollback | `git switch --detach <sha-précédent> && ./scripts/deploy.sh` (revenir ensuite : `git switch main`) |
| Redémarrer | `sudo systemctl restart quant-modeling.service` |
| Arrêter (temporaire) | `dc --profile tunnel down` — systemd le relancera au prochain boot ou `start` |
| Changer d'adresse publique | dashboard Cloudflare → tunnel `quant-modeling` → Public Hostname ; puis `dc --profile tunnel up -d` |
| Mettre à jour l'image cloudflared | changer le tag dans `docker-compose.prod.yml`, `./scripts/deploy.sh` |

---

## Dépannage

| Symptôme | Piste |
|---|---|
| `https://…` → 502 / 1033 | Le conteneur `app` n'est pas *healthy* ou `cloudflared` ne le joint pas. `docker compose -f docker-compose.prod.yml ps`, puis les logs des deux. |
| Boucle de redirection HTTPS | Mode SSL/TLS Cloudflare sur *Flexible* → passer à **Full (strict)**. |
| Page blanche / JS non exécuté | **Rocket Loader** encore activé → Off. Vider le cache Cloudflare (Caching → Purge Everything). |
| Violations CSP dans la console | Une ressource externe s'est glissée dans le build. La CSP interdit toute origine tierce — c'est voulu (blueprint WP 14 §3). |
| `cloudflared` refuse de démarrer | `CLOUDFLARE_TUNNEL_TOKEN` absent/mauvais dans `.env`. Recopie le jeton depuis la page du tunnel (§3.1). |
| `https://…` → erreur 1016 / DNS | l'hôte public n'est pas ajouté dans le tunnel (§3.3), ou le DNS de l'ancien site n'a pas été écrasé. |
| Market en erreur, le reste OK | `data-ingest-postgres` down ou pas sur le réseau `dataplatform`. |
| `deploy.sh` : `.env is missing` | `cp .env.placeholder .env` puis remplir. |
| Le port 8091 est pris | changer `QM_WEB_PORT` dans `.env` (n'importe quel port libre en 127.0.0.1). |
