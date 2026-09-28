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
  jusqu'à 21 201. **Fait (G1)** : les 354 613 entiers sont dans
  `src/utils/sobol_directions.cpp` (généré, compilé une fois ; l'en-tête ne
  garde que les déclarations), et le point n se calcule directement
  (`sobol_point_bits`) — le même code sur le device, identique au CPU bit à
  bit sur les 21 201 dimensions. Le moteur vanille GPU tourne aussi en Sobol
  RQMC (une réplique par lancement, les directions et le décalage de la
  réplique CPU). Il faut la lever — un produit quotidien d'un an en SLV
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

**Fait (G1).** Chaque modèle décrit ses tirages par un `BrownianLayout`
(dates des pas, nombre de facteurs browniens, tirages par pas) ;
`BridgedGaussians` (`utils/brownian_bridge.hpp`) lit le point de Sobol par
ordre d'importance — `W_f(T)` de chaque facteur, puis les milieux, puis les
autres tirages (sauts) dans l'ordre du temps — et rend au modèle ses
incréments normalisés dans l'ordre du temps. Décrits : Black-Scholes,
multi-actifs, vol locale, SLV, SABR, Bates (donc Heston), Merton, Kou ;
rough Bergomi (schéma hybride, vecteurs gaussiens corrélés) garde l'ordre du
temps. Actif en Sobol seulement (`mc_brownian_bridge`, vrai par défaut). Sur
un asiatique arithmétique à 52 fixings (erreur RQMC, 32 répliques) :

| 2¹⁷ chemins | erreur standard | pente log-log |
|---|---|---|
| Sobol + pont | 1,49·10⁻³ | −0,89 |
| Sobol, ordre du temps | 3,55·10⁻³ | −0,80 |
| Pseudo-aléatoire | 2,79·10⁻² | −0,50 |

soit ~350 fois moins de chemins que le pseudo-aléatoire pour la même
erreur. Contrôles : asiatique géométrique à 52 dates contre sa formule
fermée à 10⁻³ près, Merton (sauts transmis tels quels) contre la série de
Merton.

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

**Fait (G3).** Deux techniques génériques, pour tout script, sur CPU comme sur
GPU — mêmes unités, mêmes tirages Philox, mêmes estimateurs : le GPU redonne les
nombres du CPU à 10⁻⁹ (`tests/gpu/testGpuRisks.cpp`).

