# Bibliographie — ce qu'il faut avoir lu pour maîtriser le dépôt

Toutes les sources citées dans le dépôt, réunies en un seul endroit : les fiches
produits du front (`web/src/shared/products/docs.ts`), les commentaires du cœur
C++ (`include/`, `src/`), l'API (`api/app/`), les lots de travail
(`blueprint/wp/16`, `17`, `18`, `19`, `20`, `21`) et la roadmap (`etc/roadmap.md` §8).

**Légende**

- ★ : le socle, à lire en priorité (voir le parcours ci-dessous).
- ➕ : un ajout qui **n'est pas cité dans le dépôt**, mais qui comble un trou
  (le calcul stochastique en particulier : le code le suppose partout et ne le
  cite jamais).
- La colonne « Où » pointe vers le code qui applique la source.

Mettre ce fichier à jour quand une nouvelle référence entre dans le code ou
dans `docs.ts`.

---

## 0. Parcours de lecture conseillé

Dans cet ordre, ces douze lectures couvrent l'essentiel du dépôt ; tout le
reste est de l'approfondissement par sujet.

| # | Lecture | Ce qu'elle débloque |
|---|---|---|
| 1 | ➕ Shreve, *Stochastic Calculus for Finance II* (ch. 1–6) | Brownien, Itô, Girsanov, changement de numéraire, Feynman-Kac |
| 2 | Hull, *Options, Futures, and Other Derivatives* | Vocabulaire des produits, conventions de marché, greeks |
| 3 | Black & Scholes (1973) ; Merton (1973) | Tout `engines/analytic/` |
| 4 | Glasserman, *Monte Carlo Methods in Financial Engineering* (ch. 2–5, 7, 8) | Tout `engines/mc/`, `utils/` (Sobol, pont brownien, réduction de variance, greeks MC, LSM) |
| 5 | Gatheral, *The Volatility Surface* | SVI, Dupire, Heston, variance swaps — tout `market/` côté vol |
| 6 | Savine, *Modern Computational Finance: AAD and Parallel Simulations* | `aad/`, `simulation_engine_*aad*`, architecture de simulation parallèle |
| 7 | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA* | `scripting/`, logique floue, `fuzzy_evaluator` |
| 8 | Heston (1993) ; Fang & Oosterlee (2008) ; Andersen (2008) | Heston fermé (COS) et simulé (QE) |
| 9 | Hagan et al. (2002) « Managing Smile Risk » | SABR, swaptions SABR décalé |
| 10 | Andersen & Piterbarg, *Interest Rate Modeling* (vol. 1–2) | `models/rates/`, multi-courbe, Hull-White |
| 11 | O'Kane, *Modelling Single-name and Multi-name Credit Derivatives* | CDS, courbes de hazard, modèle standard ISDA |
| 12 | Bouzoubaa & Osseiran, *Exotic Options and Hybrids* | Autocalls, worst-of, mountain ranges — la logique de structuration |

---

## 1. Socle : calcul stochastique et pricing par arbitrage

