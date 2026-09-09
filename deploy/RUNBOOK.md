# Runbook — quant-modeling auto-hébergé (blueprint WP 14)

Le site est servi **depuis le serveur perso**, joignable en HTTPS sur
`https://quant.tramonihadrien.com` **via un tunnel Cloudflare** : aucun port
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
`web/14-deploy`), isolé de `~/quant-modeling` où d'autres sessions travaillent.

---

## Prérequis serveur (une fois)

- Docker + plugin `docker compose` v2, l'utilisateur dans le groupe `docker`.
- Le réseau docker partagé avec la stack data-ingest :
  ```bash
  docker network inspect dataplatform >/dev/null 2>&1 || docker network create dataplatform
  ```
- La stack `~/data-ingest` up (Postgres des prix/macro). Sans elle, l'app démarre
  quand même mais les pages *Market* affichent une erreur.
- Un compte Cloudflare (gratuit) — créé à l'étape §2.

---

## §1 — Le worktree

Déjà créé si tu lis ceci depuis `~/quant-modeling-prod`. Pour le recréer :

```bash
cd ~/quant-modeling
git fetch origin
git worktree add -b web/14-deploy ~/quant-modeling-prod web/rewrite
cd ~/quant-modeling-prod
```

Pour le supprimer proprement un jour : `git worktree remove ~/quant-modeling-prod`
(jamais `rm -rf`).

---

## §2 — Mettre `tramonihadrien.com` sur Cloudflare

Cloudflare doit gérer le DNS du domaine pour que le tunnel fonctionne. C'est
gratuit et réversible.

1. Crée un compte sur <https://dash.cloudflare.com/sign-up>.
2. **Add a site** → tape `tramonihadrien.com` → choisis le plan **Free**.
3. Cloudflare scanne tes enregistrements DNS actuels puis affiche **deux serveurs
   de noms**, du type :
   ```
   dana.ns.cloudflare.com
   rick.ns.cloudflare.com
   ```
