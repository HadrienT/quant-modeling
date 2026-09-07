# Roadmap — faire de `quant-modeling` un projet qui se démarque

> Document de stratégie, pas de spécification. Objectif : transformer une bonne
> librairie de pricing en un projet qui fait passer un entretien quant / quant dev.
> Rédigé à partir de l'état du repo (~28 000 LOC C++20, 25 fichiers de tests, CI,
> chaîne C++ → pybind11 → FastAPI → React).

---

## 1. Diagnostic de l'existant

### 1.1 Ce qui est déjà un atout

| Atout | Pourquoi ça compte |
|---|---|
| Architecture C++20 propre : `instruments` / `models` / `engines` / `pricers` avec registry + adapters | La séparation payoff / modèle / moteur est *exactement* le découpage d'une lib desk. C'est ce qu'un lead regarde en premier. |
| Catalogue exotique déjà large : autocall, mountain, dispersion, rainbow, variance swap, lookback, basket, barrière, digitale, asiatique | Au-dessus de la quasi-totalité des repos « pricing library » de GitHub. |
| Chaîne complète jusqu'au produit : bindings pybind11, API FastAPI (auth, portefeuille, backtest, price tape), front React | Très rare chez un candidat. Montre que tu sais livrer, pas seulement coder un pricer. |
| Réduction de variance sérieuse : Sobol + direction numbers, pont brownien, control variates, stratification, importance sampling | C'est du contenu technique réel, pas du cosmétique. |
| Local vol / Dupire calibré sur données de marché réelles (`api/app/local_vol/`) | La partie « données sales » est celle que tout le monde évite. |
| `core/platform.hpp` déjà pensé pour nvcc (`QM_HOST_DEVICE`) et kernels isolés | Le portage GPU est à portée : le design est déjà bon. |

### 1.2 Les trous qui coûtent cher

| Trou | Conséquence |
|---|---|
| **Aucune vol stochastique** (ni Heston, ni SABR, ni rough vol) | Rédhibitoire. « Surface de vol » sans modèle stochastique ne tient pas trente secondes en entretien. |
| **Aucun framework de calibration** en C++ (pas d'optimiseur, pas d'objective function) | Un modèle non calibré n'est pas un modèle. C'est le point n°1 de ta propre checklist `todo.txt`, section 5. |
| **AAD non implémenté** | `utils/greeks.hpp` = différences finies centrées. Le AAD est *préparé* (macros, commentaires « foundation for AAD »), pas écrit. À corriger avant de l'écrire sur le CV. |
| **Les deux V100 ne servent à rien** | Zéro fichier `.cu`. C'est le plus gros levier de différenciation inexploité du projet. |
| **Pas de LSMC / Bermudéen Monte-Carlo** | Bloque l'américain MC, les autocalls avec exercice optimal, et surtout le xVA. |
| **Courbes : une seule `DiscountCurve`, pas de bootstrap, pas de multi-courbe** | Bloque swaps / swaptions, donc bloque xVA. |
| **Pas de crédit** (hazard rate, CDS, survie) | Prérequis direct du CVA. |
| **`README.md` = 16 octets** | Trente secondes de lecture décident si un recruteur clone ou ferme l'onglet. Aujourd'hui il ferme. |

---

## 2. La thèse : sur quoi se différencier

Il existe des centaines de « j'ai écrit un pricer Black-Scholes ». Il en existe très peu
qui tiennent les **trois** axes suivants en même temps :

```
   Numérique stochastique          ×          C++ haute performance          ×          Machine learning
   (Heston, rough vol, LSMC)                  (AAD, CUDA multi-GPU)                     (deep hedging, calib neuronale)
```

C'est *ton* intersection : tu viens du ML, tu as appris le calcul stochastique, tu écris
du C++ propre, et tu as deux V100. Personne ne va te battre sur « j'ai plus de produits
dans mon catalogue ». Tout le monde peut être battu sur « j'ai un moteur MC GPU avec
greeks adjoints qui calcule un profil d'exposition CVA ».

