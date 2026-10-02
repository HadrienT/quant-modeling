# WP 21 — Taux : multi-courbe, swaps, swaptions, Hull-White calibré

| | |
|---|---|
| **Dépend de** | chantier 0 de la [roadmap](../../etc/roadmap.md) (bootstrap, Levenberg-Marquardt), SABR (chantier 1b) |
| **Débloque** | LSMC sur les bermudans (validation croisée avec le réseau d'ici), hybrides action-taux (issue #86), puis l'exposition (4c) |
| **Branche** | `feat/rates-multi-curve` |

## 1. Multi-courbe

- **OIS** (`market/multi_curve_bootstrap.hpp`, `make_ois_quote`) : la jambe
  capitalisée d'un OIS au comptant vaut `1 − P(T)` sur sa propre courbe, donc un
  OIS au pair suit l'algèbre des obligations au pair et se bootstrappe avec
  `bootstrap_curve` existant.
- **Projection** (`bootstrap_projection_curve`) : FRAs et swaps au pair sur
  l'index, actualisés sur l'OIS. Un pilier par cotation, résolu par bissection
  sur son taux zéro, les piliers précédents fixés ; la courbe re-price chaque
  entrée à 1e-12 (test). Avec des cotations tirées de l'OIS lui-même, on
  retrouve la courbe OIS (limite mono-courbe, test).
- **Extrapolation** : `DiscountCurve` gardait le facteur d'actualisation
  constant hors des piliers, ce qui annule les forwards d'une courbe de
  projection avant son premier pilier (constaté : 1Y re-pricé 10 bp à côté).
  Ajout de `CurveExtrapolation::FlatForward` (forward constant hors piliers),
  **en option** : le comportement historique reste celui par défaut, aucun
  chiffre existant ne bouge. Les courbes de taux de ce lot l'utilisent.
- Interpolation : log-linéaire sur les facteurs d'actualisation (forwards
  constants par morceaux, positifs). L'interpolation monotone convexe
  (Hagan-West) n'est pas faite ; les forwards en escalier se voient sur la page.

## 2. Swaps et swaptions

- `instruments/rates/swap.hpp` : `InterestRateSwap` (périodes explicites, deux
  jambes), `Swaption`, `BermudanSwaption` ; branchés sur le visiteur.
- `engines/analytic/swap.hpp` : valeur multi-courbe, taux par, annuité ;
  Black (décalé), Bachelier, inversion de la vol normale (sur la valeur temps,
  sans cancellation ; au-delà de 8 écarts-types la valeur temps sort du double),
  SABR décalé (Hagan 2002 sur le taux décalé).
- Pas de calendrier ni de base de décompte : les périodes sont en fractions
  d'année régulières. `Schedule` et `DayCounter` existent ; les brancher est
  un ajout d'interface, pas de modèle.

## 3. Hull-White

- `models/rates/hull_white_curve.hpp` : Hull-White 1F ajusté exactement à la
  courbe OIS, écrit en `x = r − f(0,t)` (Andersen & Piterbarg §10.1) ; base
  multiplicative déterministe vers la courbe de projection (Mercurio 2009).
  Transitions gaussiennes exactes sous toute mesure forward.
- **Européenne en forme fermée** (`hull_white_european_swaption`) : intégrale
  exacte de la valeur d'exercice sur la région où le swap vaut > 0, racines
  localisées sur une grille puis polies ; Jamshidian en est le cas à une racine.
  Tests : égale à la somme de puts zéro-coupon de Brigo-Mercurio à 1e-12,
  égale à un Monte-Carlo à 4 écarts-types en multi-courbe.
- **Bermudan** (`hull_white_bermudan_swaption`) : induction arrière sur une
  grille en x sous la mesure terminale, interpolation linéaire, espérance de
  chaque morceau linéaire contre la transition gaussienne **en forme fermée**.
  Tests : une date d'exercice = l'européenne (1,5e-4), ordre 2 en pas de grille
  mesuré, bermudan > chaque européenne co-terminale. **Validé contre
  QuantLib 1.43** (arbre trinomial, 800 pas) : rapport bermudan / européenne
  1,4393 contre 1,4385 sur un 2Y×8Y, a = 3 %, σ = 1 %.
- **Calibration** (`market/hull_white_calibration.hpp`) : (a, σ) par
  Levenberg-Marquardt sur des vols normales ATM, résidus en bp de vol ; a
  peut être fixé. Test : aller-retour exact sur des vols générées par le modèle.

## 4. API et écrans

Il n'y a plus de page `/rates` (issue #143) : elle mêlait données de marché,
construction de courbes et valorisation de deux produits, et son entrée de
menu doublait l'onglet Rates de la page Market. Elle avait été faite d'un bloc
quand aucune cotation de swap n'était gratuite ; depuis l'ingestion DTCC, ce
motif ne vaut plus que pour l'exemple EUR. Son contenu est réparti par nature,
et `/rates` redirige vers le swap du workbench.