| Référence | Où |
|---|---|
| ★ ➕ Shreve, *Stochastic Calculus for Finance II: Continuous-Time Models*, Springer, 2004 | Partout (dynamiques risque-neutre, mesures forward, isométrie d'Itô invoquée dans `vasicek_sim_model.hpp`, `schwartz_smith_sim_model.hpp`) |
| ➕ Björk, *Arbitrage Theory in Continuous Time*, 4ᵉ éd., Oxford University Press, 2019 | Changements de numéraire (mesure T-forward de `heston.hpp`, `rough_bergomi.hpp`), taux courts |
| ★ Hull, *Options, Futures, and Other Derivatives*, 11ᵉ éd., Pearson, 2021 | Référence manuel de la plupart des fiches produits |
| ★ Black & Scholes, « The Pricing of Options and Corporate Liabilities », *Journal of Political Economy* 81(3), 637–654, 1973 | `engines/analytic/black_scholes.hpp`, `engines/pde/european_vanilla.hpp` |
| ★ Merton, « Theory of Rational Option Pricing », *Bell Journal of Economics and Management Science* 4(1), 141–183, 1973 | Dividende continu, bornes, §8 : première barrière fermée |
| Black, « The Pricing of Commodity Contracts », *Journal of Financial Economics* 3(1–2), 167–179, 1976 | Black-76 : futures, caplets, swaptions (`engines/analytic/swap.hpp`, `sabr.hpp`) |

---

## 2. Produits (fiches de la page `/products`)

Reprise exacte des `references` de `web/src/shared/products/docs.ts`.

| Produit | Références |
|---|---|
| Vanille européenne | Black & Scholes (1973) ; Merton (1973) ; Hull (2021) |
| Américaine | Cox, Ross & Rubinstein, « Option Pricing: A Simplified Approach », *JFE* 7(3), 229–263, 1979 ; Brennan & Schwartz, « The Valuation of American Put Options », *Journal of Finance* 32(2), 449–462, 1977 ; Longstaff & Schwartz (2001, voir §7.5) ; Hull (2021) |
| Asiatique | Kemna & Vorst, « A Pricing Method for Options Based on Average Asset Values », *Journal of Banking & Finance* 14(1), 113–129, 1990 (forme fermée géométrique **et** variable de contrôle utilisée ici) ; Turnbull & Wakeman, « A Quick Algorithm for Pricing European Average Options », *JFQA* 26(3), 377–389, 1991 ; Glasserman (2004) |
| Barrière | Merton (1973, §8) ; Reiner & Rubinstein, « Breaking Down the Barriers », *Risk* 4(8), 28–35, 1991 (les huit formes fermées) ; Broadie, Glasserman & Kou, « A Continuity Correction for Discrete Barrier Options », *Mathematical Finance* 7(4), 325–349, 1997 ; Glasserman (2004) |
| Digitale | Reiner & Rubinstein, « Unscrambling the Binary Code », *Risk* 4(9), 75–83, 1991 ; Black & Scholes (1973) ; Hull (2021) |
| Lookback | Goldman, Sosin & Gatto, « Path Dependent Options: "Buy at the Low, Sell at the High" », *Journal of Finance* 34(5), 1111–1127, 1979 ; Conze & Viswanathan, « Path Dependent Options: The Case of Lookback Options », *Journal of Finance* 46(5), 1893–1907, 1991 ; Broadie, Glasserman & Kou, « Connecting Discrete and Continuous Path-Dependent Options », *Finance and Stochastics* 3(1), 55–82, 1999 |
| Panier | Gentle, « Basket Weaving », *Risk* 6(6), 51–52, 1993 ; Krekel, de Kock, Korn & Man, « An Analysis of Pricing Methods for Basket Options », *Wilmott Magazine*, juillet 2004, 82–89 ; Glasserman (2004) |
| Rainbow (worst-of / best-of) | Stulz, « Options on the Minimum or the Maximum of Two Risky Assets », *JFE* 10(2), 161–185, 1982 ; Johnson, « Options on the Maximum or the Minimum of Several Assets », *JFQA* 22(3), 277–283, 1987 ; Margrabe, « The Value of an Option to Exchange One Asset for Another », *Journal of Finance* 33(1), 177–186, 1978 |
| Future / forward | Cox, Ingersoll & Ross, « The Relation between Forward Prices and Futures Prices », *JFE* 9(4), 321–346, 1981 ; Hull (2021) |
| Obligation | Fabozzi, *Bond Markets, Analysis, and Strategies*, 10ᵉ éd., MIT Press, 2021 (ch. 2–4) ; Tuckman & Serrat, *Fixed Income Securities*, 3ᵉ éd., Wiley, 2011 ; Hull (2021) |
| Autocall | ★ Bouzoubaa & Osseiran, *Exotic Options and Hybrids*, Wiley, 2010 (ch. 16) ; Deng, Mallett & McCann, « Modeling Autocallable Structured Products », *Journal of Derivatives & Hedge Funds* 17(4), 326–340, 2011 ; Overhaus et al. (2007) |
| Mountain range (Himalaya) | Overhaus et al., *Equity Hybrid Derivatives*, Wiley, 2007 ; Quessette, « New Products, New Risks », *Risk* 15(3), 97–100, 2002 ; Bouzoubaa & Osseiran (ch. 14–15) |
| Variance swap | Demeterfi, Derman, Kamal & Zou, « More Than You Ever Wanted to Know About Volatility Swaps », Goldman Sachs Quantitative Strategies Research Notes, 1999 (la réplication implémentée dans `engines/analytic/variance_swap.hpp`) ; Carr & Madan, « Towards a Theory of Volatility Trading », dans *Volatility* (R. Jarrow éd.), Risk Books, 417–427, 1998 ; Gatheral (2006, ch. 11) |
| Volatility swap | Demeterfi et al. (1999) ; Broadie & Jain, « The Effect of Jumps and Discrete Sampling on Volatility and Variance Swaps », *IJTAF* 11(8), 761–797, 2008 ; Carr & Lee, « Robust Replication of Volatility Derivatives », 2009 |
| Dispersion swap | Bossu, « A New Approach for Modelling and Pricing Correlation Swaps », Dresdner Kleinwort, 2007 ; Bossu, *Advanced Equity Derivatives: Volatility and Correlation*, Wiley, 2014 ; Driessen, Maenhout & Vilkov, « The Price of Correlation Risk », *Journal of Finance* 64(3), 1377–1406, 2009 |
| FX forward | Clark, *Foreign Exchange Option Pricing: A Practitioner's Guide*, Wiley, 2011 (ch. 2–3) ; Hull (2021) |
| FX option | Garman & Kohlhagen, « Foreign Currency Option Values », *Journal of International Money and Finance* 2(3), 231–237, 1983 ; Clark (2011) |
| Commodity forward / option | Schwartz, « The Stochastic Behavior of Commodity Prices », *Journal of Finance* 52(3), 923–973, 1997 ; Geman, *Commodities and Commodity Derivatives*, Wiley, 2005 ; Black (1976) pour l'option |
| Forward-start | Rubinstein, « Pay Now, Choose Later », *Risk* 4(2), 44–47, 1991 ; Bouzoubaa & Osseiran (2010) |
| Compound | Geske, « The Valuation of Compound Options », *JFE* 7(1), 63–81, 1979 ; Bouzoubaa & Osseiran (2010) |
| Chooser | Rubinstein, « Options for the Undecided », *Risk* 4(4), 70–73, 1991 ; Bouzoubaa & Osseiran (2010) |
| Cliquet | Wilmott, « Cliquet Options and Volatility Models », *Wilmott Magazine*, décembre 2002 ; Bouzoubaa & Osseiran (2010) |
| Napoleon | Bouzoubaa & Osseiran (2010) |
| Quanto | Reiner, « Quanto Mechanics », *Risk* 5(3), 59–63, 1992 ; Bouzoubaa & Osseiran (2010) ; Hull (2021) |
| Double barrière | Kunitomo & Ikeda, « Pricing Options with Curved Boundaries », *Mathematical Finance* 2(4), 275–298, 1992 ; Bouzoubaa & Osseiran (2010) |
| Corridor / range accrual | Reiner & Rubinstein (1991, « Unscrambling the Binary Code ») ; Bouzoubaa & Osseiran (2010) |

---

## 3. Volatilité : surfaces, modèles, calibration

### 3.1 Vol locale et surface implicite

| Référence | Où |
|---|---|
| ★ Gatheral, *The Volatility Surface: A Practitioner's Guide*, Wiley, 2006 | `market/svi*.hpp`, `dupire_from_svi.hpp`, pipeline de surface |
| Dupire, « Pricing with a Smile », *Risk* 7(1), 18–20, 1994 | `models/equity/dupire.hpp`, `local_vol_*`, `market/dupire_from_svi.hpp` |
| Gatheral & Jacquier, « Arbitrage-free SVI volatility surfaces », *Quantitative Finance* 14(1), 59–71, 2014 | Condition papillon g(k) dans `market/svi.hpp` |

### 3.2 Vol stochastique et sauts

| Référence | Où |
|---|---|
| ★ Heston, « A Closed-Form Solution for Options with Stochastic Volatility », *Review of Financial Studies* 6(2), 327–343, 1993 | `models/equity/heston.hpp` |
| Albrecher, Mayer, Schoutens & Tistaert, « The Little Heston Trap », *Wilmott Magazine*, janvier 2007 | Forme de la fonction caractéristique sans discontinuité (`heston.hpp`) |
| ★ Fang & Oosterlee, « A Novel Pricing Method for European Options Based on Fourier-Cosine Series Expansions », *SIAM J. Sci. Comput.* 31(2), 826–848, 2008 | `engines/analytic/heston_cos.hpp` |
| ★ Andersen, « Simple and Efficient Simulation of the Heston Stochastic Volatility Model », *Journal of Computational Finance* 11(3), 2008 (§3.2, 4.2, 4.3) | `engines/mc/heston_qe.hpp` (schéma QE-M) |
| Lord, Koekkoek & van Dijk, « A Comparison of Biased Simulation Schemes for Stochastic Volatility Models », *Quantitative Finance* 10(2), 177–194, 2010 | Full truncation Euler (`bates_sim_model.hpp`, `slv_sim_model.hpp`) |
| Bates, « Jumps and Stochastic Volatility: Exchange Rate Processes Implicit in Deutsche Mark Options », *RFS* 9(1), 69–107, 1996 | `bates_sim_model.hpp` |
| Merton, « Option Pricing When Underlying Stock Returns Are Discontinuous », *JFE* 3(1–2), 125–144, 1976 | `merton_jump_diffusion_sim_model.hpp` |
| Kou, « A Jump-Diffusion Model for Option Pricing », *Management Science* 48(8), 1086–1101, 2002 | `kou_jump_diffusion_sim_model.hpp` |
| Guyon & Henry-Labordère, « Being Particular About Calibration », *Risk*, janvier 2012 | Méthode particulaire, `market/slv_calibration.hpp` |

### 3.3 SABR

| Référence | Où |
|---|---|
| ★ Hagan, Kumar, Lesniewski & Woodward, « Managing Smile Risk », *Wilmott Magazine*, septembre 2002, 84–108 | `models/equity/sabr.hpp`, `market/sabr_calibration.hpp`, swaptions SABR décalé |
| Hagan, Kumar, Lesniewski & Woodward, « Arbitrage-Free SABR », *Wilmott Magazine*, 2014 | `models/equity/sabr_pde.hpp` |
| Le Floc'h & Kennedy, « Finite Difference Techniques for Arbitrage-Free SABR », *Journal of Computational Finance*, 2017 (SSRN 2015) | Schéma de `sabr_pde.hpp` |
| Andreasen & Huge, « ZABR — Expansions for the Masses », SSRN, 2011 | Formulation EDP reprise dans `sabr_pde.hpp` |

### 3.4 Vol rugueuse

| Référence | Où |
|---|---|
| Gatheral, Jaisson & Rosenbaum, « Volatility is Rough », *Quantitative Finance* 18(6), 933–949, 2018 | Motivation, `rough_bergomi_sim_model.hpp` |
| Bayer, Friz & Gatheral, « Pricing under Rough Volatility », *Quantitative Finance* 16(6), 887–904, 2016 | `models/equity/rough_bergomi.hpp` |
| Bennedsen, Lunde & Pakkanen, « Hybrid Scheme for Brownian Semistationary Processes », *Finance and Stochastics* 21(4), 931–965, 2017 | `engines/mc/rough_bergomi_hybrid.hpp` |
| McCrickerd & Pakkanen, « Turbocharging Monte Carlo Pricing for the Rough Bergomi Model », *Quantitative Finance* 18(11), 1877–1886, 2018 | Idem |

---

## 4. Taux

| Référence | Où |
|---|---|
| ★ Andersen & Piterbarg, *Interest Rate Modeling*, 3 vol., Atlantic Financial Press, 2010 (§5.5, §10.1) | `models/rates/hull_white_curve.hpp`, `instruments/rates/swap.hpp` |
| ➕ Brigo & Mercurio, *Interest Rate Models — Theory and Practice*, 2ᵉ éd., Springer, 2006 | Alternative plus accessible à Andersen-Piterbarg pour Vasicek / CIR / Hull-White |
| Vasicek, « An Equilibrium Characterization of the Term Structure », *JFE* 5(2), 177–188, 1977 | `models/rates/vasicek*.hpp` |
| Cox, Ingersoll & Ross, « A Theory of the Term Structure of Interest Rates », *Econometrica* 53(2), 385–407, 1985 | `models/rates/cir.hpp` |
| Hull & White, « Pricing Interest-Rate-Derivative Securities », *RFS* 3(4), 573–592, 1990 | `models/rates/hull_white*.hpp`, `market/hull_white_calibration.hpp` |
| Jamshidian, « An Exact Bond Option Formula », *Journal of Finance* 44(1), 205–209, 1989 | `engines/analytic/hull_white_swaption.hpp`, options sur zéro-coupon |
| Mercurio, « Interest Rates and The Credit Crunch: New Formulas and Market Models », Bloomberg Portfolio Research Paper, 2009 | Spread multiplicatif vers la courbe de projection |
| Ametrano & Bianchetti, « Everything You Always Wanted to Know About Multiple Interest Rate Curve Bootstrapping but Were Afraid to Ask », SSRN, 2013 | `market/multi_curve_bootstrap.hpp` |
| Hagan & West, « Interpolation Methods for Curve Construction », *Applied Mathematical Finance* 13(2), 89–129, 2006 | Cité comme amélioration possible de l'interpolation (`api/app/rates.py`) |
| Svensson, « Estimating and Interpreting Forward Interest Rates: Sweden 1992–1994 », NBER WP 4871, 1994 (et Nelson & Siegel, *Journal of Business* 60(4), 1987) | Courbe zéro-coupon BCE lue telle quelle |

---

## 5. Crédit

| Référence | Où |
|---|---|
| ★ O'Kane, *Modelling Single-name and Multi-name Credit Derivatives*, Wiley, 2008 (ch. 3, 5, 6, 7) | `market/credit_curve.hpp`, `credit_bootstrap.hpp`, `engines/analytic/cds.hpp` (modèle standard ISDA) |
| Jarrow & Turnbull, « Pricing Derivative Securities Subject to Credit Risk », *Journal of Finance* 50(1), 53–85, 1995 | `models/credit/intensity.hpp` |
| Duffie & Singleton, « Modeling Term Structures of Defaultable Bonds », *RFS* 12(4), 687–720, 1999 | Idem |
| Merton, « On the Pricing of Corporate Debt: The Risk Structure of Interest Rates », *Journal of Finance* 29(2), 449–470, 1974 | `models/credit/merton_structural.hpp`, `api/app/fundamentals.py` |
| Crosbie & Bohn, « Modeling Default Risk », Moody's KMV, 2003 | Point de défaut KMV, EDF |
| Jones, Mason & Rosenfeld, « Contingent Claims Analysis of Corporate Capital Structures: An Empirical Investigation », *Journal of Finance* 39(3), 611–625, 1984 | Limites empiriques de Merton |
| Eom, Helwege & Huang, « Structural Models of Corporate Bond Pricing: An Empirical Analysis », *RFS* 17(2), 499–544, 2004 | Idem, `api/app/credit.py` |

---

## 6. Matières premières et FX

| Référence | Où |
|---|---|
| Schwartz & Smith, « Short-Term Variations and Long-Term Dynamics in Commodity Prices », *Management Science* 46(7), 893–911, 2000 | `models/commodity/schwartz_smith_sim_model.hpp` |
| Schwartz (1997) ; Geman (2005) | Voir §2 |
| Garman & Kohlhagen (1983) ; Clark (2011) | `models/fx/garman_kohlhagen.hpp` |

---

## 7. Méthodes numériques

### 7.1 Arbres et EDP

| Référence | Où |
|---|---|
| Cox, Ross & Rubinstein (1979) | `engines/tree/binomial.hpp` |
| Boyle, « Option Valuation Using a Three-Jump Process », *International Options Journal* 3, 7–12, 1986 | `engines/tree/trinomial.hpp` |
| Crank & Nicolson, « A Practical Method for Numerical Evaluation of Solutions of Partial Differential Equations of the Heat-Conduction Type », *Proc. Cambridge Phil. Soc.* 43(1), 50–67, 1947 | `engines/pde/european_vanilla.hpp`, `sabr_pde.hpp` |
| Rannacher, « Finite Element Solution of Diffusion Problems with Irregular Data », *Numerische Mathematik* 43, 309–327, 1984 | Démarrage Rannacher de `sabr_pde.hpp` |
| ➕ Duffy, *Finite Difference Methods in Financial Engineering: A Partial Differential Equation Approach*, Wiley, 2006 | Manuel pour tout ce qui est EDP |

### 7.2 Monte-Carlo : le manuel

| Référence | Où |
|---|---|
| ★ Glasserman, *Monte Carlo Methods in Financial Engineering*, Springer, 2004 — ch. 2 (générateurs), 3.1 (pont brownien), 4 (réduction de variance : §4.1 variables de contrôle, §4.3 stratification, §4.6 importance sampling), 5 (QMC), 7 (greeks : pathwise, likelihood ratio, §7.2–7.3 lissage), 8 (américaines) | Tout `engines/mc/` et `utils/` |
| Jäckel, *Monte Carlo Methods in Finance*, Wiley, 2002 | `utils/brownian_bridge.hpp` (algorithme de Jäckel) |

### 7.3 Générateurs aléatoires et quasi-aléatoires

| Référence | Où |
|---|---|
| O'Neill, « PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation », Harvey Mudd College, HMC-CS-2014-0905, 2014 | `utils/rng.hpp` (PCG32, saut « advance ») |
| Salmon, Moraes, Dror & Shaw, « Parallel Random Numbers: As Easy as 1, 2, 3 », *SC'11*, 2011 | `utils/philox.hpp`, générateur GPU |
| Box & Muller, « A Note on the Generation of Random Normal Deviates », *Annals of Mathematical Statistics* 29(2), 610–611, 1958 | `utils/gaussian_source.hpp` |
| Acklam, « An Algorithm for Computing the Inverse Normal Cumulative Distribution Function », note en ligne, ~2003 | `utils/inverse_normal.hpp` |
| Sobol', « On the Distribution of Points in a Cube and the Approximate Evaluation of Integrals », *USSR Comput. Math. Math. Phys.* 7(4), 86–112, 1967 | `utils/sobol.hpp` |
| Joe & Kuo, « Constructing Sobol Sequences with Better Two-Dimensional Projections », *SIAM J. Sci. Comput.* 30(5), 2635–2654, 2008 | `utils/sobol_directions.hpp` (fichier `new-joe-kuo-6.21201`) |
| Owen, « Randomly Permuted (t,m,s)-Nets and (t,s)-Sequences », *Monte Carlo and Quasi-Monte Carlo Methods in Scientific Computing*, Springer, 1995 | Brouillage d'Owen, `utils/sobol.hpp` |
| Burley, « Practical Hash-based Owen Scrambling », *JCGT* 9(4), 2020 (avec la permutation de Laine & Karras, 2011) | Idem, version par hachage |

### 7.4 Réduction de variance et échantillonnage

| Référence | Où |
|---|---|
| Kemna & Vorst (1990) | Variable de contrôle géométrique des asiatiques |
| Glasserman, Heidelberger & Shahabuddin, « Asymptotically Optimal Importance Sampling and Stratification for Pricing Path-Dependent Options », *Mathematical Finance* 9(2), 117–152, 1999 | `engines/mc/importance_sampling.hpp` |
| Cochran, *Sampling Techniques*, 3ᵉ éd., Wiley, 1977 | Strates regroupées, différences successives (`utils/variance_reduction/`) |
| Andersen & Brotherton-Ratcliffe, « Exact Exotics », *Risk* 9(10), 85–89, 1996 | Correction de pont brownien des barrières (`engines/mc/barrier.hpp`) |
| Broadie, Glasserman & Kou (1997) | Correction de continuité des barrières discrètes |

### 7.5 Exercice anticipé par simulation

| Référence | Où |
|---|---|
| ★ Longstaff & Schwartz, « Valuing American Options by Simulation: A Simple Least-Squares Approach », *RFS* 14(1), 113–147, 2001 | `engines/mc/lsm.hpp`, `scripting/exercise.hpp` |
| Andersen & Broadie, « Primal-Dual Simulation Algorithm for Pricing Multidimensional American Options », *Management Science* 50(9), 1222–1234, 2004 | Borne haute — **non implémentée**, citée comme limite dans WP 16 §14 |

---

## 8. Calibration et optimisation

| Référence | Où |
|---|---|
| Levenberg, « A Method for the Solution of Certain Non-Linear Problems in Least Squares », *Quarterly of Applied Mathematics* 2(2), 164–168, 1944 ; Marquardt, « An Algorithm for Least-Squares Estimation of Nonlinear Parameters », *SIAM J. Appl. Math.* 11(2), 431–441, 1963 | `market/calibration/levenberg_marquardt.hpp` (SVI, SABR, Heston, Hull-White) |
| ➕ Nocedal & Wright, *Numerical Optimization*, 2ᵉ éd., Springer, 2006 (ch. 10 : moindres carrés, Gauss-Newton) | Hessien de Gauss-Newton de `market/superbucket.hpp`, Newton de `merton_structural.hpp` |
| ➕ Madsen, Nielsen & Tingleff, *Methods for Non-Linear Least Squares Problems*, notes de cours DTU, 2ᵉ éd., 2004 | La référence courte et pratique pour LM |

---

## 9. Différentiation automatique adjointe (AAD) et greeks

| Référence | Où |
|---|---|
| ★ Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley, 2018 | `aad/` (tape, nœuds, expressions, blocklist), `simulation_engine_*aad*.hpp` — **à suivre de près**, écarts consignés en ADR |
| Giles & Glasserman, « Smoking Adjoints: Fast Monte Carlo Greeks », *Risk* 19(1), 88–92, 2006 | L'article fondateur |
| Capriotti, « Fast Greeks by Algorithmic Differentiation », *Journal of Computational Finance* 14(3), 3–35, 2011 | |
| Griewank & Walther, *Evaluating Derivatives: Principles and Techniques of Algorithmic Differentiation*, 2ᵉ éd., SIAM, 2008 (ch. 5 : ordre 2) | `aad/tangent.hpp`, `utils/dual.hpp`, `simulation_engine_aad2.hpp` (« adjoint over tangent ») |
| Naumann, *The Art of Differentiating Computer Programs*, SIAM, 2012 (ch. 3) | Idem |
| Glasserman (2004, ch. 7) | Pathwise vs likelihood ratio, lissage des discontinuités |

---

## 10. Scripting de produits

| Référence | Où |
|---|---|
| ★ Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley, 2021 | `scripting/` entier (lexer, parser, visiteurs, `fuzzy_evaluator`, bytecode) — **à suivre de près** |
| Andreasen & Huge, « Random Grids », *Risk*, juillet 2011 | Le scripting tel qu'il se pratique en salle |

---

## 11. GPU et calcul parallèle

| Référence | Où |
|---|---|
| Savine (2018), partie I | Parallélisme CPU (`utils/thread_pool.hpp`, `concurrent_queue.hpp`) |
| Salmon et al. (2011) | Philox, générateur à compteur sans état |
| ➕ Hwu, Kirk & El Hajj, *Programming Massively Parallel Processors*, 4ᵉ éd., Morgan Kaufmann, 2022 | `gpu/`, noyaux `engines/mc/kernels/` |
| ➕ NVIDIA, *CUDA C++ Programming Guide* et *CUDA C++ Best Practices Guide* (documentation en ligne) | Idem |
| Kahan, « Further Remarks on Reducing Truncation Errors », *Communications of the ACM* 8(1), 40, 1965 ; Welford, *Technometrics* 4(3), 419–420, 1962 | Accumulation stable (`utils/accumulators.hpp`, roadmap §chantier 3) |

---

## 12. Risque de portefeuille

| Référence | Où |
|---|---|
| Cornish & Fisher, « Moments and Cumulants in the Specification of Distributions », *Revue de l'Institut International de Statistique* 5(4), 307–320, 1938 | VaR delta-gamma de `api/app/routers/portfolio.py` |
| ➕ Jorion, *Value at Risk*, 3ᵉ éd., McGraw-Hill, 2006 | Cadre général VaR / ES |
| ★ Gregory, *The xVA Challenge: Counterparty Risk, Funding, Collateral, Capital and Initial Margin*, 4ᵉ éd., Wiley, 2020 | Métriques d'exposition et intégrales CVA / DVA / FVA / MVA / KVA, avec sa convention de signe : `risk/exposure_metrics.hpp`, `risk/xva.hpp` (`blueprint/wp/23-xva.md`) |
| Comité de Bâle, *The standardised approach for measuring counterparty credit risk exposures* (BCBS 279), 2014 ; cadre de Bâle CRE52, exemples en CRE99 | SA-CCR : `risk/regulatory/sa_ccr.hpp`, exemples chiffrés dans `tests/testSaCcr.cpp` |
| Cadre de Bâle, CRE31 (fonction de pondération IRB) et CRE53 (IMM : Effective EE, EEPE) | `risk/regulatory/irb.hpp`, `risk/exposure_metrics.hpp` |
| Comité de Bâle, *Targeted revisions to the credit valuation adjustment risk framework* (d507), 2020 ; cadre de Bâle MAR50 | BA-CVA : `risk/regulatory/ba_cva.hpp` |
| BCBS-IOSCO, *Margin requirements for non-centrally cleared derivatives*, 2020 | Grille standard de marge initiale : `risk/regulatory/im_schedule.hpp` |
| Markowitz, « Portfolio Selection », *Journal of Finance* 7(1), 77–91, 1952 ; Sharpe, « Mutual Fund Performance », *Journal of Business* 39(1), 119–138, 1966 | Optimisation au ratio de Sharpe (page stratégies / backtest) |

---

## 13. Génie logiciel, observabilité, gouvernance des modèles

Cités par WP 18 (`blueprint/wp/18-observability.md`).

| Référence | Où |
|---|---|
| Fed / OCC, *SR 11-7 — Supervisory Guidance on Model Risk Management*, 2011 | Pourquoi l'audit et le replay des valorisations |
| PRA, *SS1/23 — Model Risk Management Principles for Banks*, 2023 | Idem |
| Kleppmann, *Designing Data-Intensive Applications*, O'Reilly, 2017 (ch. 11) | Flux d'événements |
| Shapira, Palino, Sivaram & Petty, *Kafka: The Definitive Guide*, 2ᵉ éd., O'Reilly, 2021 | `api/app/audit/` (producteur Kafka) |
| Richardson, *Microservices Patterns*, Manning, 2018 | Transactional outbox (le spool `LOG_DIR/spool`) |
| Majors, Fong-Jones & Miranda, *Observability Engineering*, O'Reilly, 2022 | `api/app/telemetry.py` |
| ➕ Meyers, *Effective Modern C++*, O'Reilly, 2014 | Idiomes C++11/14 du cœur (sémantique de déplacement, `unique_ptr`, templates) |

---

## 14. Horizon de la roadmap (pas encore dans le code)

À lire quand les chantiers correspondants démarrent (`etc/roadmap.md` §8).

| Sujet | Référence |
|---|---|
| xVA (suite du WP 23 : moteur d'exposition, AMC, sensibilités) | Green, *XVA: Credit, Funding and Capital Valuation Adjustments*, Wiley, 2015 ; Andreasen & Savine (2021), partie xVA ; articles listés dans `blueprint/wp/23-xva.md` §18 |
| Deep hedging | Buehler, Gonon, Teichmann & Wood, « Deep Hedging », *Quantitative Finance* 19(8), 1271–1291, 2019 |
| Calibration neuronale | Horvath, Muguruza & Tomas, « Deep Learning Volatility », *Quantitative Finance* 21(1), 11–27, 2021 |
| Arrêt optimal par réseaux | Becker, Cheridito & Jentzen, « Deep Optimal Stopping », *Journal of Machine Learning Research* 20(74), 1–25, 2019 |