- **Variables de contrôle** (`mc_spot_control`, champ `control_variate` de
  l'API). Le spot actualisé S_a(t_e)/N(t_e) à au plus 8 dates d'événement
  (réparties, la dernière incluse ; avec plusieurs actifs, la dernière de
  chacun) : une martingale de moyenne connue S₀e^{−qt}, que les pas log-Euler
  des modèles conservent exactement sur la grille
  (`ISimulationModel::deflated_spot_mean` ; Black-Scholes mono et multi,
  vol locale, Heston/Bates, SLV). β par **régression multiple** (Glasserman
  §4.1.3) : Welford multivarié, fusionné par la formule de Chan dans l'arbre de
  réduction (`utils/variance_reduction/multi_control.hpp`) ; erreur standard de
  la régression au point X = μ, s²(1/n + (X̄−μ)ᵀS_xx⁻¹(X̄−μ)). Un contrôle
  colinéaire aux autres est écarté par la factorisation. Le diagnostic de la
  réponse donne la variance retirée, mesurée sur le run.
- **Stratification de W(T)** (`SamplerKind::Stratified`, `sampler:
  "stratified"`). La valeur terminale du premier facteur tombe dans la strate
  i de m, le reste du chemin est tiré par le **pont conditionnel séquentiel**
  de (t, W(t)) à (T, W(T)) (Glasserman §4.3.2, `engines/mc/path_draws.hpp`) :
  O(1) mémoire par chemin au lieu du tableau d'une bissection, et le modèle
  reçoit toujours ses incréments dans l'ordre du temps. Un chemin par strate ;
  16 répliques indépendantes donnent l'erreur. Sur GPU, toutes les répliques
  partagent une grille (bloc g = bloc logique g mod b de la réplique g / b) :
  à 10⁵ chemins une réplique ne fait qu'un bloc logique, et 16 lancements
  successifs occupaient 1 SM sur 80 (10 fois plus lent que le run simple).
- **Les deux ensemble.** Sous stratification, la pente utile est la pente
  *intra-strate* ; la pente ordinaire (covariance totale) dégradait l'autocall
  (variance ÷ 3,4 stratifié seul, ÷ 2,3 avec contrôle). Elle est estimée par
  différences successives entre strates voisines ([ADR-G7](#adr-g7--sous-stratification-la-pente-intra-strate)).

Mesure (`build-cuda/qm_gpu_vr_bench`, vol locale 52 pas/an, 10⁵ chemins,
40 graines) : rapport de variance au run simple de même réglage à nombre de
chemins égal (d'après les erreurs rapportées, moyennées sur les graines) et, entre
parenthèses, **gain en temps pour une erreur donnée** contre le réglage par
défaut (antithétique seul), erreur² × temps :

| Avec paires antithétiques | Stratifié | Contrôle | Les deux |
|---|---|---|---|
| Call à départ différé | ÷1,33 (×1,14) | ÷1,20 (×0,58) | ÷1,33 (×0,54) |
| Asiatique, 12 fixings | ÷1,11 (×1,11) | ÷1,16 (×0,86) | ÷1,15 (×0,74) |
| Phoenix autocall | ÷2,79 (×3,39) | ÷1,81 (×1,64) | ÷2,71 (×1,90) |
| Up-and-out quotidien | ÷1,20 (×1,47) | ÷1,08 (×0,99) | ÷1,21 (×1,26) |
| Variance swap (249 dates) | ÷1,53 (×1,79) | ÷1,39 (×1,20) | ÷1,92 (×1,87) |

| Sans paires antithétiques | Stratifié | Contrôle | Les deux |
|---|---|---|---|
| Call à départ différé | ÷2,14 (×0,57) | ÷4,10 (×0,82) | ÷6,10 (×1,01) |
| Asiatique, 12 fixings | ÷1,64 (×0,73) | ÷3,99 (×1,33) | ÷4,91 (×1,33) |
| Phoenix autocall | ÷2,41 (×2,74) | ÷1,27 (×1,05) | ÷2,47 (×1,75) |
| Up-and-out quotidien | ÷1,43 (×1,68) | ÷1,43 (×1,54) | ÷1,43 (×1,44) |
| Variance swap (249 dates) | ÷4,20 (×1,02) | ÷13,8 (×3,09) | ÷16,0 (×3,36) |

Ce que le tableau dit, et qui n'était pas écrit dans le plan :

- **Contrôles et paires antithétiques font double emploi.** Une paire (z, −z)
  annule déjà la partie du payoff linéaire en z, celle que les spots
  actualisés capturent : avec les paires, les contrôles retirent peu, et leur
  accumulateur (55 mots par unité au lieu de 3) coûte jusqu'à ×2 le temps d'un
  script court. Sans les paires, ils sont le meilleur outil du tableau sur les
  payoffs quasi linéaires en spots (variance swap : ×3,1 en temps). D'où le
  champ `antithetic` exposé dans l'API et la page Scripting (défaut inchangé).
- **La stratification est la plus robuste** : jamais perdante, ×1,1 à ×3,4 en
  temps avec les paires, surtout sur les payoffs où la valeur terminale décide
  (autocall, tests de fin de vie).
- Aucun défaut ne change : aucun nombre existant ne bouge.

### 2.6 Sobol sur GPU, et l'échantillonnage préférentiel (issues #102, #103)

**Sobol + pont pour les scripts sur GPU.** Ce qui manquait n'était pas Sobol
(sur le device depuis G1) mais le pont : la bissection de Jäckel veut le point
entier avant de rendre le premier incrément, soit `dim` doubles par chemin. En
tableau local, le pilote les réserverait pour tous les threads que la carte peut
porter (le piège mesuré sur les duaux, §6). Ils vivent donc dans une mémoire de
travail par thread en mémoire globale (disposition coalescée), le nombre de
blocs par lancement se calcule depuis `cudaMemGetInfo`, et les 16 répliques
partagent une grille (le lancement segmenté de G3).
`mc::sobol_bridged_gaussians` (`engines/mc/sobol_bridge.hpp`) fait sur tables
plates, en place, ce que font `SobolSequence` et `BridgedGaussians` : sur
l'hôte, **mêmes bits** que le moteur générique (test dans la CI) ; sur le
device, les mêmes nombres à 10⁻⁹ près sous Black-Scholes, vol locale, Heston et
SLV, sur les 42 scripts. Les moteurs de risques suivent : la tape CPU
(`simulate_aad`) prend Sobol (une réplique par lot, l'erreur des risques est la
dispersion des répliques), les duaux et l'adjoint vol locale GPU aussi (l'adjoint
relit les gaussiennes stockées au lieu de les régénérer par Philox) — risques GPU
= tape à l'arrondi. La vega par cotation (`market_vega`) suit l'échantillonneur
de la requête.

**Échantillonnage préférentiel générique** (`mc_importance_drift`, champ
`importance_sampling`). Glasserman, Heidelberger & Shahabuddin (1999 ;
Glasserman §4.6.2) : le drift au mode de G(z)φ(z), maximiseur de
log G(z) − |z|²/2 ; ici un drift constant par facteur brownien (le décalage du
moteur vanille dédié, pour tout script et tout modèle qui décrit ses
incréments), trouvé sur l'hôte par recherche par coordonnée puis section dorée,
un chemin — le chemin moyen décalé — par évaluation (`engines/mc/importance_sampling.hpp`).
Chaque tirage devient μ + x, pondéré par exp(−μ·x − |μ|²/2) ; le miroir
antithétique est μ − x. C'est un changement de variable de l'intégrande : il se
compose sans biais avec les paires, la stratification, Sobol et les contrôles
(pondérés). Le mode GHS n'est qu'une heuristique : un **pilote** (un huitième
du run, sa propre graine) mesure la variance avec et sans le drift, et le run ne
le garde que si elle baisse d'au moins 5 % — sinon il s'en passe et le dit. Le
pilote tire les mêmes nombres sur CPU et GPU : la décision est la même des deux
côtés.

Mesure (`build-cuda/qm_gpu_vr_bench`, mêmes réglages qu'au §2.5 ; la digitale à
180 % sous une vol locale plate de 25 %, ~0,8 % de chances) — rapport de
variance au run simple de même réglage, et gain en temps pour une erreur donnée
contre le défaut (antithétique seul) :

| Avec paires antithétiques | Sobol + pont | Importance sampling |
|---|---|---|
| Call à départ différé | ÷84 (×55) | ÷1,12 (×0,25) |
| Asiatique, 12 fixings | ÷41 (×44) | refusé par le pilote |
| Phoenix autocall | ÷10 (×12) | refusé |
| Up-and-out quotidien | ÷3,0 (×4,1) | refusé |
| Variance swap (249 dates) | ÷33 (×46) | refusé |
| Digitale à 180 % | ÷191 (×83) | ÷68 (×9,6) |

(Sobol n'a pas de miroir : ses deux lignes, avec et sans paires, sont le même
run ; comparé au run simple sans paires, ÷4,8 à ÷241.) Et sur les risques AAD en
vol locale, à nombre de chemins égal et au même temps : variance ÷6 à ÷30 sur
la delta, ÷8 à ÷9 sur la plus grande vega locale, ÷12 à ÷263 sur le prix.

Ce que cela dit :

- **Sobol + pont est de loin l'outil le plus fort** du dépôt, ×4 à ×58 en temps
  pour une erreur donnée, et il ne coûte rien de plus : c'était bien le gain
  manquant sur GPU.
- **L'échantillonnage préférentiel est un outil d'événement rare**, et le pilote
  fait son travail : sur les produits ordinaires il le refuse (le prix est alors
  celui du run simple, à ~35 ms de pilotes près). Sur une digitale à une seule
  date, stratifier W(T) fait mieux encore (÷290) : l'IS vaut surtout quand
  l'événement rare ne se lit pas sur la valeur terminale seule.

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

**Fait (G2).** `scripting/compiler.hpp` traduit l'arbre (après les passes du
front-end) en instructions ; `scripting::run_event` (`bytecode.hpp`) les
interprète sur des pointeurs bruts — la même fonction sur CPU
(`ScriptedProduct`, par défaut) et dans le kernel. Il reproduit les deux
évaluateurs opération par opération : sur les 42 scripts, arbre et bytecode
donnent **les mêmes bits**, en dur, en flou et pour chaque risque AAD
(`tests/testScriptBytecode.cpp`) ; l'arbre reste l'oracle. Les `if` flous
reçoivent des emplacements de sauvegarde fixes, attribués à la compilation et
réutilisés d'une date à l'autre (deux dates ne tournent jamais ensemble) : sur
la bibliothèque, 19 emplacements et 3 `if` imbriqués au plus, contre 3 720 sans
réutilisation — ce qui tient dans la mémoire locale d'un thread.

Sur GPU (`gpu/script.hpp`, `engines/mc/script_engine.hpp`) : un thread par
chemin (ou paire antithétique) avance le modèle décrit en données plates
(`models/device_model.hpp` ; Black-Scholes mono et multi-actifs à taux plat,
vol locale, Heston, SLV) et exécute le bytecode de chaque date. Mêmes unités,
tirages Philox et arbre de réduction que le moteur générique CPU en
`mc_rng = Philox`, qui est son oracle : les 42 scripts donnent le même prix à
10⁻⁹ près sous les quatre dynamiques, dur et flou
(`tests/gpu/testGpuScripts.cpp`). Hors du périmètre du kernel — Sobol, sauts,
courbe de taux, AAD, script trop profond — `auto` reste sur le CPU et le dit
dans les diagnostics, `gpu` refuse avec la raison.

| Script (BS / SLV) | CPU, 10⁵ chemins | GPU, 10⁶ chemins | Gain par chemin |
|---|---|---|---|
| Variance swap (249 dates) | 2,70 s / 3,65 s | 100 ms / 263 ms | ×270 / ×139 |
| Double no-touch | 0,77 s / 1,35 s | 44 ms / 125 ms | ×175 / ×108 |
| Phoenix autocall | 39 ms / 721 ms | 3,7 ms / 72 ms | ×105 / ×100 |
| Asiatique | 68 ms / 492 ms | 4,3 ms / 50 ms | ×157 / ×99 |

Le bytecode seul rend aussi le CPU ~2 fois plus rapide que l'arbre. API :
`device` et `rng` sur les requêtes de script ; le sélecteur *Compute* et le
panneau *CPU vs GPU* s'appliquent aux produits scriptés de la page Pricing, et
la page Scripting a son sélecteur.

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

**Fait (G1) pour vol locale, Heston et SLV** : `models/equity/path_steps.hpp`
(recherche de maille, interpolation bilinéaire de σ_loc, levier en escalier en
T, pas d'Euler à troncature complète), templé sur T. Les modèles CPU
(`LocalVolSimModel`, `SLVSimModel`, `BatesSimModel`) appellent désormais ces
foncteurs — un seul code — sans qu'aucun bit ne bouge (prix et risques AAD
comparés avant / après). Le kernel `gpu::terminal_spots` les exécute sur le
device : nourri des mêmes tirages Philox, chaque chemin GPU égale le chemin CPU à
10⁻¹¹ près. Le pas de Black-Scholes (mono et multi-actifs) n'a pas encore son
foncteur : une ligne, ajoutée avec le kernel des scripts (G2).

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

**Fait (G3).** L'oracle est plus strict que prévu : `simulate_aad` gagne
l'option Philox, donc la tape CPU et le GPU tirent **les mêmes chemins**, et
les risques doivent être égaux à l'arrondi près, pas à l'erreur Monte-Carlo
près. Tout le code par chemin est `QM_HOST_DEVICE`
(`engines/mc/script_path.hpp`, `script_adjoint.hpp`, `scripting/bytecode_adjoint.hpp`) :
la CI, sans GPU, le vérifie contre la tape script par script
(`tests/testScriptAdjoint.cpp`), et le kernel ne fait que l'exécuter.

1. **Duaux** (`utils/dual.hpp`, `Dual<N>`) : l'interpréteur et les pas de
   modèles étant templés sur `T`, `script_path<Dual<N>>` suffit. Directions
   dans l'ordre des étiquettes du modèle CPU : Black-Scholes (4), deux actifs
   (7), Heston (8 ; les trois étiquettes de sauts restent à 0 sans saut). Mêmes
   dérivées locales et même côté aux points anguleux que la tape
   (`max(l, r)` → l si l > r, `fabs` → +1 en 0) ; une dérivée infinie ou NaN
   n'est jamais propagée vers une constante (`pow` d'une base négative dans le
   variance swap), comme la tape n'envoie rien vers une feuille constante.
2. **Adjoint par chemin en vol locale** : l'aller empile sur la *trace* du
   chemin le spot de départ de chaque pas et, pour chaque événement, ce que
   l'adjoint du bytecode demande (opérandes, branches prises) ; le retour
   dépile : `reverse_event` (une adjointe par opcode, `if` flous compris)
   donne ∂payoff/∂spot, puis l'adjoint du pas de vol locale écrit à la main
   remonte d'un pas en ajoutant sa part aux quatre coins de sa maille. Les
   tirages ne sont pas stockés : Philox les régénère. Pas de boucle dans un
   script, donc la taille de la trace est connue à la compilation
   (`trail_bound`) : le nombre de threads par lancement se calcule depuis
   `cudaMemGetInfo` et le coût par thread (trace + ligne de gradient),
   jamais par défaut. Pas d'`atomicAdd` : chaque thread a sa ligne de
   gradient, repliée par warp dans l'ordre des lanes
   ([ADR-G6](#adr-g6--des-lignes-de-gradient-par-thread-pas-datomicadd)) —
   bit à bit identique quel que soit le découpage des lancements et sur
   l'une ou l'autre carte (testé). Erreur standard des risques : lots d'un
   warp (512 chemins), estimateur par quotient.

Égalité à la tape (Philox, 4 096 chemins, 42 scripts, dur et flou) : prix à
10⁻¹⁰, risques à 10⁻⁸ de la plus grande (en pratique 10⁻¹⁵ ; 7·10⁻¹⁰ sur un
Heston à barrière). **Superbucket sur GPU** : `market_vega` passe `device` au
pricing, dV/dσ_loc vient de l'adjoint GPU, puis le même passage par Dupire et
les fits SVI ; les vegas par cotation GPU = celles de la tape à 10⁻⁷, et leur
somme = choc parallèle des cotations (recalibrées, repricées sur GPU à nombres
aléatoires communs) à 3 % près.

Temps (`build-cuda/qm_gpu_risk_bench`, 102 400 chemins, mêmes chemins des deux
côtés, écart relatif maximal entre les deux vecteurs de risques) :

| Script (modèle) | Risques | Tape CPU, 1 thread | 1 V100 | Gain | Écart |
|---|---|---|---|---|---|
| Call (vol locale 30×12, pas quotidien) | 363 | 13,0 s | 69 ms | ×189 | 2·10⁻¹⁵ |
| Up-and-out (vol locale) | 363 | 18,4 s | 140 ms | ×132 | 8·10⁻¹⁶ |
| Phoenix autocall (vol locale) | 363 | 20,5 s | 107 ms | ×191 | 6·10⁻¹⁶ |
| Variance swap (vol locale) | 363 | 23,0 s | 180 ms | ×128 | 3·10⁻¹⁵ |
| Phoenix autocall (Black-Scholes, duaux) | 4 | 140 ms | 5,5 ms | ×25 | 4·10⁻¹⁶ |
| Phoenix autocall (Heston quotidien, duaux) | 11 | 30,6 s | 69 ms | ×444 | 6·10⁻¹⁴ |

Mémoire : le pilote réserve la pile locale d'un kernel pour tous les threads
que la carte peut porter (2 048 × 80 SM) et la garde après le lancement ; les
duaux à 8 directions (13 Ko de pile par thread) retenaient ainsi 1,95 Go d'une
carte que partage le LLM de l'assistant. Chaque calcul de script remet la
limite de pile à sa valeur d'avant (`StackLimitRestore`) : mesuré, les 1,95 Go
reviennent. Une carte à court de mémoire renvoie `auto` sur le CPU, avec la
raison.

Hors du GPU, et dit dans les diagnostics : les risques SLV (grille de levier :
il faudrait l'adjoint du pas SLV), plus de deux actifs (duaux à 8 directions au
plus), Sobol et la stratification sous AAD (les deux moteurs adjoints sont
pseudo-aléatoires).

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

**Fait (G4).** Un calcul partage ses blocs logiques entre les cartes :
`detail::run_on_devices` (`src/gpu/logical_blocks.cuh`) donne à la carte k la
k-ième plage contiguë de blocs, sur son propre thread hôte, avec sa propre
copie du script, du modèle et des tables Sobol ; chaque bloc est réduit par le
même arbre quelle que soit la carte, et l'hôte replie les partiels dans l'ordre
des blocs. Pour l'adjoint, les sommes de gradient par warp remontent bloc par
bloc et l'hôte les replie dans l'ordre global — les lots de l'erreur standard
sont les mêmes. Par défaut, un calcul GPU prend toutes les cartes utilisables
(`PricingSettings::mc_gpus = 0`) : c'est sans effet sur les nombres, seulement
sur le temps. **Vérifié bit à bit** (`tests/gpu/testGpuTwoCards.cpp`) : vanille
(Philox et Sobol, nombre d'unités impair), scripts (Philox, contrôles,
stratification, Sobol, importance sampling ; vol locale, Heston,
Black-Scholes, trois actifs), risques par duaux et par adjoint (Philox et
Sobol) — 1 carte, 2 cartes, et les deux cartes dans l'autre ordre. La réponse
de l'API et l'événement d'audit disent combien de cartes ont servi (`gpus`,
ajout optionnel au bloc `engine`, compatible `BACKWARD` selon le contrat de
`quant-platform`).

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
- **API / front** : un champ `device` (`cpu` / `gpu` / `auto`) — pas
  `engine`, déjà pris par analytique / MC / EDP / arbres — et `rng`
  (`pcg32` / `philox`) ; la réponse dit où le calcul a tourné (`device`) et
  l'audit le consigne (`engine.device`). **Fait pour la vanille européenne**
  (lot « sélection du device », après G0) : sélecteur *Compute* sur la page
  Pricing en Monte-Carlo, et panneau **CPU vs GPU** qui lance la même requête
  sur les deux — Philox et même graine, donc même prix à 10⁻¹⁵ près, seul le
  temps serveur diffère. `GET /price/devices` décrit le serveur ; le contexte
  CUDA est créé au démarrage de l'API (0,3 s, +140 Mo de RAM hôte) pour que le
  premier temps affiché soit celui du pricing. Les produits suivent lot par
  lot (`gpu: true` dans le catalogue du front) : scripts après G2. Depuis G3,
  les risques AAD des scripts (`greeks_method: "aad"`) et la vega par cotation
  suivent le même champ `device` ; la page Scripting propose l'échantillonneur
  stratifié, les contrôles et les paires antithétiques.
- **Image de production** : `web/Dockerfile.prod` compile le wheel avec
  `QM_CUDA=ON` (défaut), avec la même chaîne que le serveur (Debian trixie
  non-free `nvidia-cuda-toolkit` 12.4 + g++-13) ; le runtime CUDA est lié
  statiquement, le pilote est monté par le runtime Docker `nvidia`, que
  `docker-compose.prod.yml` sollicite (`reservations.devices`). Sans GPU au
  run, la même image price sur CPU.

## 9. Benchmark

Le livrable qui compte. **Fait (G4)**, publié dans le README
(`build-cuda/qm_gpu_table_bench`) : temps pour atteindre une erreur standard
relative de 10⁻⁴ sur le prix (10⁻³ sur la delta pour le superbucket, dont
l'unité de travail est un chemin *et* 363 dérivées). Les colonnes CPU font
tourner le code même des kernels (`script_path`, `script_lv_adjoint_path`, le
kernel vanille) sur l'hôte, sur les mêmes blocs logiques : on compare le
matériel, pas deux implémentations. Pseudo-aléatoire : un pilote fixe le nombre
de chemins ; Sobol : on double jusqu'à atteindre la cible (erreur atteinte
donnée). `*` : au-delà de 30 s, chronométré sur une fraction du travail (au
moins un quart du budget, coûts fixes négligeables) et extrapolé linéairement.

Pseudo-aléatoire (Philox ; paires antithétiques sauf le superbucket) :

| | Chemins | CPU 1 thread | CPU 8 threads | 1 V100 | 2 V100 |
|---|---|---|---|---|---|
| Vanille BS | 1,25·10⁸ | 8,9 s | 1,3 s | 28 ms | 17 ms |
| Up-and-out quotidien, vol locale | 1,55·10⁸ | 169 min * | 27 min * | 25,2 s | 12,7 s |
| Worst-of autocall, 3 actifs | 3,8·10⁶ | 10,3 s | 1,5 s | 41 ms | 34 ms |
| Superbucket (363 risques) | 8,5·10⁵ | 51,5 s * | 7,5 s | 237 ms | 237 ms |

Sobol RQMC + pont brownien (16 répliques) :

| | Chemins | Erreur atteinte | CPU 1 thread | CPU 8 threads | 1 V100 | 2 V100 |
|---|---|---|---|---|---|---|
| Vanille BS | 1,3·10⁵ | 7,5·10⁻⁵ | 17 ms | 3 ms | 5 ms | 6 ms |
| Up-and-out quotidien, vol locale | 3,4·10⁷ | 8,6·10⁻⁵ | 35 min * | 291 s * | 4,5 s | 2,3 s |
| Worst-of autocall, 3 actifs | 1,0·10⁶ | 6,6·10⁻⁵ | 2,8 s | 478 ms | 10 ms | 9 ms |
| Superbucket (363 risques) | 1,6·10⁴ | 7,6·10⁻⁴ | 695 ms | 119 ms | 13 ms | 14 ms |

Lecture :

- **Le cas qui justifie les cartes** — une barrière quotidienne sous vol locale,
  252 pas × 1,5·10⁸ chemins : près de trois heures sur un cœur, 27 minutes sur
  huit, **12,7 s sur deux V100** (×800 contre un cœur, ×2 d'une carte à deux).
  Avec Sobol + pont, il faut 4,6 fois moins de chemins : **2,3 s**, soit ×4 400
  contre la référence d'un cœur en pseudo-aléatoire.
- **Deux cartes font ×2 quand le travail est gros**, rien quand il est petit :
  une carte ne rejoint un calcul qu'avec au moins 128 blocs logiques à faire
  (`gpu::kMinBlocksPerDevice` ; sinon son démarrage coûte plus qu'il ne
  rapporte : mesuré 6 ms sur une carte, 13 ms sur deux pour une réplique de
  deux blocs). Le superbucket pseudo-aléatoire (208 blocs) reste ainsi sur une
  carte. Le nombre de cartes réellement utilisées est dans la réponse
  (`gpus`), l'audit et les diagnostics.
- **Sobol + pont** divise le nombre de chemins par 4,6 (barrière quotidienne) à
  ~1 000 (vanille) : sur les petits problèmes, ce sont alors les coûts fixes
  (copies, lancement) qui dominent, d'où des colonnes GPU de quelques ms.
- **Ce qui limite chaque kernel** (bande passante ou calcul) reste à mesurer :
  `ncu` n'a pas accès aux compteurs matériels sans droit administrateur
  (`ERR_NVGPUCTRPERM` ; il faut `NVreg_RestrictProfilingToAdminUsers=0` au
  module `nvidia`, ou lancer `ncu` en root). Ce que l'on sait par
  construction : les kernels de prix ne lisent que des tables de quelques Ko
  (en cache) et sont limités par le calcul FP64 (Φ⁻¹, `exp`), comme mesuré au
  lot G0 ; l'adjoint et Sobol + pont écrivent et relisent leur mémoire de
  travail par thread en mémoire globale, coalescée.

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

Ligne « Superbucket » (lot G3, [§6](#6-aad-sur-gpu)) : contre la **tape** CPU
(et non le code du kernel sur l'hôte comme dans le tableau ci-dessus), dV/dσ_loc
sur une grille Dupire 30×12 en pas quotidien, 102 400 chemins — tape un thread
13 à 23 s, une V100 70 à 180 ms selon le script (×130 à ×190), mêmes risques au
10⁻¹⁵ près.

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
  parallèle (le test du lot 17h, sur GPU). **Fait (G3)**, plus strict : mêmes
  chemins Philox, donc égalité à l'arrondi ; le code par chemin tourne aussi
  sur l'hôte, ce qui met adjoint du bytecode, adjoint vol locale et duaux dans
  la CI (`tests/testScriptAdjoint.cpp`).
- La CI GitHub n'a pas de GPU : elle compile sans CUDA et teste tout ce qui
  est CPU (Philox, pont, bytecode) ; les tests GPU tournent sur le serveur
  (`ctest -L gpu`).

## 11. Séquencement

| Lot | Contenu | Acceptation |
|---|---|---|
| **G0** | CMake CUDA ; Philox hôte/device ; réduction Welford ; le kernel vanille existant sur GPU | Prix GPU = prix CPU ; premier point du benchmark — **fait** : tirages identiques bit à bit, prix à quelques ulp ([§7](#7-reproductibilité-bit-à-bit)), bit à bit entre les deux V100 ; 280× un thread CPU ([§9](#9-benchmark)) |
| **G1** | Pont brownien dans le moteur générique (CPU) ; directions Joe-Kuo jusqu'à 21 201 ; Sobol device ; modèles en foncteurs | Sobol + pont bat le pseudo-aléatoire en log-log sur un asiatique ; mêmes lois CPU / GPU — **fait** : pente −0,89 contre −0,50, erreur ÷19 à 2¹⁷ chemins ; Sobol GPU = CPU bit à bit sur les 21 201 dimensions ; vol locale, Heston et SLV suivent sur GPU les chemins du CPU ([§2.4](#24-le-pont-brownien-dans-le-moteur-générique), [§4](#4-les-modèles--un-ensemble-fermé)) |
| **G2** | Compilateur et interpréteur de bytecode, CPU puis GPU | Les 42 scripts : même prix arbre / bytecode / GPU — **fait** : arbre = bytecode au bit près (dur, flou, AAD) ; GPU = CPU Philox sous Black-Scholes, vol locale, Heston et SLV ; ×2 sur CPU, ×100 à ×270 par chemin sur GPU ([§3](#3-les-scripts--un-bytecode)) |
| **G3** | Variables de contrôle et stratification génériques ; AAD : duaux, puis adjoint par chemin en vol locale | Réduction de variance mesurée ; risques GPU = AAD CPU ; superbucket sur GPU — **fait** : stratification ×1,1 à ×3,4 en temps, contrôles ×3,1 sur le variance swap sans paires (et redondants avec elles, mesuré) ; risques GPU = tape CPU à l'arrondi sur les mêmes chemins, ×130 à ×440 ; superbucket GPU = tape, somme = choc parallèle à 3 % ([§2.5](#25-la-réduction-de-variance-technique-par-technique), [§6](#6-aad-sur-gpu)) |
| **G4** | Deux GPU, reproductibilité bit à bit, benchmark complet | 1 GPU = 2 GPU bit à bit ; tableau publié — **fait** : bit à bit sur la vanille, les scripts (tous échantillonneurs) et les risques ; tableau au [§9](#9-benchmark) et dans le README ; profilage des kernels en attente des droits sur les compteurs |

## 12. Hors périmètre

- Brouillage d'Owen (meilleur que le décalage digital sur intégrandes lisses) :
  après G4, si le benchmark le justifie. **Évalué, pas activé.** Brouillage
  imbriqué par hachage (Burley, « Practical Hash-based Owen Scrambling »,
  JCGT 9(4), 2020 — sans état, idéal pour le GPU ; `owen_scramble`,
  `utils/sobol.hpp`), testé (permutation imbriquée, propriété de (0, m, 1)-réseau,
  CPU = GPU au bit près) et disponible comme réglage interne
  `PricingSettings::mc_sobol_owen`, désactivé et non exposé. Mesure
  (`build-cuda/qm_gpu_owen_bench`) : les 42 scripts, dur et flou, Sobol + pont,
  16 répliques, 6,6·10⁴ et 5,2·10⁵ points, dispersion vraie sur **200 graines**
  (intervalle à 95 % du rapport de variance : ×/÷1,32). En dur, 84 comparaisons :

  | | Owen meilleur | Indistinguable | Owen pire |
  |---|---|---|---|
  | Scripts sans test de seuil sur un spot (44) | 15 | 21 | 8 |
  | Scripts avec test de seuil (40) | 5 | 33 | 2 |

  Meilleurs gains ÷2,17 (outperformance), ÷1,99 (basket), ÷1,66 (call) ; pires
  pertes ×2,3 (reverse cliquet), ×2,9 (capital-protected note à 5,2·10⁵
  points). Surtout, **le verdict change de sens avec le nombre de points pour un
  même produit** (capital-protected note : ÷2,05 puis ×2,9 ; forward-start :
  ÷1,97 puis ×1,45 ; double no-touch : ÷1,38 puis ×1,33). Il n'y a donc pas de
  règle a priori — le test de seuil (`ScriptAnalysis::spot_threshold_test`) ne
  sépare pas gains et pertes — ni de pilote utile (un petit nombre de points ne
  prédit pas le grand). Décision : décalage digital partout. Piste si l'on y
  revient : un vrai brouillage d'Owen (des bits aléatoires par nœud de l'arbre,
  pas un hachage), dont le hachage de Burley n'est qu'une approximation.
- ~~Échantillonnage préférentiel générique (drift optimal par payoff).~~ Fait
  après G3 ([§2.6](#26-sobol-sur-gpu-et-léchantillonnage-préférentiel-issues-102-103)),
  sous la forme d'un drift constant par facteur ; un drift dépendant du temps
  (le vrai mode GHS chemin par chemin) reste à faire si un produit le réclame.
- AAD d'ordre 2 sur GPU.
- Rough Bergomi et calibration neuronale : chantiers 1c et 5, qui
  s'appuieront sur ce moteur.
- `float` : tout reste en `double` (FP64 à 1:2 sur V100 — c'est l'argument
  même de ces cartes).

## 13. Décisions

### ADR-G1 — Un bytecode, pas l'arbre, sur le device

**Décision.** Le script est compilé en bytecode de machine à pile, interprété
par un kernel — et, depuis G2, aussi sur le CPU par défaut (même code).
La vérification que le livre décrit ce principe n'a pas pu être faite à la
rédaction du lot (le livre n'est pas dans le dépôt) : à confirmer par le
mainteneur ; l'évaluateur visiteur reste l'oracle, bit pour bit. **Pourquoi.** Un arbre de nœuds alloués, parcouru par visiteur,
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

### ADR-G6 — Des lignes de gradient par thread, pas d'`atomicAdd`

**Décision.** Dans l'adjoint par chemin, chaque thread accumule sa part de
dV/dσ_loc dans sa propre ligne en mémoire globale ; un second kernel replie les
32 lignes d'un warp dans l'ordre des lanes, l'hôte replie les warps dans
l'ordre des indices. **Pourquoi.** Le plan (§6) prévoyait `atomicAdd` : l'ordre
des additions y dépend de l'ordonnancement, donc le résultat change au dernier
bit d'un run à l'autre — contraire à ADR-G5 et au critère de G4. Les lignes
coûtent de la mémoire (8 octets par paramètre et par thread d'un lancement),
que le dimensionnement des lancements compte ; elles donnent aussi, par warp,
les lots dont on tire l'erreur standard des risques. **Mesuré** : même vecteur
de risques au bit près pour 1 ou n lancements, sur l'une ou l'autre V100.

### ADR-G7 — Sous stratification, la pente intra-strate

**Décision.** Quand stratification et contrôles se combinent, β vient des
co-moments des **différences successives** entre strates voisines (chaque
thread de réduction reçoit 16 strates consécutives, `mc::stratum_of`), pas des
co-moments totaux. **Pourquoi.** La stratification retire déjà la variance
entre strates ; ce qui reste, c'est la variance intra-strate, et la pente qui la
minimise est la pente intra-strate. Avec un chemin par strate on ne peut pas la
mesurer strate par strate ; des strates voisines se ressemblent, et
E[(v_{i+1} − v_i)(v_{i+1} − v_i)ᵀ] ≈ 2Σ_intra — l'estimateur des strates
regroupées de la théorie des sondages (Cochran, *Sampling Techniques*).
**Mesuré** : la pente ordinaire dégradait l'autocall (÷3,4 stratifié seul,
÷2,3 combiné) ; avec la pente intra-strate, le combiné égale ou dépasse la
stratification seule à l'incertitude de la mesure près (autocall ÷2,71 contre
÷2,79 ; variance swap ÷1,92 contre ÷1,53). **Alternative rejetée** : la régression
entre répliques (16 points pour jusqu'à 8 contrôles : sur-ajustement).

## 14. Bibliographie

| Sujet | Référence |
|---|---|
| Monte-Carlo, QMC, réduction de variance | Glasserman, *Monte Carlo Methods in Financial Engineering*, Springer, 2004 (ch. 2 générateurs, 3.1 pont brownien, 4 réduction de variance, 5 QMC) |
| Directions de Sobol | Joe & Kuo, « Constructing Sobol Sequences with Better Two-Dimensional Projections », *SIAM J. Sci. Comput.* 30(5), 2008 |
| Générateur à compteur | Salmon, Moraes, Dror & Shaw, « Parallel Random Numbers: As Easy as 1, 2, 3 », *SC'11*, 2011 |
| Pont brownien | Jäckel, *Monte Carlo Methods in Finance*, Wiley, 2002 |
| Brouillage | Owen, « Randomly Permuted (t,m,s)-Nets and (t,s)-Sequences », *Monte Carlo and Quasi-Monte Carlo Methods in Scientific Computing*, Springer, 1995 |
| Strates regroupées, différences successives | Cochran, *Sampling Techniques*, 3ᵉ éd., Wiley, 1977 |
| Nombres duaux (mode direct) | Griewank & Walther, *Evaluating Derivatives*, 2ᵉ éd., SIAM, 2008 |
| Scripting, AAD | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley, 2021 ; Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley, 2018 |