| Quoi | Où |
|---|---|
| Courbe de swaps SOFR et vols de swaptions tirées des transactions, avec le nombre de transactions et ce qui est écarté | Market → Rates, devise USD (`features/market/SwapMarketPanel.tsx`) |
| Swap, swaption européenne et bermudane | Pricing, famille « Fixed income » (`features/pricing/rates/`) |
| Payoff, hypothèses, modèle minimal, sources | Products, fiches `swap` et `swaption` (`shared/products/docs.ts`), avec un profil de risque par bump des cotations |

**API.**

- `GET /api/rates/quotes/{set_id}` : un jeu de cotations (`usd-sofr` ou
  `eur-illustrative`), les contrats sur lesquels le workbench s'ouvre, et d'où
  viennent les chiffres.
- `POST /api/rates/curves` : les deux courbes d'un jeu de cotations, la base
  entre leurs forwards, l'erreur de repricing.
- `POST /price/rates/swap` et `POST /price/rates/swaption` : passent par
  `valuation.PRODUCTS` (`interest_rate_swap`, `swaption`), donc chaque
  valorisation émet un `pricing.valuation` et se rejoue.

**Un prix est une fonction des cotations que porte sa requête.** Le workbench
charge un jeu, l'utilisateur peut l'éditer, et la requête de pricing emporte
les cotations elles-mêmes : rien ne lit la base pendant le pricing. Le replay
n'a donc besoin que de la requête enregistrée, et un jeu édité se rejoue comme
un jeu de marché.

**Deux jeux de cotations.** `usd-sofr` est le défaut : des données de marché
(médianes de transactions DTCC, `api/app/swaption_market.py`). S'il manque ou date de plus de sept
jours, c'est une erreur qui dit ce qui manque, jamais une bascule silencieuse
vers l'autre jeu. `eur-illustrative` (€STR, EURIBOR 6M) reste, marqué « Manual
input » : aucune source gratuite ne publie de courbe de swaps IBOR, et c'est
le seul jeu où la courbe de projection diffère de la courbe d'actualisation.
En USD l'indice est le taux au jour le jour : une seule courbe, base nulle, et
l'écran le dit.

**Choix du modèle de la swaption (ADR-S8 du WP 16, appliqué aux taux).** Par
défaut le modèle est choisi et annoncé avec sa raison, et reste sélectionnable
à la main :

- européenne → Bachelier sur la vol normale ATM cotée la plus proche : c'est
  la convention de cotation du marché ;
- bermudane → Hull-White calibré : sa valeur dépend du mouvement de toute la
  courbe entre les dates d'exercice, ce qu'un modèle d'un seul taux de swap ne
  décrit pas. Les modèles à un taux sont refusés pour une bermudane (422).

Les quatre modèles restent comparés côte à côte, chacun avec la vol normale
qu'il implique. Deux avertissements accompagnent le prix : un strike hors de
la monnaie valorisé à une vol ATM (les cotations n'ont pas de smile), et
l'erreur d'ajustement de Hull-White quand c'est lui qui valorise. Le modèle
retenu, le modèle demandé et les paramètres calibrés vont dans l'événement
d'audit. La calibration (≈ 0,6 s) est mise en cache par ses entrées : changer
le strike ou le sens ne recalibre pas.

## 6. Hybrides action-taux (issue #86)

`models/hybrid/hull_white_equity_sim_model.hpp` : l'action à vol plate sous
des taux Hull-White ajustés à la courbe, corrélés (ρ). Numéraire = compte
bancaire ; entre deux dates, (x, ∫x, incrément de W_S) est gaussien sachant
x, simulé exactement (covariance 3 × 3, moments par Gauss-Legendre) : pas de
biais de discrétisation, pas de pas intermédiaire. `df(T)` d'un script lit
P(t, T | x). Modèle `hull_white` de `price_script` et de l'API
(`hull_white = {mean_reversion, sigma, rho, currency}`) ; la courbe est la
courbe d'État de la devise dans la base (la seule gratuite), jamais un taux
plat de remplacement.

Tests (`tests/testHybrid.cpp`) : zéro-coupon = courbe quelle que soit la vol
des taux, forward de l'action exact pour tout ρ, un call long gagne avec ρ > 0,
et **une swaption bermudane écrite en script** (`df()` + `exercise()`) priceée
par LSMC retombe sur le réseau Hull-White du §3.

## 5. Ce qui manque

- Sources de marché hors USD : swaps OIS / IBOR et vols de swaptions, à
  ingérer dans `~/data-ingest` si une source gratuite apparaît (le dollar est
  couvert par les transactions DTCC).
- Grecques de la swaption dans la réponse (vega normal, delta par pilier) : le
  bloc `greeks` est vide, seul le profil de risque de la fiche Products bumpe
  les cotations.
- Courbe de forwards en dents de scie sur le jeu USD : médianes de
  transactions interpolées log-linéairement. Un lissage (Hagan-West) ou un
  ajustement de la courbe aux transactions la rendrait lisible.
- σ(t) constant par morceaux (calibration co-terminale exacte), cube de
  swaptions (smile par expiry × tenor), calendriers et bases de décompte.
