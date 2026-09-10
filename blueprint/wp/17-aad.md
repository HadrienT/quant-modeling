# WP 17 — AAD : différentiation adjointe algorithmique (Savine)

| | |
|---|---|
| **Dépend de** | l'architecture timeline / scénario de `feature/dated-mc` — voir [§0](#0-lacquis--et-ce-qui-nest-templé-quen-apparence) |
| **Bloque** | la surface « AAD vs bump » du [lot 15 §2](15-future-quant-surfaces.md#2-aad-vs-bump-and-reprice--chantier-2) ; les sensibilités des scripts ([lot 16](16-scripting.md)) ; les sensibilités xVA ([`etc/roadmap.md`](../../etc/roadmap.md) §4d) |
| **Branche** | `core/aad` |
| **Référence** | Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley 2018 |

> **Deuxième lot du blueprint consacré au cœur C++**, après le
> [lot 16](16-scripting.md). Même principe : l'objectif déclaré est une
> implémentation **fidèle au livre**. Les écarts sont tous listés en
> [§17](#17-décisions) avec leur raison ; il n'y en a aucun qui ne soit imposé
> par l'architecture existante ou par une règle écrite du projet.

## Objectif

Obtenir **toutes les sensibilités d'un prix Monte-Carlo pour un coût de
l'ordre de trois à cinq fois celui du prix, quel que soit leur nombre** — par
un type numérique `Number` qui enregistre les opérations sur une *tape*, puis
une propagation arrière des adjoints.

C'est le chantier 2 de la roadmap, présenté comme « le plus fort signal
technique » du projet, avec cette mise en garde qu'il faut garder en tête :
**ne pas annoncer l'AAD avant de l'avoir écrit**. Aujourd'hui,
[`utils/greeks.hpp`](../../include/quantModeling/utils/greeks.hpp) fait des
différences finies centrées. Il n'y a pas une ligne d'AAD dans le repo.

## Ce que ça change

| | Bump-and-reprice (aujourd'hui) | AAD (ce lot) |
|---|---|---|
| Coût pour *n* sensibilités | ≈ 2*n* + 1 pricings | ≈ 3 à 5 pricings, **indépendant de *n*** |
| Panier à 50 sous-jacents (50 deltas + 50 vegas + rho) | ≈ 200 pricings | ≈ 4 pricings |
| Local vol sur une grille 50 × 30 (1 500 vegas locales) | ≈ 3 000 pricings, en pratique infaisable | ≈ 5 pricings |
| Précision | bruit MC amplifié par 1/*h*, biais en *h*² | exacte **sur chaque chemin**, à la précision machine |
| Payoff discontinu (digitale, barrière) | fonctionne, bruité | dérivée pathwise nulle : exige le lissage ([§5.3](#53-comparaisons-et-flot-de-contrôle)) |

La dernière ligne n'est pas un détail : c'est précisément la raison d'être de la
logique floue du [lot 16](16-scripting.md#52-fuzzyevaluatort--la-logique-floue).
Les deux lots se répondent.

---

## 0. L'acquis — et ce qui n'est templé qu'en apparence

L'architecture de simulation de `feature/dated-mc` a été écrite **dans la
perspective de ce lot** : tout y est templé sur `T`.

| Brique | Fichier | État pour l'AAD |
|---|---|---|
| `Sample<T>`, `Scenario<T>` | [`core/sample.hpp`](../../include/quantModeling/core/sample.hpp) | ✅ prêt |
| `ISimulatableProduct<T>` | [`instruments/simulatable.hpp`](../../include/quantModeling/instruments/simulatable.hpp) | ✅ prêt — il manque `clone()` pour le parallèle |
| `ISimulationModel<T>` | [`models/simulation_model.hpp`](../../include/quantModeling/models/simulation_model.hpp) | ⚠️ il manque l'accès aux **paramètres** |
| `simulate()` | [`engines/mc/simulation_engine.hpp`](../../include/quantModeling/engines/mc/simulation_engine.hpp) | reste le moteur `double` ; l'AAD a son propre moteur |
| `WelfordAccumulator::merge` | [`utils/accumulators.hpp`](../../include/quantModeling/utils/accumulators.hpp) | ✅ la réduction parallèle existe déjà |
| `QM_CACHELINE` | [`core/platform.hpp`](../../include/quantModeling/core/platform.hpp) | ✅ sert au bourrage anti-*false sharing* de la tape |

**Mais le modèle et le produit ne sont templés qu'en apparence.** Dans
[`bs_sim_model.hpp`](../../include/quantModeling/models/equity/bs_sim_model.hpp) :

```cpp
Real s0_, r_, q_, sigma_;              // paramètres en double
struct Step { Real drift; Real vol_sqrt_dt; };   // pré-calculs en double
Real S = s0_;                          // état du chemin en double
S *= std::exp(st.drift + st.vol_sqrt_dt * z);    // std:: qualifié
```

Instancié avec `T = Number`, ce modèle compilerait (grâce aux conversions
`T(...)` à la sortie) **et renverrait des sensibilités nulles** : aucune
opération ne dépend d'un `Number`, rien n'est enregistré. C'est le piège
silencieux de l'AAD par surcharge d'opérateurs, et le premier chantier du lot
est de le refermer ([§6](#6-instrumenter-le-code-existant)). Même constat, en
plus léger, dans
[`simulatable_asian.hpp`](../../include/quantModeling/instruments/equity/simulatable_asian.hpp)
(`std::log`, `std::exp`, `std::max`).

---

## 1. Le mode adjoint en une page

Un calcul est une suite d'opérations élémentaires
`y_k = f_k(y_{a}, y_{b})` qui part des entrées `x` et aboutit au résultat `V`.

- **Mode tangent** (différences finies, dual numbers) : on propage
  `dy_k/dx_i` vers l'avant. **Une passe par entrée** → coût O(*n*).
- **Mode adjoint** : on définit l'adjoint `ȳ_k = ∂V/∂y_k` et on le propage
  **vers l'arrière**, par la règle de dérivation en chaîne lue à l'envers :

  ```
  ȳ_V = 1
  pour k de la fin vers le début :
      pour chaque argument a de l'opération k :
          ȳ_a += (∂f_k/∂y_a) · ȳ_k
  ```

  À la fin, `x̄_i = ∂V/∂x_i` pour **toutes** les entrées à la fois. **Une passe
  par résultat** → coût O(1) en nombre d'entrées.

Le prix à payer : la passe arrière a besoin de **toutes les opérations
effectuées, dans l'ordre, avec leurs dérivées locales**. D'où la *tape* —
l'enregistrement du calcul — et tout le travail d'ingénierie du livre porte sur
la rendre rapide et petite.

---

## 2. `blocklist` — la mémoire par blocs

`include/quantModeling/aad/blocklist.hpp`

Le conteneur sous-jacent de la tape. **Pas un `std::vector`**, pour deux raisons
qui sont les deux exigences de la tape :

1. **Adresses stables.** Les nœuds pointent les uns vers les autres par
   pointeurs bruts. Un `std::vector` qui réalloue invaliderait tout.
2. **Aucune allocation dans la boucle chaude.** Rembobiner ne libère rien : les
   blocs restent en place et sont réutilisés au chemin suivant.

```cpp
template <class T, std::size_t block_size>
class blocklist
{
    std::list<std::array<T, block_size>> data_;
    iterator cur_block_, last_block_, marked_block_;
    block_iterator next_space_, last_space_, marked_space_;

    void new_block();    // alloue un bloc en fin de liste
    void next_block();   // passe au bloc suivant, en alloue un si besoin
public:
    T* emplace_back(Args&&...);            // placement new dans le bloc courant
    template <std::size_t n> T* emplace_back_multi();  // n cases contiguës
    T* emplace_back_multi(std::size_t n);
    void clear();          // libère tout
    void rewind();         // revient au début, NE LIBÈRE RIEN
    void set_mark();       // retient la position courante
    void rewind_to_mark(); // revient à la marque, NE LIBÈRE RIEN
    void memset(unsigned char v = 0);
    // itérateurs bidirectionnels : la propagation parcourt la tape à l'envers
};
```

`emplace_back_multi<n>` réserve `n` cases **contiguës** — les dérivées locales
et les pointeurs d'adjoints d'un nœud à `n` arguments. S'il reste moins de `n`
cases dans le bloc courant, on saute au bloc suivant (la fin du bloc est
perdue ; `n ≤ block_size` est un invariant).

**Propriété vérifiable** (et testée, [§14](#14-tests)) : après le premier
chemin, la mémoire de la tape n'augmente plus.

---

## 3. `Node` — un nœud de tape

`include/quantModeling/aad/node.hpp`

```cpp
class Node
{
    friend class Tape;
    friend class Number;

    const std::size_t n_;          // nombre d'arguments
    double    adjoint_ = 0.0;      // adjoint mono-dimensionnel
    double*   derivatives_;        // n dérivées locales ∂f/∂arg_i
    double**  arg_adjoints_;       // n pointeurs vers les adjoints des arguments
    double*   adjoints_multi_;     // multi-adjoints (§9)
public:
    static std::size_t num_adj;    // dimension multi (§9)
    explicit Node(std::size_t n = 0) : n_(n) {}

    double& adjoint() { return adjoint_; }

    void propagate_one()
    {
        if (!n_ || !adjoint_) return;          // feuille, ou adjoint nul : rien à faire
        for (std::size_t i = 0; i < n_; ++i)
            *arg_adjoints_[i] += derivatives_[i] * adjoint_;
    }
    void propagate_all();                      // version multi (§9)
};
```

Deux choix du livre à conserver tels quels :

- **Le nœud stocke des pointeurs vers les *adjoints* de ses arguments**, pas vers
  les nœuds eux-mêmes. La propagation est alors une boucle de
  multiplications-additions sans indirection supplémentaire.
- **Le test `!adjoint_`** court-circuite les pans entiers de calcul dont le
  résultat ne dépend pas (branches mortes, chemins désactivés). C'est un gain
  réel, pas une micro-optimisation.

Taille : 40 octets par nœud + 16 octets par argument (une dérivée, un pointeur).

---

## 4. `Tape`

`include/quantModeling/aad/tape.hpp` · `src/aad/tape.cpp`

```cpp
constexpr std::size_t BLOCKSIZE = 16384;   // nœuds par bloc
constexpr std::size_t ADJSIZE   = 32768;   // multi-adjoints par bloc
constexpr std::size_t DATASIZE  = 65536;   // dérivées et pointeurs par bloc

class Tape
{
    static bool multi;                                   // §9
    blocklist<double,  ADJSIZE>  adjoints_multi_;
    blocklist<double,  DATASIZE> derivatives_;
    blocklist<double*, DATASIZE> arg_ptrs_;
    blocklist<Node,    BLOCKSIZE> nodes_;
    alignas(QM_CACHELINE) char pad_[QM_CACHELINE];       // anti false sharing

public:
    template <std::size_t N> Node* record_node();
    void reset_adjoints();
    void reset_adjoints_before_mark();     // ajout au livre — voir §7.4
    void clear();                          // libère toute la mémoire
    void rewind();                         // retour au début, mémoire conservée
    void mark();                           // pose la marque de check-pointing
    void rewind_to_mark();                 // retour à la marque, mémoire conservée

    using iterator = /* itérateur de nodes_ */;
    iterator begin(); iterator end(); iterator mark_it();
    iterator find(Node* node);
};
```

`record_node<N>()` est le seul point d'écriture :

```cpp
template <std::size_t N> Node* Tape::record_node()
{
    Node* node = nodes_.emplace_back(N);
    if constexpr (N > 0) {
        node->derivatives_  = derivatives_.emplace_back_multi<N>();
        node->arg_adjoints_ = arg_ptrs_.emplace_back_multi<N>();
    }
    if (multi) {
        node->adjoints_multi_ = adjoints_multi_.emplace_back_multi(Node::num_adj);
        std::fill_n(node->adjoints_multi_, Node::num_adj, 0.0);
    }
    return node;
}
```

`N` est un paramètre **template** : l'arité de chaque opération est connue à la
compilation, les branches `if constexpr` disparaissent.

Le bourrage `pad_` évite que deux tapes de deux threads partagent une ligne de
cache ([§8](#8-parallélisme)) — le `QM_CACHELINE` de
[`core/platform.hpp`](../../include/quantModeling/core/platform.hpp) a été
posé exactement pour ça.

---

## 5. `Number`

`include/quantModeling/aad/number.hpp` · `src/aad/number.cpp`

### 5.1 Représentation

```cpp
class Number
{
    double value_;
    Node*  node_;

    template <std::size_t N> void create_node() { node_ = tape->record_node<N>(); }
    // accès aux dérivées locales du nœud courant
    double& derivative();                  // unaire
    double& left_der();  double& right_der();   // binaire

    Number(Node& arg, double val);                   // nœud unaire
    Number(Node& lhs, Node& rhs, double val);        // nœud binaire
public:
    static thread_local Tape* tape;        // la tape du thread courant — voir ADR-A2

    Number() = default;                    // non initialisé, pas de nœud
    explicit Number(double val);           // feuille : enregistrée sur la tape
    Number& operator=(double val);         // idem
    void put_on_tape();                    // (ré)enregistre comme feuille

    double  value() const;  double& value();
    double  adjoint() const; double& adjoint();
    double& adjoint(std::size_t n);        // multi

    explicit operator double() const;      // explicite : aucune conversion silencieuse

    static void propagate_adjoints(Tape::iterator from, Tape::iterator to);
    void        propagate_to_start();
    void        propagate_to_mark();
    static void propagate_mark_to_start();
};
```

**16 octets**, deux champs. La conversion vers `double` est **explicite** : une
conversion implicite couperait la dépendance sans bruit — le bug de [§0](#0-lacquis--et-ce-qui-nest-templé-quen-apparence),
en pire.

### 5.2 La table des dérivées locales

Chaque surcharge calcule la valeur, enregistre un nœud et y écrit ses dérivées
locales. C'est **toute** la sémantique de l'AAD :

| Opération | Valeur *v* | ∂*v*/∂*x* | ∂*v*/∂*y* |
|---|---|---|---|
| `x + y` | *x* + *y* | 1 | 1 |
| `x - y` | *x* − *y* | 1 | −1 |
| `x * y` | *xy* | *y* | *x* |
| `x / y` | *x*/*y* | 1/*y* | −*v*/*y* |
| `pow(x, y)` | *x*ʸ | *y*·*v*/*x* | *v*·ln *x* |
| `max(x, y)` | | 1 si *x* > *y*, sinon 0 | complément |
| `min(x, y)` | | 1 si *x* < *y*, sinon 0 | complément |
| `-x` | −*x* | −1 | |
| `exp(x)` | eˣ | *v* | |
| `log(x)` | ln *x* | 1/*x* | |
| `sqrt(x)` | √*x* | 0,5/*v* | |
| `fabs(x)` | \|*x*\| | signe de *x* | |
| `normal_dens(x)` | φ(*x*) | −*x*·*v* | |
| `normal_cdf(x)` | Φ(*x*) | φ(*x*) | |

**Les opérations mixtes `Number ⋄ double` créent un nœud *unaire*** (`x * 2.0`
→ un nœud, dérivée 2), jamais un nœud binaire avec une feuille pour la
constante. Écrire `x * Number(2.0)` pollue la tape d'une feuille inutile à
chaque chemin. Toutes les opérations de la table existent en version mixte, y
compris `max(x, 0.0)` — le cas le plus fréquent dans un payoff.

Les dérivées de `x / y` et `sqrt` réutilisent la valeur *v* déjà calculée :
c'est gratuit et le livre le fait systématiquement.

### 5.3 Comparaisons et flot de contrôle

`<`, `>`, `==`… comparent les **valeurs** et ne créent **aucun** nœud. Le flot
de contrôle n'est donc pas différentié : un `if (S > K)` choisit une branche et
la dérivée de l'indicatrice est **nulle presque partout**. Conséquence directe :
le delta pathwise d'une digitale ou d'une barrière est nul.

Le remède est le lissage — la logique floue du
[lot 16](16-scripting.md#42-domainprocessor--le-cœur-du-flou) pour les scripts,
et le lissage des digitales et les probabilités de survie de barrière déjà
présents dans le repo ([`tests/testSmoothing.cpp`](../../tests/testSmoothing.cpp))
pour les engines écrits à la main. Un test de [§14](#14-tests) documente la
limite plutôt que de la cacher.

### 5.4 La propagation

```cpp
void Number::propagate_adjoints(Tape::iterator from, Tape::iterator to)
{
    auto it = from;
    while (it != to) { it->propagate_one(); --it; }
    it->propagate_one();
}
void Number::propagate_to_start()      { adjoint() = 1.0; propagate_adjoints(tape->find(node_), tape->begin()); }
void Number::propagate_to_mark()       { adjoint() = 1.0; propagate_adjoints(tape->find(node_), tape->mark_it()); }
void Number::propagate_mark_to_start() { propagate_adjoints(std::prev(tape->mark_it()), tape->begin()); }
```

Les trois points d'entrée sont la mécanique du check-pointing ([§7](#7-monte-carlo-adjoint-et-check-pointing--simulate_aad)).

### 5.5 Le piège ADL

Dans du code templé sur `T`, **jamais `std::exp(x)`** : avec `T = Number`, il
choisit la surcharge `double` via la conversion — qui est explicite, donc ça ne
compile pas, ce qui est le meilleur cas. Le pire cas est une expression
convertie à la main (`std::exp(double(x))`) qui compile et coupe la dépendance.

La règle, dans tout fichier templé :

```cpp
using std::exp; using std::log; using std::sqrt; using std::max;
S *= exp(drift + vol_sqrt_dt * z);   // ADL : trouve aad::exp si T = Number
```

---

## 6. Instrumenter le code existant

### 6.1 La règle

**Tout ce qui dépend d'un paramètre devient `T`. Tout le reste reste `double`.**

| Reste `double` | Devient `T` |
|---|---|
| gaussiennes, uniformes, RNG | paramètres du modèle |
| timeline, dates, `dt`, `√dt` | pré-calculs dépendant des paramètres (drift, `σ√dt`) |
| strike, barrière, notionnel **du produit** | état du chemin (spot, variables) |
| constantes du script (lot 16, [ADR-S3](16-scripting.md#adr-s3--last-nest-pas-templé-sur-le-type-numérique)) | numéraire, discounts, forwards |

La troisième ligne est une contrainte du livre qu'il faut rendre explicite : les
**produits ne portent pas de `Number`**. Ils sont partagés entre threads en
lecture seule ([§8.3](#83-simulate_parallel_aad)). Une sensibilité à un
paramètre de produit (niveau de barrière) passe par un paramètre de modèle, ou
par un clone du produit par thread.

### 6.2 L'interface des paramètres

Ajout à `ISimulationModel<T>` :

```cpp
virtual const std::vector<T*>&           parameters() = 0;
virtual const std::vector<std::string>&  parameter_labels() const = 0;
std::size_t num_params() const;

void put_parameters_on_tape()
{
    if constexpr (std::is_same_v<T, aad::Number>)
        for (T* p : parameters()) p->put_on_tape();
}
```

`parameters()` renvoie des **pointeurs non-possédants vers les membres du
modèle** — conforme à la convention d'ownership du cœur. Le piège du livre,
qu'il faut reproduire à l'identique : **ces pointeurs sont invalidés par la
copie**. Tout modèle a un `set_param_pointers()` appelé par le constructeur
**et** par le constructeur de copie, sans quoi `clone()` rend un modèle dont
les paramètres pointent dans l'original — et le parallèle calcule des
sensibilités fausses sans planter.

### 6.3 Fichier par fichier

| Fichier | Changement |
|---|---|
| [`models/simulation_model.hpp`](../../include/quantModeling/models/simulation_model.hpp) | §6.2 |
| [`instruments/simulatable.hpp`](../../include/quantModeling/instruments/simulatable.hpp) | `virtual std::unique_ptr<ISimulatableProduct<T>> clone() const = 0;` |
| [`models/equity/bs_sim_model.hpp`](../../include/quantModeling/models/equity/bs_sim_model.hpp) | `s0_ r_ q_ sigma_` → `T` ; `Step::drift`, `Step::vol_sqrt_dt` → `T` ; `S` → `T` ; `using std::exp` + appels non qualifiés ; `parameters() = {&s0_, &sigma_, &r_, &q_}`, labels `"spot" "vol" "rate" "div"` ; `set_param_pointers()` |
| [`instruments/equity/simulatable_asian.hpp`](../../include/quantModeling/instruments/equity/simulatable_asian.hpp) | ADL ; `avg - strike_` en mixte au lieu de `T(strike_)` ; accumulateur initialisé sans feuille |
| `models/equity/localvol_sim_model.hpp` *(à écrire, reste du lot timeline)* | la grille σ_loc en `T` : **chaque point de grille est un paramètre** — c'est le cas d'usage phare |
| `models/equity/multiasset_bs_sim_model.hpp` *(à écrire)* | spots et vols en `T`, corrélation en `double` en v1 ; prérequis de la démonstration à 50 sous-jacents |

`simulate()` n'est pas touché : il reste le moteur `double`, et c'est le témoin
des tests de parité.

---

## 7. Monte-Carlo adjoint et check-pointing — `simulate_aad`

`include/quantModeling/engines/mc/simulation_engine_aad.hpp`

### 7.1 L'algorithme

L'algorithme du livre, étape par étape :

```cpp
AADSimulResults simulate_aad(const ISimulatableProduct<Number>& prd,
                             ISimulationModel<Number>& mdl,
                             RNG& rng, std::size_t n_paths,
                             const Aggregator& agg = first_payoff)
{
    Tape& tape = *Number::tape;
    tape.rewind();                                   // 1. tape vide, mémoire gardée

    mdl.put_parameters_on_tape();                    // 2. feuilles : les paramètres
    mdl.init(prd.timeline(), prd.defline());         // 3. pré-calculs, ENREGISTRÉS
    rng.init(mdl.sim_dim());
    Scenario<Number> path; allocate_scenario(path, prd.defline(), mdl.n_underlyings());
    std::vector<Number> payoffs(prd.payoff_labels().size());
    std::vector<double> gauss(mdl.sim_dim());

    tape.mark();                                     // 4. LA MARQUE

    for (std::size_t p = 0; p < n_paths; ++p)
    {
        tape.rewind_to_mark();                       // 5. oublie le chemin précédent
        rng.next_g(gauss);
        mdl.generate_path(gauss, path);              //    enregistré après la marque
        prd.payoffs(path, payoffs);
        Number result = agg(payoffs);
        result.propagate_to_mark();                  // 6. adjoints → jusqu'à la marque
        /* stocker les payoffs en double */
    }

    Number::propagate_mark_to_start();               // 7. marque → paramètres, UNE fois
    for (std::size_t j = 0; j < mdl.num_params(); ++j)
        results.risks[j] = mdl.parameters()[j]->adjoint() / n_paths;
    tape.clear();
    return results;
}
```

### 7.2 Pourquoi ça marche, et pourquoi c'est indispensable

**Pourquoi ça marche.** Les nœuds *après* la marque sont recréés à chaque chemin
(placement `new` → adjoint remis à 0). Les nœuds *avant* la marque — paramètres
et pré-calculs — ne sont jamais recréés : leurs adjoints **s'accumulent** sur
tous les chemins, via les pointeurs que les nœuds de chemin tiennent vers eux.
À l'étape 7, une seule propagation depuis la marque pousse la **somme** de ces
adjoints vers les paramètres. C'est exact **par linéarité de la propagation
arrière** : propager une somme d'adjoints, c'est sommer les propagations.

**Pourquoi c'est indispensable.** Un chemin Dupire à 100 pas, ~20 opérations
par pas : ~2 000 nœuds, de l'ordre de 100 Ko de tape. Sans check-pointing, un
million de chemins, c'est **~100 Go**. Avec, c'est **~100 Ko**, réutilisés. Et
les pré-calculs du modèle — potentiellement coûteux, la moitié du travail d'un
Dupire — ne sont enregistrés **qu'une fois**, pas un million.

### 7.3 L'agrégateur

Un produit peut avoir plusieurs payoffs (`payoff_labels()`). L'AAD mono-adjoint
différentie **un** scalaire : `agg` le fabrique. Par défaut le premier payoff,
comme dans le livre ; la somme pour un portefeuille. Pour les sensibilités de
**chaque** payoff en une passe : [§9](#9-différentiation-multiple).

### 7.4 Risques par lot — l'écart assumé au livre

Le livre propage la marque vers le début **une seule fois**, à la fin. On le
fait **par lot de 64 chemins** :

```
pour chaque lot :
    … 64 chemins, propagate_to_mark() chacun …
    Number::propagate_mark_to_start();
    risques_du_lot[j] = paramètre_j.adjoint()
    tape.reset_adjoints_before_mark();      // repart de zéro
risques = somme des lots / N ;  erreur standard = Welford sur les moyennes de lot
```

Deux gains, qui sont deux règles écrites du projet :

1. **Une erreur standard sur chaque sensibilité.** Le blueprint exige « jamais
   un prix sans son erreur standard » ; les champs `delta_std_error`… de
   [`core/results.hpp`](../../include/quantModeling/core/results.hpp) existent
   déjà. Les moyennes par lot sont i.i.d. : c'est exactement la méthode d'erreur
   des lots RQMC déjà utilisée par
   [`engines/mc/black_scholes.cpp`](../../src/engines/mc/black_scholes.cpp).
2. **La reproductibilité bit-à-bit en parallèle** ([§8.4](#84-reproductibilité)).

Coût : une propagation de la section d'initialisation par lot de 64 chemins.
Négligeable en Black-Scholes (quatre paramètres, quelques nœuds) ; borné en
Dupire. Le détail est en [ADR-A6](#adr-a6--risques-par-lot-plutôt-quune-propagation-unique).

---

## 8. Parallélisme

### 8.1 `ConcurrentQueue` et `ThreadPool`

`include/quantModeling/utils/{concurrent_queue,thread_pool}.hpp`

Aucun thread n'existe aujourd'hui dans le cœur. On reprend ceux du livre :

```cpp
template <class T> class ConcurrentQueue {   // mutex + condition_variable + std::queue
    bool try_pop(T&); void pop(T&); void push(T); void interrupt();
};

using Task       = std::packaged_task<bool(void)>;
using TaskHandle = std::future<bool>;

class ThreadPool
{
    ConcurrentQueue<Task>    queue_;
    std::vector<std::thread> threads_;
    static thread_local std::size_t tls_num_;       // 0 = thread principal
public:
    void start(std::size_t n = std::thread::hardware_concurrency() - 1);
    void stop();
    std::size_t num_threads() const;
    static std::size_t thread_num() { return tls_num_; }
    template <class F> TaskHandle spawn_task(F&& f);
    bool active_wait(const TaskHandle& f);   // le principal exécute des tâches en attendant
};
```

`active_wait` est le point fin : le thread principal ne dort pas en attendant ses
tâches, il **dépile et exécute**. Pas de cœur perdu, pas d'interblocage si des
tâches en créent d'autres. Le pool est un objet possédé, pas un singleton —
[ADR-A7](#adr-a7--threadpool--lapi-du-livre-sans-le-singleton).

### 8.2 Générateurs avec `skip_to`

Pour qu'un résultat ne dépende pas du nombre de threads, **le chemin *p* doit
toujours consommer les mêmes nombres aléatoires**, quel que soit le thread qui
le calcule. Le livre donne à chaque générateur un `skip_to(p)` :

```cpp
class RNG {
    virtual void init(std::size_t sim_dim) = 0;
    virtual void next_u(std::span<double>) = 0;
    virtual void next_g(std::span<double>) = 0;
    virtual std::unique_ptr<RNG> clone() const = 0;
    virtual void skip_to(std::size_t path) = 0;
    virtual std::size_t sim_dim() const = 0;
};
```

Deux implémentations, sur les générateurs **déjà présents** ([ADR-A8](#adr-a8--linterface-rng-du-livre-sur-pcg32-et-sobol)) :

- **PCG32** — [`utils/rng.hpp`](../../include/quantModeling/utils/rng.hpp) n'a pas
  de saut aujourd'hui. Un LCG saute de *k* pas en O(log *k*) par
  exponentiation binaire de l'application affine `s ↦ a·s + c`. Saut de
  `p × sim_dim` tirages.
- **Sobol** — [`utils/sobol.hpp`](../../include/quantModeling/utils/sobol.hpp)
  avance en code de Gray (`index_`, un XOR par dimension). Le point *n* se
  calcule directement : XOR des nombres directeurs aux bits de
  `gray(n) = n ^ (n >> 1)`, en O(dim × 32). Le décalage digital est conservé.

Contrainte qui en découle : **gaussiennes par inverse de la normale uniquement**.
Box-Muller garde un tirage en réserve ([`NormalBoxMuller::spare`](../../include/quantModeling/utils/rng.hpp)) :
son état dépend de l'historique, le saut devient faux. `InverseNormalSource`
existe déjà, et le commentaire de
[`pricers/context.hpp`](../../include/quantModeling/pricers/context.hpp) le
désigne déjà comme « la transformation sans état requise par le QMC et CUDA ».

### 8.3 `simulate_parallel_aad`

L'algorithme du livre :

```
tapes    : une par thread secondaire ; le principal garde la sienne
modèles  : un clone par thread           (paramètres = Number SUR SA tape)
chemins, payoffs, gaussiennes, RNG : un par thread
produit  : partagé, lecture seule        (§6.1)

pour chaque lot de 64 chemins → spawn_task :
    n = ThreadPool::thread_num()
    Number::tape = &tapes[n]                          ← le thread_local
    si le modèle n n'est pas encore initialisé sur cette tape :
        tape.rewind(); modèle.put_parameters_on_tape(); modèle.init(); tape.mark()
    rng[n].skip_to(premier_chemin_du_lot)
    pour chaque chemin du lot :
        rewind_to_mark ; next_g ; generate_path ; payoffs ; propagate_to_mark
    (§7.4) propagate_mark_to_start ; lire les adjoints ; reset_adjoints_before_mark

active_wait sur tous les lots
risques = Σ_lots / N
```

Trois points qui font la différence entre du parallèle qui marche et du
parallèle qui a l'air de marcher :

- **Chaque thread a sa propre copie des paramètres, sur sa propre tape.** Un
  `Number` enregistré sur la tape A et utilisé par le thread B écrit dans la
  mémoire de A : corruption silencieuse. D'où les clones, et d'où
  `set_param_pointers()` ([§6.2](#62-linterface-des-paramètres)).
- **L'initialisation est paresseuse et par thread** : un thread qui ne reçoit
  aucun lot n'initialise rien.
- **Lot de 64** : assez gros pour amortir le coût d'une tâche, assez petit pour
  équilibrer la charge. C'est la valeur du livre.

### 8.4 Reproductibilité

| | Garantie |
|---|---|
| Payoffs | **bit-à-bit** identiques au série et entre 1, 2, *n* threads (`skip_to`) |
| Risques, variante du livre | identiques **à l'ordre de sommation près** : les adjoints d'un thread s'additionnent dans l'ordre d'arrivée des lots |
| Risques, variante §7.4 | **bit-à-bit** : on réduit `risques_du_lot` **dans l'ordre des lots**, indépendamment du thread |

La roadmap fait de la reproductibilité bit-à-bit une exigence de desk (« le
contrôle des risques doit pouvoir rejouer un chiffre ») ; §7.4 la donne au prix
d'un tableau `n_lots × n_params`.

---

## 9. Différentiation multiple

Le mode adjoint coûte une passe **par résultat**. Pour les sensibilités de
*m* payoffs (un portefeuille, ou les `payoff_labels()` d'un produit), *m*
passes — sauf à propager *m* adjoints à la fois.

- `Tape::multi = true`, `Node::num_adj = m` : chaque nœud porte un tableau de
  *m* adjoints (`adjoints_multi_`) au lieu d'un scalaire.
- `propagate_all()` : même boucle que `propagate_one`, avec une boucle
  intérieure sur les *m* adjoints — vectorisable.
- `simulate_aad_multi()` : chaque payoff *i* est ensemencé sur **sa**
  composante (`payoffs[i].adjoint(i) = 1`), une seule passe arrière.

Résultat : une **matrice** `m × n_params` de sensibilités pour un coût en
O(*m*) sur la seule partie propagation — l'enregistrement, lui, n'est fait
qu'une fois. Le livre montre le gain sur un portefeuille ; ici, c'est ce qui
servira au pricing par lot du [lot 09](09-portfolio-risk.md).

---

## 10. Expression templates

`include/quantModeling/aad/expression.hpp`

Dernière étape du livre, et la plus spectaculaire en performance. Sans
expression templates, `y = x1 * x2 + exp(x3)` enregistre **trois** nœuds (`*`,
`exp`, `+`). Avec, il en enregistre **un** — à trois arguments, avec ses trois
dérivées partielles calculées **à la compilation** par récursion de templates.
Tape 3 à 5 fois plus petite, AAD environ deux fois plus rapide.

```cpp
template <class E> struct Expression {
    double value() const { return static_cast<const E&>(*this).value(); }
};

template <class LHS, class OP, class RHS>
class BinaryExpression : public Expression<BinaryExpression<LHS, OP, RHS>>
{
    const double value_; const LHS lhs_; const RHS rhs_;
public:
    enum { num_numbers = LHS::num_numbers + RHS::num_numbers };
    template <std::size_t N, std::size_t n>
    void push_adjoint(Node& node, double adj) const {
        if constexpr (LHS::num_numbers > 0)
            lhs_.template push_adjoint<N, n>(node, adj * OP::left_derivative(lhs_.value(), rhs_.value(), value_));
        if constexpr (RHS::num_numbers > 0)
            rhs_.template push_adjoint<N, n + LHS::num_numbers>(node, adj * OP::right_derivative(lhs_.value(), rhs_.value(), value_));
    }
};

struct OPMult { static double eval(double l, double r) { return l * r; }
                static double left_derivative (double, double r, double) { return r; }
                static double right_derivative(double l, double, double) { return l; } };
// OPAdd, OPSub, OPDiv, OPPow, OPMax, OPMin ; UnaryExpression + OPExp, OPLog, …
// et les versions scalaires (Number ⋄ double) comme en §5.2
```

`Number` devient une feuille d'expression (`num_numbers = 1`), et l'affectation
d'une expression à un `Number` déclenche **l'unique** enregistrement :

```cpp
template <class E> Number(const Expression<E>& e) : value_(e.value())
{
    Node* node = tape->record_node<E::num_numbers>();
    static_cast<const E&>(e).template push_adjoint<E::num_numbers, 0>(*node, 1.0);
}
```

**Le piège, à écrire en gras dans les conventions** : une expression capture ses
opérandes par référence. `auto y = a * b;` garde des références vers des
temporaires détruits en fin d'instruction. **Jamais `auto` pour une expression
en `T`** — toujours `T y = …` ou `Number y = …`. Le preset `asan` du projet
attrape ces cas ; les tests AAD y tournent systématiquement.

L'ordre est celui du livre ([ADR-A5](#adr-a5--dabord-sans-expression-templates)) :
d'abord un `Number` sans expression templates, puis on remplace son
implémentation **derrière la même API**. La version sans ET devient l'oracle :
les deux doivent donner des sensibilités identiques à la précision machine.

---

## 11. À travers la calibration — des risques modèle aux risques marché

L'AAD de §7 donne des **risques modèle** : ∂V/∂(paramètres du modèle). Un desk
veut des **risques marché** : ∂V/∂(cotations). Pour une local vol, ce ne sont
pas les 1 500 points de σ_loc mais les vols implicites cotées. C'est le
« superbucket » Dupire du livre.

**Calibration explicite (Dupire).** La local vol est une fonction explicite de
la surface de vol implicite (formule de Dupire). On templatise la calibration,
on l'enregistre sur la tape **avant la marque**, et `propagate_mark_to_start()`
pousse les adjoints à travers elle jusqu'aux vols implicites. Le livre en fait
une version en deux temps pour la mémoire :

1. AAD de simulation → ∂V/∂σ_loc (risques modèle) ;
2. calibration enregistrée seule, ses sorties σ_loc **ensemencées** avec
   ∂V/∂σ_loc, propagation arrière → ∂V/∂σ_impl.

C'est du check-pointing à travers la calibration.

**Calibration itérative (Heston, Hull-White).** Pas de formule : un optimiseur.
On ne dérive pas à travers ses itérations, on utilise le **théorème des
fonctions implicites** à l'optimum : si `R(θ, m) = 0` caractérise la
calibration, `dθ/dm = −(∂R/∂θ)⁻¹ ∂R/∂m`, et les deux jacobiennes se calculent
elles-mêmes par AAD.

**Deux dépendances bloquantes, à dire franchement :**

- la calibration Dupire est **en Python**
  ([`api/app/local_vol/dupire.py`](../../api/app/local_vol/dupire.py),
  [`iv_surface.py`](../../api/app/local_vol/iv_surface.py)) ; le C++ ne reçoit
  qu'une grille pré-calculée. Le superbucket exige de la porter en C++
  templé — ce que la roadmap demande déjà en §1d ;
- il n'y a **aucun framework de calibration** en C++ (roadmap, chantier 0).

Cette partie est donc la dernière du lot, et elle ne démarre qu'après ces deux
prérequis.

---

## 12. Ordre 2

L'AAD du livre est d'ordre 1. Le gamma s'obtient par **différences finies sur
l'AAD** : bumper le spot de ±*h*, relancer deux AAD, `(Δ⁺ − Δ⁻) / 2h`. Chaque
paire de relances donne **toute une ligne** de la hessienne (gamma, mais aussi
∂²V/∂S∂σ, ∂²V/∂S∂r…) — contre O(*n*²) pricings en bump pur.

Mise en garde : le delta d'un payoff à coin est discontinu, la différence finie
dessus est bruitée. Le lissage de [§5.3](#53-comparaisons-et-flot-de-contrôle)
aide ici aussi. L'AAD d'ordre 2 (tangent sur adjoint) est hors périmètre.

---

## 13. Résultats et intégration

### 13.1 Le rapport de risques

Le struct `Greeks` actuel a cinq champs fixes : il ne peut pas porter 1 500
vegas locales. Ajout à [`core/results.hpp`](../../include/quantModeling/core/results.hpp) :

```cpp
struct RiskReport
{
    std::vector<std::string> labels;       // parameter_labels() : "spot", "vol", "lvol[12,3]"…
    std::vector<double>      values;
    std::vector<double>      std_errors;   // §7.4
};
struct PricingResult { /* … */ std::optional<RiskReport> risks; };
```

Pour les modèles qui s'y prêtent, les `Greeks` existants sont **remplis depuis
le rapport** (`delta ← "spot"`, `vega ← "vol"`, `rho ← "rate"`), ce qui laisse
le front actuel fonctionner sans changement. Le theta reste en bump : le temps
n'est pas un paramètre du modèle.

`PricingSettings` gagne `GreeksMethod greeks = GreeksMethod::Bump`
(`None`, `Bump`, `AAD`), **ajouté en dernier** pour préserver l'initialisation
d'agrégat — la règle déjà écrite dans
[`pricers/context.hpp`](../../include/quantModeling/pricers/context.hpp).

### 13.2 Bindings, API, front

- `pricing_result_to_dict` ([`src/module.cpp`](../../src/module.cpp)) sérialise
  `risks` quand il est présent.
- API : `PricingResponse.risks: Optional[List[RiskEntry]]` avec
  `RiskEntry { label, value, std_error }`, et `greeks_method: Literal["bump",
  "aad"]` dans les requêtes des produits qui le supportent.
- Front : **la surface est déjà réservée** — [lot 15 §2](15-future-quant-surfaces.md#2-aad-vs-bump-and-reprice--chantier-2) :
  courbe coût en fonction du nombre de paramètres (bump linéaire, adjoint plat),
  tableau comparatif sur le panier à cinquante sous-jacents, écart AAD / bump
  avec barres d'erreur.

---

## 14. Tests

Des tests de propriété, comme l'exige [`CLAUDE.md`](../../CLAUDE.md). Tous
tournent **aussi sous le preset `asan`** : l'AAD manipule des pointeurs bruts
entre blocs, c'est là que les erreurs se cachent.

| Test | Fichier | Propriété |
|---|---|---|
| Opérateurs | `testAADNumber.cpp` | pour **chaque** ligne de la table §5.2 : dérivée adjointe = différence finie centrée à 1e-7 près, sur une grille de points |
| Fonction jouet | `testAADNumber.cpp` | une fonction à cinq entrées avec réutilisation de variable (à la manière du livre) : gradient adjoint = gradient analytique à 1e-12 |
| Mixtes | `testAADNumber.cpp` | `x * 2.0` enregistre **un** nœud, pas deux |
| Mémoire | `testAADTape.cpp` | taille de la tape après *N* chemins = après 1 chemin (`rewind_to_mark` réutilise) |
| Check-pointing | `testAADTape.cpp` | risques avec marque = risques en enregistrant tout (petit *N*) — **prouve la linéarité de §7.2** |
| Black-Scholes | `testAADSimulation.cpp` | delta, vega, rho adjoints = greeks analytiques à < 3 erreurs standard |
| Pathwise | `testAADSimulation.cpp` | adjoint = bump à nombres aléatoires communs, *h* → 0, **chemin par chemin**, à 1e-8 |
| Clone | `testAADSimulation.cpp` | risques d'un modèle cloné = ceux de l'original (`set_param_pointers`) |
| Parallèle | `testAADParallel.cpp` | payoffs bit-à-bit égaux au série ; risques bit-à-bit (variante §7.4) avec 1, 2, 8 threads |
| Multi | `testAADMulti.cpp` | matrice multi-adjoints = *m* AAD mono-adjoint |
| ET | `testAADExpression.cpp` | mêmes risques avec et sans expression templates, à la précision machine |
| Limite | `testAADSimulation.cpp` | delta adjoint d'une digitale **dure** = 0 ; non nul et correct **lissée** — la limite est documentée par un test |
| Coût | `benchmarks/bench_aad.cpp` | ratio (coût AAD / coût pricing) **plat** en fonction du nombre de paramètres ; celui du bump **linéaire** — la courbe du lot 15 |

Le benchmark va dans [`benchmarks/`](../../benchmarks/) avec google-benchmark,
à côté de `bench_vanilla_mc.cpp`, sous le preset `release`.

---

## 15. Séquencement

L'ordre suit celui du livre, avec l'intégration insérée dès qu'il y a quelque
chose à montrer.

| Lot | Contenu | Critère d'acceptation |
|---|---|---|
| **17a** | `blocklist`, `Node`, `Tape`, `Number` sans ET | tests opérateurs, fonction jouet, mémoire — verts sous `asan` |
| **17b** | Instrumentation (§6) : `bs_sim_model`, `simulatable_asian`, `parameters()`, `clone()` ; `simulate_aad` série avec check-pointing et risques par lot | BS : greeks adjoints = analytiques ; test de check-pointing vert |
| **17c** | `RiskReport`, `GreeksMethod`, binding, API | delta / vega / rho adjoints visibles dans la réponse de l'API |
| **17d** | `ConcurrentQueue`, `ThreadPool`, `RNG::skip_to` (PCG32 + Sobol), `simulate_parallel_aad` | reproductibilité bit-à-bit 1/2/8 threads |
| **17e** | `MultiAssetBSSimModel<T>`, `LocalVolSimModel<T>` ; benchmark ; surface du lot 15 | **le tableau du panier à 50 sous-jacents**, et la courbe coût / nombre de paramètres |
| **17f** | Multi-adjoints | matrice = *m* runs mono |
| **17g** | Expression templates | mêmes risques, tape plus petite, gain de temps mesuré au benchmark |
| **17h** | Calibration : Dupire C++ templé, superbucket ; théorème des fonctions implicites | ∂V/∂σ_impl ; **attend** Dupire en C++ et le framework de calibration |

**17a + 17b + 17c est le livrable défendable minimal** : « j'ai écrit une tape
AAD, un `Number` à surcharge d'opérateurs, et un Monte-Carlo adjoint avec
check-pointing qui redonne les greeks de Black-Scholes. » **17e est la
démonstration** : c'est le tableau que la roadmap veut en tête du README.

Et le [lot 16](16-scripting.md) en bénéficie sans rien faire de plus :
`Evaluator<Number>` sur un `ScriptedProduct` donne les sensibilités de **tout
payoff scripté**.

---

## 16. Hors périmètre

- **AAD sur GPU.** Le chantier 3 de la roadmap porte le Monte-Carlo en CUDA ;
  l'AAD sur GPU est un sujet de recherche à part (tape en mémoire partagée,
  propagation par warp). Le livre ne le traite pas.
- **Ordre 2 exact** (tangent sur adjoint) — voir §12.
- **Transformation de source** (Tapenade, Enzyme) — écarté en ADR-A1.
- **AAD à travers une régression LSMC** — viendra avec le LSMC et l'xVA.

---

## 17. Décisions

### ADR-A1 — Écrire l'AAD, par surcharge d'opérateurs et tape

**Décision.** Implémentation maison, fidèle au livre : `Number` à surcharge
d'opérateurs, tape en `blocklist`.

**Pourquoi.** C'est l'objet même du chantier : la roadmap interdit d'annoncer
l'AAD sans l'avoir écrit, et la valeur du projet est de pouvoir l'expliquer
jusqu'au pointeur. La maîtrise du check-pointing et de la tape par thread est
aussi ce qui distingue une implémentation sérieuse d'un appel de librairie.

**Écarté.** *Adept, CoDiPack, XAD* : librairies ouvertes solides, mais on
délègue précisément ce qu'il faut savoir faire. *dco/c++ (NAG)* : commercial.
*Transformation de source* (Tapenade, Enzyme) : autre paradigme que celui du
livre, outillage de compilation lourd.

### ADR-A2 — `thread_local Tape*` : l'exception assumée à « aucun état statique mutable »

**Décision.** `Number::tape` est un pointeur statique `thread_local`.

**Pourquoi.** La checklist [`etc/todo.md`](../../etc/todo.md) §8 exclut l'état
statique mutable. Mais une surcharge d'opérateur (`a * b`) ne peut pas recevoir
la tape en argument : elle doit la trouver quelque part. `thread_local` rend cet
état **sûr par construction** (un thread ne voit que sa tape) et c'est le seul
point de ce type dans le cœur. L'exception est écrite ici pour ne pas être
contournée ailleurs.

### ADR-A3 — Espace de noms `quantModeling::aad`

**Décision.** `Node`, `Tape`, `Number`, `blocklist` vivent dans
`quantModeling::aad`.

**Pourquoi.** Les deux livres de Savine appellent `Node` deux choses
différentes : le nœud de tape ici, le nœud d'AST dans le
[lot 16](16-scripting.md#2-last). Les noms du livre sont gardés **dans** des
espaces de noms distincts (`aad::Node`, `scripting::Node`).

### ADR-A4 — Noms de classes du livre, méthodes en snake_case

**Décision.** `Number`, `Tape`, `blocklist`, `ThreadPool` comme dans le livre ;
les méthodes suivent la convention du repo : `put_on_tape`,
`propagate_to_mark`, `rewind_to_mark`.

**Pourquoi.** L'architecture de simulation a déjà été portée ainsi
(`generatePath` → `generate_path`, `payoffLabels` → `payoff_labels`). Mélanger
les deux conventions dans le même appel serait pire que l'un ou l'autre. La
table de correspondance (§18) garde le livre lisible ligne à ligne.

### ADR-A5 — D'abord sans expression templates

**Décision.** 17a livre un `Number` sans ET ; les ET arrivent en 17g derrière
la même API.

**Pourquoi.** C'est la progression du livre, et elle a une raison : les ET sont
une optimisation qui ne doit changer **aucune** sensibilité. La version simple
est l'oracle qui le prouve. Commencer par les ET, c'est déboguer la
métaprogrammation et l'AAD en même temps.

### ADR-A6 — Risques par lot plutôt qu'une propagation unique

**Décision.** `propagate_mark_to_start()` par lot de 64 chemins, adjoints de la
section d'initialisation remis à zéro entre deux lots (§7.4). C'est un écart au
livre, qui propage une seule fois.

**Pourquoi.** Deux règles du projet : une erreur standard sur tout chiffre
Monte-Carlo, et la reproductibilité bit-à-bit en parallèle. Le coût — une
propagation de la section d'initialisation par lot — est négligeable devant 64
propagations de chemin.

**Écarté.** *Propagation unique (le livre)* : pas d'erreur standard, somme
dépendante de l'ordre des threads. *Adjoints par chemin* : erreur standard
exacte, mais une propagation de l'initialisation **par chemin** — trop cher dès
qu'elle est grosse (Dupire).

### ADR-A7 — `ThreadPool` : l'API du livre, sans le singleton

**Décision.** L'interface du livre (`spawn_task`, `active_wait`, `thread_num`)
à l'identique, mais le pool est un objet **possédé par l'appelant** — la couche
de bindings en tient un pour le processus — et passé par référence à
`simulate_parallel_aad`.

**Pourquoi.** Dans le livre, le pool est un singleton : c'est une commodité, pas
une nécessité algorithmique. La checklist interdit les singletons globaux dans
le cœur ; la seule exception acceptée est la tape (ADR-A2), et elle est
`thread_local`. `thread_num()` reste statique car il lit une variable
`thread_local`.

### ADR-A8 — L'interface `RNG` du livre sur PCG32 et Sobol

**Décision.** L'interface `RNG` du livre (`init`, `next_u`, `next_g`, `clone`,
`skip_to`, `sim_dim`) implémentée sur les générateurs existants ; MRG32k3a, le
générateur du livre, n'est pas porté.

**Pourquoi.** Ce que l'AAD parallèle exige, c'est l'**interface** — surtout
`skip_to` en temps logarithmique — pas un générateur particulier. PCG32 et le
Sobol existant la satisfont tous deux, et ils sont déjà testés
(`testSobol.cpp`). Porter MRG32k3a reste possible pour une fidélité stricte :
c'est une implémentation de plus de la même interface.

---

## 18. Correspondance livre → repo

| Livre | Repo | Fichier |
|---|---|---|
| `blocklist` | `aad::blocklist` | `aad/blocklist.hpp` |
| `Node`, `propagateOne`, `propagateAll` | `aad::Node`, `propagate_one`, `propagate_all` | `aad/node.hpp` |
| `Tape`, `recordNode<N>` | `aad::Tape`, `record_node<N>` | `aad/tape.hpp` |
| `rewind`, `mark`, `rewindToMark` | `rewind`, `mark`, `rewind_to_mark` | `aad/tape.hpp` |
| `Number`, `putOnTape` | `aad::Number`, `put_on_tape` | `aad/number.hpp` |
| `propagateToStart`, `propagateToMark`, `propagateMarkToStart` | `propagate_to_start`, `propagate_to_mark`, `propagate_mark_to_start` | `aad/number.hpp` |
| `Expression`, `BinaryExpression`, `OPMult`… | idem, `push_adjoint`, `num_numbers` | `aad/expression.hpp` |
| `Sample`, `SampleDef`, `Scenario` | `Sample<T>`, `SampleDef`, `Scenario<T>` | [`core/sample.hpp`](../../include/quantModeling/core/sample.hpp) — **déjà là** |
| `Product<T>` | `ISimulatableProduct<T>` | [`instruments/simulatable.hpp`](../../include/quantModeling/instruments/simulatable.hpp) — **déjà là** |
| `Model<T>` | `ISimulationModel<T>` | [`models/simulation_model.hpp`](../../include/quantModeling/models/simulation_model.hpp) — **déjà là** |
| `allocate` + `init` | `init` (fusionnés) | idem |
| `generatePath`, `simDim` | `generate_path`, `sim_dim` | idem |
| `parameters`, `parameterLabels`, `putParametersOnTape` | `parameters`, `parameter_labels`, `put_parameters_on_tape` | idem — **à ajouter** (§6.2) |
| `RNG`, `skipTo` | `RNG`, `skip_to` | `utils/rng.hpp`, `utils/sobol.hpp` |
| `mcSimul` | `simulate` | [`engines/mc/simulation_engine.hpp`](../../include/quantModeling/engines/mc/simulation_engine.hpp) — **déjà là** |
| `mcSimulAAD`, `mcParallelSimulAAD` | `simulate_aad`, `simulate_parallel_aad` | `engines/mc/simulation_engine_aad.hpp` |
| `AADSimulResults` | `AADSimulResults` | idem |
| `ThreadPool`, `spawnTask`, `activeWait`, `threadNum` | `ThreadPool`, `spawn_task`, `active_wait`, `thread_num` | `utils/thread_pool.hpp` |
| `ConcurrentQueue` | `ConcurrentQueue` | `utils/concurrent_queue.hpp` |

---

## 19. Bibliographie

| Sujet | Référence |
|---|---|
| Le livre de référence du lot | Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley 2018 |
| Logique floue, AAD des scripts | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021 |
| L'article fondateur des greeks adjoints | Giles & Glasserman, « Smoking Adjoints: Fast Monte Carlo Greeks », *Risk*, 2006 |
| Différentiation algorithmique en finance | Capriotti, « Fast Greeks by Algorithmic Differentiation », *Journal of Computational Finance*, 2011 |
| Théorie de la différentiation algorithmique | Griewank & Walther, *Evaluating Derivatives*, SIAM, 2ᵉ éd. 2008 |
| Mise en œuvre | Naumann, *The Art of Differentiating Computer Programs*, SIAM 2012 |
| Pathwise, likelihood ratio | Glasserman, *Monte Carlo Methods in Financial Engineering*, Springer 2003, ch. 7 |