**Corollaire important : arrête d'ajouter des produits.** Le 25ᵉ payoff exotique
n'ajoute rien au CV. La profondeur sur trois sujets bat la largeur sur vingt.

---

## 3. Les chantiers, par ordre de priorité

### Chantier 0 — Fondations (2 à 3 semaines) · *prérequis, pas optionnel*

Sans ça, les chantiers 1 à 4 sont impossibles à faire proprement.

**Framework de calibration**
- Interface `ObjectiveFunction` : `residuals(params) -> vector<Real>`, bornes explicites.
- Optimiseurs : Levenberg-Marquardt (le workhorse des calibrations de smile) et
  Nelder-Mead ou Differential Evolution pour les cas non convexes.
- Poids par vega (on calibre en prix mais on pondère par sensibilité, sinon les ailes
  écrasent l'ATM), graine fixée, calibration reproductible.
- Rapport de calibration : RMSE en vol implicite (en points de vol, pas en prix),
  pire point, temps, nombre d'itérations.

**Infrastructure de marché**
- Conventions : `DayCounter` (ACT/360, ACT/365F, 30/360), `Calendar`,
  `BusinessDayConvention`, génération de `Schedule`.
- Bootstrap de courbe multi-instruments (dépôts → futures/FRA → swaps).
- Multi-courbe : courbe d'actualisation OIS/SOFR ≠ courbe de projection forward.
  C'est le standard post-2008 ; ne pas l'avoir date immédiatement le projet.
- Interpolation en forward instantané monotone, avec test de non-arbitrage.

**Ce qu'on te demandera en entretien**
> Pourquoi actualise-t-on à l'OIS un swap collatéralisé ? Que devient la courbe de
> projection ? Pourquoi interpoler sur les forwards plutôt que sur les taux zéro ?

---

### Chantier 1 — Volatilité stochastique et surface (5 à 7 semaines) · **priorité maximale**

C'est le trou le plus visible et le plus pénalisant.

**1a. Heston**
- Pricing semi-analytique par Fourier. Deux implémentations à connaître :
  Carr-Madan (FFT) et **la méthode COS de Fang & Oosterlee** — cette dernière est
  plus rapide et plus stable, c'est celle qu'il faut pour calibrer.
- Le piège classique : la **discontinuité du logarithme complexe** dans la fonction
  caractéristique. Utiliser la formulation « Little Heston Trap » d'Albrecher et al.,
  sinon la calibration diverge sur les maturités longues.
- Simulation Monte-Carlo : **schéma QE d'Andersen** (Quadratic Exponential).
  L'Euler naïf sur le processus de variance produit des variances négatives et un
  biais massif dès que la condition de Feller (2κθ > ξ²) n'est pas respectée —
  et elle ne l'est presque jamais sur les paramètres calibrés en pratique.
- Validation : reproduire les valeurs de référence publiées (Albrecher et al. 2007,
  Andersen 2008) à 1e-6 près.

**1b. SABR**
- Formule de Hagan pour la vol implicite, **et** sa correction : l'expansion originale
  produit des densités négatives en strikes bas, ce qui est le problème central des
  taux bas. Implémenter au minimum la version arbitrage-free de Hagan (2014)
  ou l'approche de Obłój.
- Usage : smile de taux (caps/floors, swaptions), et interpolation de smile equity.

**1c. Rough Bergomi** — *le différenciateur*
- Vol rugueuse : H ≈ 0.1, mémoire longue, reproduit la pente ATM en T^(H−1/2)
  que ni Heston ni la local vol ne capturent.
- **Schéma hybride de Bennedsen-Lunde-Pakkanen** pour simuler le mouvement brownien
  fractionnaire de façon efficace.
- C'est un sujet de recherche actif (Gatheral, Jaisson, Rosenbaum, « Volatility is
  rough », 2018) et **c'est extrêmement coûteux en calcul** — donc c'est le candidat
  parfait pour les chantiers GPU et calibration neuronale ci-dessous. Les trois se
  renforcent.