4. Va chez le **registrar** où tu as acheté le domaine (OVH, Gandi, Namecheap,
   Google Domains / Squarespace…). Si tu ne sais plus lequel :
   `whois tramonihadrien.com` (ou <https://lookup.icann.org>) → ligne *Registrar*.
5. Dans l'espace du registrar, cherche **« Serveurs DNS »**, **« Nameservers »**
   ou **« Gérer les DNS »**, choisis **« serveurs DNS personnalisés »** et
   **remplace** les serveurs existants par les deux de Cloudflare. Enregistre.
6. Retour sur Cloudflare : **Check nameservers**. La propagation prend de
   quelques minutes à ~24 h. Cloudflare envoie un mail **« … is now active »**.

Une fois la zone active, règle dans le dashboard Cloudflare :

| Réglage | Où | Valeur |
|---|---|---|
| Mode SSL/TLS | SSL/TLS → Overview | **Full (strict)** |
| Always Use HTTPS | SSL/TLS → Edge Certificates | **On** |
| Rocket Loader | Speed → Optimization → Content Optimization | **Off** (casse les SPA) |
| Auto Minify | idem | Off (ou absent, retiré par CF en 2024) |

---

## §3 — Créer le tunnel (`cloudflared`)

On utilise l'image docker pour ne rien installer sur l'hôte. Les fichiers
produits restent dans `~/quant-modeling-prod/cloudflared/` ; seul `config.yml`
est versionné, `cert.pem` et `*.json` sont des **secrets gitignorés**.

```bash
cd ~/quant-modeling-prod
CFD="docker run --rm -it -v $PWD/cloudflared:/home/nonroot/.cloudflared cloudflare/cloudflared:2026.8.3"

# 1. Lie cloudflared à ton compte (ouvre un navigateur : choisis la zone tramonihadrien.com)
$CFD tunnel login                       # → cloudflared/cert.pem

# 2. Crée le tunnel nommé "quant-modeling"
$CFD tunnel create quant-modeling       # → cloudflared/<UUID>.json  + affiche l'UUID

# 3. Renomme le fichier d'identifiants pour matcher config.yml
mv cloudflared/*.json cloudflared/quant-modeling.json

# 4. Crée l'enregistrement DNS (CNAME proxifié quant → <UUID>.cfargotunnel.com)
$CFD tunnel route dns quant-modeling quant.tramonihadrien.com

# 5. Vérifie
$CFD tunnel list
```

> Si un jour tu veux servir aussi l'apex : `... route dns quant-modeling tramonihadrien.com`
> puis ajoute la `hostname:` correspondante dans `cloudflared/config.yml`.

`cloudflared/config.yml` (déjà dans le repo) mappe
`quant.tramonihadrien.com → http://app:8080`. `app` est le nom du service dans
`docker-compose.prod.yml`, résolu sur le réseau docker interne.

Rends les fichiers lisibles par le conteneur (utilisateur non-root) :
```bash
chmod 644 cloudflared/config.yml cloudflared/quant-modeling.json
```

---

## §4 — Premier déploiement

```bash
cd ~/quant-modeling-prod
cp .env.placeholder .env
```

Édite `.env` :

| Variable | Valeur |
|---|---|
| `JWT_SECRET` | `openssl rand -hex 32` (nouveau) — ou recopie celui de `~/quant-modeling/.env` pour garder les sessions existantes |
| `PGPASSWORD` | **identique** à `~/data-ingest/.env` |
| `QM_WEB_PORT` | `8091` (port de debug local, lié à 127.0.0.1 uniquement) |
| `PUBLIC_ORIGIN` | `https://quant.tramonihadrien.com` |

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
curl -sf https://quant.tramonihadrien.com/health
```

Puis ouvre `https://quant.tramonihadrien.com` dans un navigateur et contrôle :

- Console : **aucune violation CSP** (teste aussi la page *Products* et la page
  *surface WebGL*, pas seulement l'accueil).
- Onglet Réseau : **aucune requête vers un domaine tiers**.
- `curl -sI -H 'Accept-Encoding: gzip' https://quant.tramonihadrien.com/ | grep -i content-encoding`
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

Choisis une **destination hors-machine** (NAS monté, disque USB, cible rclone) —
une sauvegarde sur le même disque ne protège de rien.

```bash
echo 'BACKUP_DEST=/mnt/offsite/quant' | sudo tee /etc/quant-modeling-backup.conf
sudo cp deploy/quant-modeling-backup.service deploy/quant-modeling-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now quant-modeling-backup.timer
systemctl list-timers quant-modeling-backup.timer      # prochaine exécution
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
curl -sf https://quant.tramonihadrien.com/health
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

| Tâche | Commande (depuis `~/quant-modeling-prod`) |
|---|---|
| Déployer une mise à jour | `git pull --ff-only && ./scripts/deploy.sh` |
| Voir l'état | `docker compose -f docker-compose.prod.yml ps` |
| Logs applicatifs | `docker compose -f docker-compose.prod.yml logs -f app` |
| Logs tunnel | `docker compose -f docker-compose.prod.yml logs -f cloudflared` |
| Santé | `curl -s http://127.0.0.1:8091/health` |
| Rollback | `git checkout <sha-précédent> -- . && COMMIT_SHA=<sha> docker compose -f docker-compose.prod.yml up -d --build` (ou `git switch -d <sha>` puis `./scripts/deploy.sh`) |
| Redémarrer le stack | `sudo systemctl restart quant-modeling.service` |
| Arrêter | `docker compose -f docker-compose.prod.yml down` (systemd le relancera au prochain boot) |
| Changer le hostname public | éditer `cloudflared/config.yml` + `PUBLIC_ORIGIN` dans `.env`, `... route dns quant-modeling <nouveau>`, `docker compose -f docker-compose.prod.yml up -d` |

---

## Dépannage

| Symptôme | Piste |
|---|---|
| `https://…` → 502 / 1033 | Le conteneur `app` n'est pas *healthy* ou `cloudflared` ne le joint pas. `docker compose -f docker-compose.prod.yml ps`, puis les logs des deux. |
| Boucle de redirection HTTPS | Mode SSL/TLS Cloudflare sur *Flexible* → passer à **Full (strict)**. |
| Page blanche / JS non exécuté | **Rocket Loader** encore activé → Off. Vider le cache Cloudflare (Caching → Purge Everything). |
| Violations CSP dans la console | Une ressource externe s'est glissée dans le build. La CSP interdit toute origine tierce — c'est voulu (blueprint WP 14 §3). |
| `cloudflared` : `tunnel credentials file not found` | `cloudflared/quant-modeling.json` absent ou mal nommé, ou pas `chmod 644`. Re-voir §3. |
| Market en erreur, le reste OK | `data-ingest-postgres` down ou pas sur le réseau `dataplatform`. |
| `deploy.sh` : `.env is missing` | `cp .env.placeholder .env` puis remplir. |
| Le port 8091 est pris | changer `QM_WEB_PORT` dans `.env` (n'importe quel port libre en 127.0.0.1). |
