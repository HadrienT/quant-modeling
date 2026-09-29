# WP 22 — Rough Bergomi calibré (chantier 1c)

| | |
|---|---|
| **Dépend de** | le schéma hybride existant (`engines/mc/rough_bergomi_hybrid.hpp`), la surface SVI du marché (`market_snapshot`), Levenberg-Marquardt |
| **Débloque** | le chantier 5 option A (calibration neuronale du rough Bergomi) ; rough Bergomi comme modèle de script |
| **Branche** | `feat/rough-bergomi-calibration` |

## 1. Ce qui est calibré, et ce qui ne l'est pas

- **ξ0(t) = E[V_t] n'est pas un paramètre libre** : c'est la courbe de
  variance forward du marché, celle du **variance swap**,
  w_VS(T) = −2 E[ln S_T/F] = 2 ∫ OTM(K)/K² dK (Carr & Madan 1998), répliquée
  sur chaque tranche SVI puis différenciée en T.
  *Écart mesuré qui a imposé ce choix* : avec ξ0 pris sur la variance ATM, le
  modèle calibré sur SPY (25/09/2026) sortait **un point de vol trop bas à
  chaque strike**. Sous un skew négatif, le variance swap est au-dessus de la
  vol ATM, et E[V] est le variance swap.
- **(H, η, ρ)** sont calibrés par Levenberg-Marquardt dans
  H ∈ [0,02 ; 0,45], η ∈ [0,2 ; 5], ρ ∈ [−0,99 ; 0,2], sur les résidus
  « vol modèle − vol marché ». Les cibles sont lues sur les tranches SVI de la
  grille de Dupire (#119), aux mêmes moneyness standardisées que Heston, de
  une semaine à six mois : c'est là que la rugosité se voit.

## 2. Le pricer de surface

`engines/mc/rough_bergomi_surface.hpp` : toute la surface sur **les mêmes
chemins**. Schéma hybride κ = 1 (Bennedsen-Lunde-Pakkanen), grille uniforme
de 1/steps_per_year jusqu'à la plus longue maturité, chaque maturité lue au
point de grille le plus proche (à au plus un demi-pas). Chemins antithétiques,
X_T = S_T/F comme variable de contrôle (E[X_T] = 1). Philox par indice de
chemin et blocs sommés dans leur ordre : **les mêmes bits sur 1 ou 56
threads**. C'est ce qui rend l'objectif de calibration déterministe
(nombres aléatoires communs), donc différentiable par différences finies.

## 3. Tests (`tests/testRoughBergomiCalibration.cpp`)

- η = 0 : Black-Scholes sur la variance forward, à chaque strike ;
- mêmes bits sur 1 et 8 threads ;
- accord avec le pricer mono-maturité existant, dans l'erreur Monte-Carlo ;
- **pente du skew ATM en T^(H−1/2)** mesurée en log-log de une semaine à
  deux mois (Bayer, Friz & Gatheral 2016), à ±0,1 près ;
- aller-retour de calibration : paramètres retrouvés à 5e-3 près sur H ;
- un script à deux dates sous le modèle calibré (courbe ξ0, grille
  uniforme) égal à la somme des deux prix du pricer de surface.

## 4. Branchements

- `RoughBergomiSimModel` gagne une forme calibrée : courbe ξ0, plusieurs
  dates d'événement lues sur une grille uniforme (au moins quotidienne).
  L'ancienne forme (ξ0 plat, une maturité) est inchangée.
- Modèle de script `rough_bergomi` (`price_script`, API `model="rough_bergomi"`,
  page `/scripting`) ; `api/app/rough_vol.py` calibre une fois par snapshot,
  avec le même cache et le même stockage que Heston. L'erreur Monte-Carlo de
  chaque vol modèle est rapportée : c'est la résolution du fit.

## 5. Ce qui manque

- **GPU** : le pricer de surface est en C++ multithreadé (2 à 3 s pour une
  calibration SPY sur 56 cœurs). Un noyau CUDA du schéma hybride (convolution
  O(n²) par chemin, ou FFT) est le prochain pas s'il faut descendre sous la
  seconde ou calibrer tout l'univers.
- **Calibration neuronale** (chantier 5 option A) : ce pricer fournit les
  données d'entraînement.
- **AAD** du rough Bergomi calibré : η et ρ sont déjà des `T` dans le modèle
  de simulation ; ξ0 et H restent en double.