**1d. Objet surface de vol propre**
- `VolSurface` avec `vol(T, K)`, construite à partir de quotes de marché.
- **Paramétrisation SVI** (Gatheral) par tranche de maturité, avec les conditions
  de non-arbitrage : butterfly (densité positive) et calendar spread (variance
  totale croissante en T).
- Aujourd'hui la construction de surface est en Python (`api/app/local_vol/`) ;
  la faire remonter en C++ rend la lib autonome.

**Ce qu'on te demandera en entretien**
> Pourquoi l'Euler explose-t-il sur Heston ? Que fait le QE ? Qu'est-ce que la
> condition de Feller et pourquoi on s'en moque en pratique ? Pourquoi la local vol
> donne-t-elle une mauvaise dynamique forward du smile alors qu'elle recale
> parfaitement les vanilles d'aujourd'hui ? Qu'apporte la vol rugueuse ?

---

### Chantier 2 — AAD réel (4 à 5 semaines) · **le plus fort signal technique**

C'est le sujet qui sépare un candidat « a lu des livres » d'un candidat « peut
contribuer à une lib de production ». Référence : Antoine Savine, *Modern
Computational Finance: AAD and Parallel Simulations*.

**Ce qu'il faut construire**
- Un type `Number` avec surcharge d'opérateurs qui enregistre les opérations sur
  une **tape** (bande d'enregistrement) : mode adjoint / reverse.
- Tape par thread, allocation par blocs, pas d'allocation dynamique dans la boucle
  chaude (cohérent avec ta section 8 de `todo.txt`).
- **Checkpointing** : sur un MC à 1M chemins × 250 pas, la tape complète ne tient pas
  en mémoire. Il faut enregistrer par chemin et rembobiner, ou faire du checkpointing
  par blocs de temps.
- Propagation vers les **paramètres de marché** et pas seulement les paramètres du
  modèle : c'est le passage des « greeks modèle » aux « greeks marché », qui demande
  de différentier à travers la calibration (théorème des fonctions implicites, ou
  matrice jacobienne de la calibration inversée).

**La démonstration qui frappe**
Un tableau comparant, pour un panier à 50 sous-jacents (donc ~50 deltas + 50 vegas +
corrélations) :

| Méthode | Coût relatif à un pricing | Précision |
|---|---|---|
| Bump-and-reprice, différences centrées | ~2 × N_params (≈ 200×) | Bruit MC amplifié par 1/h |
| AAD (mode adjoint) | ~3 à 5× | Exacte à la précision machine sur le path |

Ce ratio — **toutes les sensibilités pour quelques fois le prix, indépendamment de
leur nombre** — est le résultat central du sujet. C'est un excellent point de README.

**Ce qu'on te demandera en entretien**
> Pourquoi le mode adjoint coûte-t-il O(1) en nombre de paramètres et le mode tangent
> O(N) ? Comment gères-tu la mémoire de la tape sur un MC long ? Comment
> différentie-t-on un payoff discontinu (digitale, barrière) — pourquoi le pathwise
> échoue-t-il et que fait-on (lissage, likelihood ratio, ou combinaison) ?

*Note : ton lissage de digitales et tes probabilités de survie de barrière
(commit « Phase 3 smoothing ») sont précisément la brique qui rend le pathwise
applicable. C'est déjà fait, il faut le connecter.*

---

### Chantier 3 — Monte-Carlo GPU sur les deux V100 (4 à 6 semaines)

Tu as un actif matériel que presque aucun candidat n'a. Ne pas l'utiliser est un
gâchis pur.

**Pourquoi la V100 est particulièrement pertinente ici**
- Ratio FP64:FP32 de **1:2** (≈ 7,8 TFLOPS en double précision). C'est une carte de
  calcul scientifique, contrairement aux GeForce où le FP64 est bridé à 1:32 ou 1:64.
  Un MC de pricing en `double` y tourne réellement vite — argument technique concret
  et vérifiable à mettre dans le README.
- Compute capability 7.0 (`sm_70`), Tensor Cores FP16 disponibles pour la partie ML.
- 16 GB × 2 : largement assez pour des tableaux de chemins par blocs et pour
  l'entraînement des réseaux du chantier 5.

**Plan de portage**
1. `cuRAND` pour la génération : `MRG32k3a` ou `Philox` pour le pseudo-aléatoire,
   `scrambled Sobol` pour le QMC — tu as déjà tes direction numbers, il faut décider
   entre les tiens et ceux de cuRAND et documenter le choix.
2. Génération de chemins dans le kernel, payoff appliqué **sur place** : on ne
   rapatrie jamais les chemins en mémoire hôte, seulement les accumulateurs.
3. Réduction hiérarchique (warp shuffle → shared memory → grid) pour la moyenne
   et la variance. Attention à la stabilité numérique : accumulation de Welford ou
   Kahan, pas une somme naïve sur 10⁸ termes.
4. **Multi-GPU** : découpage du flux QMC par blocs disjoints de la séquence Sobol
   (surtout pas la même sous-séquence sur les deux cartes), un stream par carte,
   réduction finale sur l'hôte. Le point délicat et intéressant : **garantir la
   reproductibilité bit-à-bit** indépendamment du nombre de GPU. C'est une vraie
   exigence de desk (contrôle des risques doit pouvoir rejouer un chiffre).
5. Occupancy et coalescence : layout des chemins en structure-of-arrays.

**Le livrable qui compte : un benchmark honnête**
Un tableau CPU mono-thread / CPU multi-thread / 1 V100 / 2 V100, avec chemins par
seconde, erreur standard atteinte, et temps pour une erreur cible fixée. Un speedup
annoncé sans erreur standard associée ne vaut rien — la bonne métrique est
**« temps pour atteindre une erreur de 1e-4 »**, pas « chemins par seconde ».

**Ce qu'on te demandera en entretien**
> Qu'est-ce qui limite ton kernel, la bande passante mémoire ou le calcul ? Comment
> assures-tu que deux GPU ne consomment pas les mêmes nombres quasi-aléatoires ?
> Pourquoi la réduction en float naïve est-elle dangereuse à 10⁸ chemins ?

---

### Chantier 4 — Taux, crédit, puis xVA (8 à 10 semaines) · **le capstone**

C'est ton objectif déclaré et c'est le bon : le xVA est le point de convergence de
tout le reste. Il ne peut pas être fait en premier, mais il justifie tous les
chantiers précédents.

**4a. Prérequis taux (3 semaines)**
- Swaps vanille, avec legs et cashflows propres (ta section 2 de `todo.txt`).
- Swaptions : Black, Bachelier (**indispensable en taux bas / négatifs**, où le
  lognormal n'a pas de sens), SABR pour le smile.
- Hull-White calibré sur une grille de swaptions — tu as déjà le modèle, il manque
  la calibration (chantier 0).

**4b. Prérequis crédit (2 semaines)**
- Courbe d'intensité de défaut (hazard rate) constante par morceaux.
- Bootstrap depuis des spreads CDS de marché.
- Probabilités de survie et de défaut, avec recovery rate.

**4c. Moteur d'exposition (3 semaines)**
- Simulation jointe de tous les facteurs de risque d'un portefeuille, sur une grille
  de dates futures (typiquement mensuelle jusqu'à 30 ans).
- Repricing du portefeuille à chaque date × chaque chemin. **C'est ici que le GPU
  du chantier 3 devient indispensable** : c'est un problème à 10⁴–10⁶ repricings
  imbriqués, c'est exactement ce que les desks font tourner la nuit.
- Pour les produits sans forme fermée à une date future : **LSMC (Longstaff-Schwartz)**
  comme proxy de pricing conditionnel. Tu as besoin du LSMC de toute façon pour le
  bermudéen — mutualiser.
- Profils : EE (Expected Exposure), EPE, ENE, PFE à 95% / 99%.
- **Netting sets** et collatéral CSA : seuil, MTA, période de marge (MPoR).
  Sans netting, un calcul de CVA est faux d'un ordre de grandeur.

**4d. Les ajustements (2 semaines)**
- **CVA** : perte attendue sur défaut de la contrepartie.
- **DVA** : le symétrique sur ton propre défaut (et la discussion sur son caractère
  discutable comptablement — bonne question d'entretien).
- **FVA** : coût de financement du collatéral non rémunéré.
- **MVA** : coût de financement de la marge initiale (SIMM).
- **Wrong-way risk** : corrélation entre exposition et défaut. C'est le sujet
  difficile et celui qui montre que tu as compris, pas seulement implémenté.

**Le bouquet final : CVA + AAD**
Calculer les **sensibilités du CVA** (à chaque point de courbe, chaque spread CDS,
chaque vol) par mode adjoint. Un desk XVA a des milliers de sensibilités à sortir
quotidiennement ; le bump-and-reprice y est physiquement impossible. C'est
littéralement le cas d'usage industriel qui a fait adopter le AAD en banque.

Si tu ne dois faire qu'**une** chose remarquable dans ce projet, c'est celle-là :
*un profil d'exposition CVA calculé sur GPU avec ses sensibilités par AAD*.

---

### Chantier 5 — La couche ML (3 à 5 semaines par sujet) · **ton avantage comparatif**

Ne pas faire les trois. En choisir **un**, le faire bien, savoir le défendre.

**Option A — Calibration neuronale** *(meilleur rapport valeur/effort)*
- Référence : Horvath, Muguruza, Tomas, *Deep Learning Volatility* (2019).
- Principe : un réseau apprend l'application `paramètres du modèle → surface de vol
  implicite`, hors ligne, sur des millions d'échantillons générés par ton moteur MC.
  Ensuite la calibration devient une inversion de ce réseau, en millisecondes au lieu
  de minutes.
- **Pourquoi c'est le bon choix ici** : ça branche directement sur le chantier 1,
  ça rend le rough Bergomi calibrable (sinon c'est trop lent pour être utilisable),
  ça consomme le GPU pour générer les données *et* pour entraîner, et ça met en
  valeur ton profil ML sur un problème authentiquement financier.
- Le point de rigueur : montrer que l'erreur du réseau reste **sous le bid-ask du
  marché**. Un réseau précis à 0,5 point de vol est inutile si le spread est de
  0,2 point.

**Option B — Deep hedging** *(le plus spectaculaire)*
- Référence : Buehler, Gonon, Teichmann, Wood (2019).
- Principe : au lieu de couvrir par le delta du modèle, un réseau apprend
  directement la politique de couverture qui minimise un risque (CVaR, entropique)
  **sous coûts de transaction et contraintes de liquidité** — c'est-à-dire dans le
  monde où le delta-hedging théorique n'est pas optimal.
- Démonstration : comparer P&L de couverture delta-BS vs deep hedging sur une barrière,
  en présence de coûts. Le réseau doit apprendre à sous-couvrir près de la barrière.
- Attention : c'est le sujet le plus « joli » mais le plus facile à faire
  superficiellement. Il faut une vraie fonction d'utilité et une vraie analyse de
  distribution de P&L, pas juste une courbe de loss qui descend.

**Option C — Deep Optimal Stopping / Deep BSDE**
- Références : Becker, Cheridito, Jentzen (2019) ; Han, Jentzen, E (2018).
- Pour le bermudéen en grande dimension, là où Longstaff-Schwartz s'effondre
  (régression polynomiale sur 50 sous-jacents = malédiction de la dimension).
- Bon complément au chantier 4 (exposition future de produits callables).

**Le piège à éviter absolument.** Un jury de quants est *sceptique* par défaut sur le
ML en finance. Ce qui te sauve : toujours comparer au benchmark classique, montrer
où le ML gagne **et où il perd**, et donner des barres d'erreur. Un candidat qui dit
« mon réseau bat Black-Scholes » sans intervalle de confiance perd le poste ;
un candidat qui dit « mon réseau égale le semi-analytique à 3e-4 près en 1/1000ᵉ du
temps, et voici où il se dégrade » le décroche.

---

## 4. Ce qui transforme un projet étudiant en projet crédible

À travail égal, c'est ça qui fait la différence. Ça vaut plus que deux produits de plus.

**Validation contre des références externes**
- Comparaison systématique à **QuantLib** sur tout ce qui existe des deux côtés,
  avec les écarts publiés dans le repo.
- Reproduction des valeurs de référence des articles fondateurs (Heston, QE d'Andersen,
  Longstaff-Schwartz table 1).
- Un test qui échoue si l'écart dépasse la tolérance : la validation doit être dans la
  CI, pas dans un notebook.

**Tests de convergence, pas seulement de valeur**
- Vérifier les **ordres de convergence** : Euler faible 1, fort 1/2 ; Milstein fort 1.
  Un test qui mesure la pente en log-log est bien plus convaincant qu'un test d'égalité.
- Convergence de grille PDE (Crank-Nicolson en O(Δt²) — et savoir pourquoi il oscille
  sur un payoff discontinu, et pourquoi on démarre par des pas Rannacher).
- Décroissance de l'erreur MC en 1/√N, et gain effectif du QMC.

**Parités et bornes de non-arbitrage comme tests**
- Put-call parity, put-call symmetry, bornes de monotonie et de convexité en strike,
  in + out = vanille pour les barrières, croissance de la variance totale en maturité.
  Ce sont des tests gratuits, très discriminants, et ils montrent que tu penses en
  financier et pas seulement en développeur.

**Benchmarks reproductibles**
- `google-benchmark` est déjà là ([benchmarks/](benchmarks/)). L'étendre.
- Publier un tableau chiffré avec le matériel exact (CPU, 2× V100 16 GB, versions).
- Métrique : **temps pour atteindre une erreur cible**, jamais « chemins/seconde » seul.

**Le README** — *l'action la plus rentable de toute la liste*

Il fait aujourd'hui 16 octets. C'est le seul fichier dont on est certain qu'un
recruteur l'ouvrira. Il doit contenir, dans cet ordre :
1. Une phrase qui dit ce que c'est et ce qui est notable.
2. **Les résultats chiffrés** (tableau de benchmark, écarts de validation vs QuantLib,
   ratio de coût AAD vs bump).
3. Un exemple de code de dix lignes qui price quelque chose.
4. L'architecture en un schéma.
5. Les références bibliographiques implémentées.
6. Les limites connues, assumées explicitement.

Le point 6 compte plus qu'on ne croit : un README qui dit « le QE d'Andersen est
implémenté mais je n'ai pas traité le cas κ→0 » inspire nettement plus confiance
qu'un README qui ne promet que des succès.

---

## 5. Séquencement proposé sur six mois

| Période | Chantier | Livrable vérifiable |
|---|---|---|
| Mois 1 | Chantier 0 : calibration + conventions + bootstrap multi-courbe | Calibration Hull-White sur swaptions, RMSE publié |
| Mois 1–2 | Chantier 1a/1b : Heston (COS + QE) et SABR | Valeurs de référence Albrecher/Andersen reproduites, surface calibrée sur données réelles |
| Mois 2–3 | Chantier 2 : AAD | Tableau coût AAD vs bump sur panier 50 actifs |
| Mois 3–4 | Chantier 3 : portage CUDA, puis multi-GPU | Benchmark 1 vs 2 V100, reproductibilité bit-à-bit démontrée |
| Mois 4 | Chantier 1c : rough Bergomi (schéma hybride) sur GPU | Pente ATM en T^(H−1/2) retrouvée numériquement |
| Mois 4–5 | Chantier 5 option A : calibration neuronale du rough Bergomi | Calibration en millisecondes, erreur sous le bid-ask |
| Mois 5–6 | Chantier 4 : crédit → exposition → CVA, puis sensibilités CVA par AAD | Profils EE/PFE sur portefeuille de swaps nettés, greeks CVA adjoints |

**Ordre non négociable** : chantier 0 avant 1 (rien ne se calibre sinon), 2 et 3 avant 4
(le xVA sans GPU ni AAD reste un jouet), LSMC avant 4c.

---

## 6. Utilisation concrète des deux V100

| Usage | Charge | Pourquoi ces cartes conviennent |
|---|---|---|
| MC de pricing en double précision | Vanilles, exotiques, paniers | FP64 à 1:2 — rare et déterminant |
| Moteur d'exposition xVA | 10⁴–10⁶ repricings imbriqués | Massivement parallèle, l'usage industriel type |
| Rough Bergomi | Schéma hybride, très coûteux | Devient praticable uniquement sur GPU |
| Génération du dataset de calibration neuronale | Millions de surfaces simulées | Le goulot est la génération, pas l'entraînement |
| Entraînement des réseaux (chantier 5) | Deep hedging / calibration | Tensor Cores FP16, 32 GB cumulés |

Deux cartes plutôt qu'une, c'est aussi ce qui rend le sujet **multi-GPU** légitime :
partitionnement de la séquence quasi-aléatoire, réduction déterministe, scaling mesuré.
Le sujet a bien plus de valeur que le simple facteur deux de performance.

---

## 7. Ce qu'il ne faut pas faire

- **Ajouter des produits.** Tu en as déjà plus que nécessaire. Le rendement marginal
  est nul, et ça donne l'impression d'un catalogue plutôt que d'une compétence.
- **Empiler des modèles sans en calibrer aucun.** Un Heston non calibré vaut moins
  qu'un Black-Scholes calibré.
- **Annoncer l'AAD sur le CV avant de l'avoir écrit.** L'état actuel, c'est du
  bump-and-reprice avec des macros préparatoires. La question tombera en entretien,
  et il vaut mieux qu'elle tombe sur du code qui existe.
- **Faire du ML pour faire du ML.** Chaque brique ML doit être comparée à sa
  référence classique, avec barres d'erreur, et le cas où elle perd doit être
  documenté.
- **Négliger le README et les benchmarks** au profit du code. À ce stade du projet,
  la valeur marginale d'une heure de README dépasse celle d'une heure de code.

---

## 8. Bibliographie de travail

| Sujet | Référence |
|---|---|
| AAD | Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley 2018 |
| Vol stochastique, smile | Gatheral, *The Volatility Surface*, Wiley 2006 |
| Heston par Fourier | Fang & Oosterlee, « A Novel Pricing Method... COS », 2008 ; Albrecher et al., « The Little Heston Trap », 2007 |
| Simulation de Heston | Andersen, « Efficient Simulation of the Heston Stochastic Volatility Model », 2008 |
| SABR | Hagan et al., « Managing Smile Risk », 2002 ; « Arbitrage-free SABR », 2014 |
| Vol rugueuse | Gatheral, Jaisson, Rosenbaum, « Volatility is Rough », 2018 ; Bennedsen, Lunde, Pakkanen (schéma hybride), 2017 |
| Monte-Carlo | Glasserman, *Monte Carlo Methods in Financial Engineering*, Springer 2003 |
| Taux | Andersen & Piterbarg, *Interest Rate Modeling*, 3 volumes |
| xVA | Green, *XVA: Credit, Funding and Capital Valuation Adjustments* ; Gregory, *The xVA Challenge* |
| Deep hedging | Buehler, Gonon, Teichmann, Wood, « Deep Hedging », 2019 |
| Calibration neuronale | Horvath, Muguruza, Tomas, « Deep Learning Volatility », 2019 |
| Arrêt optimal par réseaux | Becker, Cheridito, Jentzen, « Deep Optimal Stopping », 2019 |

---

## 9. Résumé en une phrase

Le projet a déjà la largeur ; il lui manque la profondeur sur trois axes —
**vol stochastique calibrée**, **greeks adjoints**, **Monte-Carlo GPU** — qui
convergent naturellement vers un capstone xVA. C'est cette convergence, et non le
nombre de produits, qui rend le projet difficile à ignorer sur un CV.
