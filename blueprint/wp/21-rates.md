# WP 21 — Taux : multi-courbe, swaps, swaptions, Hull-White calibré, page `/rates`

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

## 4. API et page

`POST /api/rates/analyse` (`api/app/rates_derivatives.py`) : courbes, swap,
calibration, swaption sous les quatre modèles, bermudan et prime de switch.
`GET /api/rates/example` : les cotations de départ.

**Les cotations sont des saisies.** Aucune source gratuite ne publie de swaps
OIS, de courbe de swaps sur un index ni de vols de swaptions (FRED n'en a pas).
La page part de cotations EUR illustratives (€STR, EURIBOR 6M), l'écrit en
tête (« Manual input ») et dans la méthodologie, et calcule sur ce que
l'utilisateur saisit. Rien ne lit la base, donc pas de repli Yahoo possible.
L'analyse n'est pas un `pricing.valuation` : c'est une page d'analyse comme
`/credit`, sans produit unique à rejouer.

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

- Sources de marché : swaps OIS / IBOR et vols de swaptions, à ingérer dans
  `~/data-ingest` si une source gratuite apparaît.
- σ(t) constant par morceaux (calibration co-terminale exacte), cube de
  swaptions (smile par expiry × tenor), calendriers et bases de décompte.
