# WP 19 — Monte-Carlo sur GPU (les deux V100)

> Chantier 3 de la [roadmap](../../etc/roadmap.md). Conception, pas code : les
> lots G0–G4 ([§11](#11-séquencement)) livrent chacun un critère vérifiable.
> Dépend de : [WP 16](16-scripting.md) (scripts), [WP 17](17-aad.md) (AAD),
> le moteur de simulation générique (`engines/mc/simulation_engine.hpp`).
> Référence de méthode Monte-Carlo : **Glasserman, *Monte Carlo Methods in
> Financial Engineering*, Springer 2004** — les numéros de chapitre ci-dessous
> sont les siens.

---

## 0. L'acquis — et ce qui ne l'est qu'en apparence

| Élément | État |
|---|---|
| `QM_HOST_DEVICE` ([`core/platform.hpp`](../../include/quantModeling/core/platform.hpp)) | Macro de **compilation** : `__host__ __device__` sous nvcc, rien sinon. Aucune détection du GPU à l'exécution. |
| Kernels `vanilla_bs.hpp`, `asian_bs.hpp` | Fonctions libres sur agrégats plats, sans virtuel ni allocation : **compilables** par nvcc, jamais compilées. Aucun fichier `.cu`. |
| `inverse_normal.hpp` | Acklam + un pas de Halley, `QM_HOST_DEVICE`, sans état : prêt pour le GPU. |
| `SobolSequence` | Joe-Kuo *new-joe-kuo-6*, **1 024 dimensions**, code de Gray, décalage digital aléatoire (XOR), `skip_to(n)` en O(32·d). Classe hôte (`std::vector`). |
| `Pcg32` | O'Neill 2014, flux indépendants, `skip(n)` en O(log n). À état : un générateur par thread. |
| Pont brownien (Jäckel) | Dans les **moteurs dédiés** seulement ; le moteur générique des scripts consomme les gaussiennes dans l'ordre du temps. |
| Réduction de variance | Antithétique (moteur générique) ; variable de contrôle (asiatique géométrique) ; stratification et échantillonnage préférentiel (moteur BS dédié). |
| Matériel | 2 × Tesla V100-PCIE 16 Go, `sm_70`, FP64 à 1:2 ; CUDA 12.4, pilote 550. **CUDA 12.x est la dernière branche qui supporte les V100** : on n'en sort pas. |

Ce qui ne passe **pas** tel quel sur GPU, et pourquoi (un GPU exécute les
threads par *warps* de 32 qui avancent ensemble ; il craint les appels
virtuels, l'allocation dynamique et les pointeurs qu'on suit de proche en
proche) :

- l'**AST des scripts** : nœuds `unique_ptr`, évaluateur visiteur ;
- les **modèles** : `ISimulationModel` virtuel, `std::vector`, `clone()` ;
- la **tape AAD** : liste de nœuds allouée par blocs, une par thread CPU — à
  10⁵ threads, la mémoire explose.

## 1. Principe

**Compiler sur l'hôte, exécuter du plat sur le device.** Tout le travail
« intelligent » (parse, analyse, calibration) reste sur le CPU ; le GPU reçoit
des données plates — bytecode du script, paramètres du modèle, directions de
Sobol — et un kernel serré les consomme. Le découpage du dépôt est préservé : un
nouveau **moteur** `GpuSimulationEngine`, qui ne connaît aucun produit.

Chaque chemin est une fonction **pure** de (graine, indice du chemin) : c'est
ce qui rend le calcul reproductible bit à bit quel que soit le découpage
([§7](#7-reproductibilité-bit-à-bit)) et ce qui permet à l'AAD de régénérer
les tirages au lieu de les stocker ([§6](#6-aad-sur-gpu)).

---

## 2. L'aléatoire

### 2.1 Ce que Glasserman impose, et comment on le tient

| Exigence (Glasserman) | Réponse |
|---|---|
| Un QMC n'est bon que si ses **premières coordonnées portent la variance** (ch. 5.5, dimension effective) | Pont brownien dans le **moteur générique** (lot G1), pour le CPU comme le GPU : la coordonnée 1 donne W(T) de chaque facteur, puis les milieux ([§2.4](#24-le-pont-brownien-dans-le-moteur-générique)) |
| **Gaussiennes par inverse de la fonction de répartition**, jamais Box-Muller, en QMC (ch. 5.2) | `inverse_normal` pour les deux échantillonneurs sur GPU : un seul chemin de code, et la stratification en profite |
| **QMC randomisé** pour avoir une barre d'erreur (ch. 5.4) | B décalages digitaux indépendants → B moyennes i.i.d. → erreur standard de Student. Inchangé, simplement lancé en un seul kernel |
| **Flux indépendants** entre processeurs (ch. 2.1) | Générateurs **adressables** : le point n de la dimension d ne dépend que de (graine, n, d) — pas de flux partagé, pas de synchronisation |
| Réduction numérique stable sur 10⁸ termes | Welford par thread, fusion de Chan pour les réductions ([§5](#5-réduction-et-estimateurs)) |

### 2.2 Sobol sur GPU : les nôtres, pas ceux de cuRAND

Le point n de Sobol se calcule **directement** : x_n = ⊕_k [bit k de gray(n)]·V_k
(c'est déjà notre `skip_to`). Un thread qui traite les chemins n, n+1, …, n+m
calcule le premier par cette formule, puis passe au suivant par la mise à jour
de Gray (un XOR par dimension) — la même arithmétique **entière** que le CPU,
donc les mêmes bits.

- **Directions** : les nôtres (Joe-Kuo *new-joe-kuo-6*), en mémoire globale en
  lecture seule (`__ldg`) : 32 entiers × d dimensions dépassent vite les 64 Ko
  de mémoire constante.
- **Pourquoi pas cuRAND** : ses directions et son brouillage diffèrent des
  nôtres ; le GPU ne reproduirait plus le CPU, et nos tests de convergence
  seraient à refaire. cuRAND reste un comparatif de benchmark ([ADR-G2](#adr-g2--nos-sobol-pas-ceux-de-curand)).
- **Randomisation** : le décalage digital actuel (XOR par dimension, tiré d'un
  PCG32 par réplique). Le brouillage d'Owen (ch. 5.4) donne une meilleure
  variance sur les intégrandes lisses ; il est noté hors périmètre v1
  ([§12](#12-hors-périmètre)).
- **La limite des 1 024 dimensions** saute : le fichier Joe-Kuo complet en va
  jusqu'à 21 201. Il faut la lever — un produit quotidien d'un an en SLV
  demande 2 facteurs × ~300 pas, déjà plus de 600 ; avec le pont brownien, les
  dimensions au-delà des premières ne portent presque rien, mais elles doivent
  exister.

### 2.3 Pseudo-aléatoire : Philox, à compteur

Un générateur à **état** (PCG32, Mersenne Twister) impose un état par thread
et un « saut » pour les répartir. Un générateur **à compteur** — Philox4x32-10
(Salmon, Moraes, Dror, Shaw, *Parallel Random Numbers: As Easy as 1, 2, 3*, SC 2011)
— n'a pas d'état : u = Philox_clé(compteur), clé = graine, compteur =
(indice du chemin, indice du tirage). C'est exactement la propriété voulue.

- On l'écrit **nous-mêmes** en `QM_HOST_DEVICE` (une trentaine de lignes, les
  vecteurs de test de Random123 en oracle), pour que le CPU produise les mêmes
  nombres que le GPU. Le moteur générique CPU gagne l'option `philox` ;
  `pcg32` reste le défaut des moteurs existants (pas de changement de nombres
  sous les pieds des tests actuels).
- Uniformes sur **52 bits** (deux mots de 32), u = (m + ½)/2⁵², puis
  `inverse_normal`. Pas 53 : (2⁵³ − 1) + ½ n'est pas un double et s'arrondit
  à 2⁵³, soit u = 1 et Φ⁻¹(u) = +∞ — le test de l'intervalle ouvert l'a
  attrapé au lot G0. Les tirages extrêmes valent ±8,2σ.

### 2.4 Le pont brownien dans le moteur générique

Aujourd'hui un modèle demande ses gaussiennes pas par pas ; en Sobol, la
coordonnée d du point sert au pas d, et les dernières dates (les plus
importantes pour un payoff à maturité) reçoivent les coordonnées les moins
bien réparties. Le lot G1 insère la construction de Jäckel **entre la source et
le modèle** :

1. le modèle déclare ses facteurs (1 pour BS et vol locale, 2 pour Heston et
   SLV, n pour le multi-actifs) et sa grille de pas ;
2. la source produit un point de dimension (facteurs × pas) ;
3. le pont consomme les coordonnées **par ordre d'importance** : W(T) de
   chaque facteur d'abord, puis les milieux, récursivement ;
4. le modèle reçoit ses incréments **dans l'ordre du temps**, comme avant : il
   ne voit pas la différence.

Invisible pour le pseudo-aléatoire (les incréments restent i.i.d.), décisif
pour le QMC : c'est ce qui rend Sobol utile sur les produits à trajectoire.
Corrélation (Heston, multi-actifs) appliquée **après** le pont, par la
factorisation de Cholesky déjà présente dans les modèles.

### 2.5 La réduction de variance, technique par technique

| Technique (Glasserman) | Sur GPU | Pour les scripts |
|---|---|---|
| **Antithétique** (4.2) | Un thread évalue z et −z ; l'estimateur est la moyenne de la **paire** (les paires sont i.i.d., pas les chemins) | Générique, gardé |
| **Variables de contrôle** (4.1) | Le kernel accumule par réplique Y, X, XY, X², Y² (Welford) ; β* = Cov(X,Y)/Var(X) estimé à la réduction. Biais en O(1/n) : négligeable à nos tailles, ou estimé sur un pilote séparé si l'on veut l'éliminer | **Contrôle générique** : le spot actualisé à chaque date d'événement, martingale de moyenne connue S₀e^{−qt} ; en vol locale, le même payoff sous Black-Scholes quand il a une forme fermée |
| **Stratification** (4.3) | Stratification proportionnelle de la **valeur terminale** (la première coordonnée du pont) : chemin i dans la strate ⌊i·K/n⌋, remplissage par le pont. Batches de strates → erreur par répliques | Générique grâce au pont |
| **Échantillonnage préférentiel** (4.6) | Changement de drift gaussien + poids de vraisemblance par chemin, dans le kernel | Le drift optimal dépend du payoff : v1 garde celui du moteur BS dédié ; générique hors périmètre ([§12](#12-hors-périmètre)) |
| **QMC randomisé** (5.4) | [§2.2](#22-sobol-sur-gpu--les-nôtres-pas-ceux-de-curand) | Générique |

Les techniques se composent (Sobol + pont + antithétique + contrôle) ; chaque
combinaison publie son **erreur standard mesurée** — la métrique du benchmark
est le temps pour atteindre une erreur donnée, pas le nombre de chemins
par seconde ([§9](#9-benchmark)).

---

## 3. Les scripts : un bytecode

L'arbre du script (après les passes existantes : indexation des variables,
conditions constantes, `if`, domaines) est traduit sur l'hôte en **bytecode**
d'une machine à pile : un tableau d'instructions (`PUSH_SPOT a`, `PUSH_VAR i`,
`PUSH_CONST c`, `ADD`, `MUL`, `MAX`, `LOG`, `CMP_GT`, `JUMP_IF_FALSE o`,
`ASSIGN i`, `PAY`…), un tableau de constantes, et pour chaque événement la
plage d'instructions à exécuter. Un thread = un chemin : il simule le chemin,
et à chaque date d'événement interprète la plage correspondante ; ses variables
(leur nombre est connu) tiennent en registres ou en mémoire locale.

- **D'abord sur CPU** (lot G2) : l'interpréteur de bytecode remplace
  l'évaluateur visiteur, se teste dans la CI (qui n'a pas de GPU) et doit
  donner **les mêmes nombres** que l'arbre sur les 42 scripts de la
  bibliothèque. Puis le même interpréteur, `QM_HOST_DEVICE`, sur le GPU.
- **Divergence** : un `if` qui part dans des sens différents selon les chemins
  d'un warp sérialise le warp. La **logique floue** (WP 16c) évalue les deux
  branches pondérées : plus de branchement, plus de divergence. En mode dur,
  la divergence est réelle mais bornée — les scripts ont peu de branches.
- **Écart au livre** : Andreasen & Savine évaluent l'arbre par visiteur ; une
  forme compilée est l'optimisation naturelle et, sauf vérification contraire
  à la rédaction du lot, le livre en décrit le principe. Consigné en
  [ADR-G1](#adr-g1--un-bytecode-pas-larbre-sur-le-device).

## 4. Les modèles : un ensemble fermé

Chaque modèle devient un agrégat de paramètres plats avec
`QM_HOST_DEVICE step(état, incréments, dt)` — sans virtuel, sans allocation :

| Modèle | Facteurs | Données sur le device |
|---|---|---|
| Black-Scholes (mono, multi-actifs) | 1, n | spots, vols, Cholesky |
| Vol locale | 1 | grille (K, T) de σ_loc, recherche binaire par pas |
| Heston | 2 | 5 paramètres, schéma à troncature complète (le QE d'Andersen en variante) |
| SLV | 2 | Heston + grille de levier |

Le choix se fait au lancement (un `switch` sur une énumération → une
instanciation de kernel par modèle), comme `make_script_model` aujourd'hui.

## 5. Réduction et estimateurs

Par thread : un accumulateur de Welford (et les moments croisés des
variables de contrôle). Puis warp (`__shfl_down_sync`), bloc (mémoire
partagée), grille (un tableau de partiels par bloc) — chaque étage fusionne
des Welford (formule de Chan), jamais une somme naïve. Les chemins ne quittent
jamais le device : seuls les accumulateurs remontent.

## 6. AAD sur GPU

Pas de tape générique sur le device. Deux mécanismes, selon le nombre de
paramètres :

1. **Mode direct (nombres duaux)** pour les modèles à peu de paramètres
   (Black-Scholes, Heston : < 10) : un type `Dual<N>` (valeur + N dérivées),
   `QM_HOST_DEVICE`. Le code étant templé sur le type numérique `T` depuis le
   WP 17, il se branche naturellement ; coût ∝ N, sans mémoire.
2. **Adjoint chemin par chemin** pour les milliers de paramètres de la vol
   locale (donc le superbucket) : en aller, chaque thread stocke l'état de son
   chemin à chaque pas (≈ 4 Ko pour 500 pas ; 400 Mo pour 10⁵ chemins) ; au
   retour, il remonte avec l'**adjoint écrit à la main** du pas de chaque
   modèle (quatre, une fois pour toutes) et l'**adjoint du bytecode**, généré
   mécaniquement (chaque instruction a son adjointe). Les tirages ne sont pas
   stockés : Philox / Sobol les **régénèrent** depuis (graine, chemin). Les
   contributions à la grille s'accumulent par `atomicAdd` en double (natif
   depuis `sm_60`) — chaque pas ne touche que les quatre coins de sa case.

**Oracle** : l'AAD CPU du WP 17. Chaque risque GPU doit l'égaler à l'erreur
Monte-Carlo près ; le bug de vol locale corrigé au lot 17h a montré que cet
oracle indépendant n'est pas un luxe.

## 7. Reproductibilité bit à bit

Exigence de desk (roadmap, chantier 3) : le même chiffre sur 1 ou 2 GPU, et le
même que le CPU avec le même générateur.

- Chaque chemin est une fonction pure de (graine, indice) ([§2](#2-laléatoire)).
- **L'ordre des additions est fixé par les indices, pas par le matériel** : les
  chemins sont groupés en blocs logiques de taille fixe (par exemple 4 096) ;
  chaque bloc logique est réduit par le même arbre ; les partiels sont fusionnés
  dans l'ordre des indices, sur l'hôte. Deux GPU se partagent des blocs logiques
  entiers : le résultat ne dépend pas de leur nombre.
- Le CPU applique le même groupement quand on lui demande Philox
  ([`engines/mc/logical_blocks.hpp`](../../include/quantModeling/engines/mc/logical_blocks.hpp)
  rejoue l'arbre du kernel). Les **tirages** sont alors identiques bit à bit
  (arithmétique entière), mais les **résultats** CPU et GPU diffèrent de
  quelques ulp (~10⁻¹⁶ relatif) : `exp`, `log` et `erfc` de libdevice ne sont
  pas ceux de la glibc, et nvcc contracte `a*b + c` en FMA. Mesuré au lot G0
  (vanille, 1,6·10⁷ chemins : 1 à 3 ulp sur la moyenne). Le bit à bit tient
  donc **entre GPU** (même code machine) — 1 GPU = 2 GPU, un ou plusieurs
  lancements — et le CPU est un oracle à 10⁻¹² près, bien sous l'erreur
  Monte-Carlo. Forcer l'égalité exacte demanderait `--fmad=false` et nos
  propres `exp`/`log` sur les deux cibles : pas justifié.

## 8. Intégration

- **CMake** : option `QM_ENABLE_CUDA` (défaut OFF), `CMAKE_CUDA_ARCHITECTURES=70`,
  sources `.cu` dans `src/gpu/` ; sans l'option, le dépôt compile comme
  aujourd'hui (`src/gpu/cpu_only.cpp` répond « aucun device »). Preset
  `cuda` → `build-cuda/`. CUDA 12.4 refuse gcc 14 : nvcc compile avec
  **g++-13** comme hôte, le reste avec le g++ du projet, et le dossier
  `libstdc++` de gcc 13 est retiré de l'édition de liens.
- **Réglages** (`PricingSettings`) : `mc_rng` (`Pcg32` par défaut, `Philox`)
  et `mc_device` (`Cpu` par défaut, `Gpu`, `Auto`). `Gpu` refuse de se
  replier ; `Auto` prend le GPU si un device est présent et que le moteur
  sait y faire la requête. Défauts inchangés : aucun nombre existant ne bouge.
- **Détection à l'exécution** : `cudaGetDeviceCount` à la première requête ;
  le registre route vers `GpuSimulationEngine` si un device est présent et que
  le produit / modèle y est supporté, sinon le CPU. Le choix est dit dans les
  diagnostics de la réponse et enregistré dans l'audit (moteur, nombre de
  GPU).
- **Mémoire** : les cartes sont partagées (l'assistant de scripting peut en
  prendre). Le nombre de chemins par lancement se **calcule** depuis
  `cudaMemGetInfo` et le coût par chemin (état AAD compris), jamais par défaut.
- **API / front** : un champ `engine` (`auto` / `cpu` / `gpu`) ; la page de
  scripting et la comparaison de modèles affichent le temps de calcul et le
  moteur.

## 9. Benchmark

Le livrable qui compte. Tableau publié dans le README :

| | CPU 1 thread | CPU 8 threads | 1 V100 | 2 V100 |
|---|---|---|---|---|
| Vanille BS | … | … | … | … |
| Up-and-out quotidien, vol locale | … | … | … | … |
| Worst-of autocall, 3 actifs | … | … | … | … |
| Superbucket (AAD vol locale) | … | … | … | … |

Premier point (lot G0, `build-cuda/qm_gpu_bench`) — call ATM, S = K = 100,
T = 1, σ = 20 %, antithétique, Philox, les six estimateurs du kernel (prix,
delta trajectoriel, vega et rho LRM, gamma et theta par différences à
nombres communs) ; 6,2·10⁷ paires pour 1e-4 relatif :

| | CPU 1 thread | CPU 8 threads | 1 V100 |
|---|---|---|---|
| Vanille BS (pseudo, Philox) | 8,92 s | 1,30 s | 0,032 s |

Même prix aux trois colonnes (9,226547, erreur 9,2·10⁻⁴). Le kernel ne lit
rien en mémoire : il est limité par le **calcul FP64** (Φ⁻¹, trois `exp` par
chemin), ce qui est le régime où les V100 (FP64 à 1:2) ont leur avantage.

Métrique : **temps pour atteindre une erreur standard de 1e-4** (en relatif),
par échantillonneur (pseudo / Sobol + pont) — un speedup sans erreur standard
ne vaut rien. Chaque ligne indique aussi ce qui limite le kernel (bande
passante ou calcul, mesuré au profileur `nsys` / `ncu`).

## 10. Tests

- Philox : vecteurs de test de Random123 ; Sobol GPU = Sobol CPU bit à bit sur
  les 21 201 dimensions.
- Pont brownien : covariance empirique des W(tᵢ) = min(tᵢ, tⱼ) ; en Sobol,
  ordre de convergence mesuré en log-log sur un asiatique (≈ n⁻¹ contre n⁻¹ᐟ²).
- Bytecode : les 42 scripts, même prix que l'arbre (tolérance nulle en
  pseudo-aléatoire à graine égale).
- GPU = CPU à l'erreur Monte-Carlo près ; 1 GPU = 2 GPU bit à bit.
- AAD GPU = AAD CPU à l'erreur près ; somme des vegas par cotation = choc
  parallèle (le test du lot 17h, sur GPU).
- La CI GitHub n'a pas de GPU : elle compile sans CUDA et teste tout ce qui
  est CPU (Philox, pont, bytecode) ; les tests GPU tournent sur le serveur
  (`ctest -L gpu`).

## 11. Séquencement

| Lot | Contenu | Acceptation |
|---|---|---|
| **G0** | CMake CUDA ; Philox hôte/device ; réduction Welford ; le kernel vanille existant sur GPU | Prix GPU = prix CPU ; premier point du benchmark — **fait** : tirages identiques bit à bit, prix à quelques ulp ([§7](#7-reproductibilité-bit-à-bit)), bit à bit entre les deux V100 ; 280× un thread CPU ([§9](#9-benchmark)) |
| **G1** | Pont brownien dans le moteur générique (CPU) ; directions Joe-Kuo jusqu'à 21 201 ; Sobol device ; modèles en foncteurs | Sobol + pont bat le pseudo-aléatoire en log-log sur un asiatique ; mêmes lois CPU / GPU |
| **G2** | Compilateur et interpréteur de bytecode, CPU puis GPU | Les 42 scripts : même prix arbre / bytecode / GPU |
| **G3** | Variables de contrôle et stratification génériques ; AAD : duaux, puis adjoint par chemin en vol locale | Réduction de variance mesurée ; risques GPU = AAD CPU ; superbucket sur GPU |
| **G4** | Deux GPU, reproductibilité bit à bit, benchmark complet | 1 GPU = 2 GPU bit à bit ; tableau publié |

## 12. Hors périmètre

- Brouillage d'Owen (meilleur que le décalage digital sur intégrandes lisses) :
  après G4, si le benchmark le justifie.
- Échantillonnage préférentiel générique (drift optimal par payoff).
- AAD d'ordre 2 sur GPU.
- Rough Bergomi et calibration neuronale : chantiers 1c et 5, qui
  s'appuieront sur ce moteur.
- `float` : tout reste en `double` (FP64 à 1:2 sur V100 — c'est l'argument
  même de ces cartes).

## 13. Décisions

### ADR-G1 — Un bytecode, pas l'arbre, sur le device

**Décision.** Le script est compilé en bytecode de machine à pile, interprété
par un kernel. **Pourquoi.** Un arbre de nœuds alloués, parcouru par visiteur,
fait tout ce qu'un GPU craint (pointeurs, virtuels, allocation). Le bytecode
est plat, indexé, identique CPU / GPU. **Écart au livre** : le livre évalue
l'arbre ; la compilation est une optimisation dont le principe est à confirmer
dans le livre à la rédaction du lot G2 — l'évaluateur visiteur reste l'oracle.

### ADR-G2 — Nos Sobol, pas ceux de cuRAND

**Décision.** Directions Joe-Kuo et décalage digital du dépôt, calculés sur le
device. **Pourquoi.** Bits identiques au CPU (tests, reproductibilité) ; cuRAND
n'offre que ses propres directions et son brouillage. **Alternative** : cuRAND,
gardé comme comparatif de performance.

### ADR-G3 — Philox, générateur à compteur

**Décision.** Philox4x32-10 écrit dans le dépôt, hôte et device. **Pourquoi.**
Pas d'état : un tirage est fonction de (graine, chemin, rang) — parallélisme
sans coordination, reproductibilité indépendante du découpage, et régénération
des tirages dans l'adjoint. **Alternative rejetée** : PCG32 avec `skip` par
thread (un état par thread, et le saut à refaire à chaque découpage).

### ADR-G4 — Pas de tape générique sur le device

**Décision.** Duaux pour peu de paramètres, adjoint écrit à la main (pas de
modèle + bytecode) pour beaucoup. **Pourquoi.** Une tape par thread à 10⁵
threads ne tient pas en mémoire ; les modèles sont en nombre fermé, leur
adjoint s'écrit une fois. L'AAD CPU (WP 17) reste l'oracle.

### ADR-G5 — L'ordre des additions fixé par les indices

**Décision.** Blocs logiques de taille fixe, arbre de réduction fixe, fusion
dans l'ordre des indices. **Pourquoi.** L'addition flottante n'est pas
associative : sans cela, le résultat dépendrait du nombre de GPU et de blocs.

## 14. Bibliographie

| Sujet | Référence |
|---|---|
| Monte-Carlo, QMC, réduction de variance | Glasserman, *Monte Carlo Methods in Financial Engineering*, Springer, 2004 (ch. 2 générateurs, 3.1 pont brownien, 4 réduction de variance, 5 QMC) |
| Directions de Sobol | Joe & Kuo, « Constructing Sobol Sequences with Better Two-Dimensional Projections », *SIAM J. Sci. Comput.* 30(5), 2008 |
| Générateur à compteur | Salmon, Moraes, Dror & Shaw, « Parallel Random Numbers: As Easy as 1, 2, 3 », *SC'11*, 2011 |
| Pont brownien | Jäckel, *Monte Carlo Methods in Finance*, Wiley, 2002 |
| Brouillage | Owen, « Randomly Permuted (t,m,s)-Nets and (t,s)-Sequences », *Monte Carlo and Quasi-Monte Carlo Methods in Scientific Computing*, Springer, 1995 |
| Scripting, AAD | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley, 2021 ; Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley, 2018 |
