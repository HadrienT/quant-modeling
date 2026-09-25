# WP 16 — Scripting de payoffs (Andreasen & Savine)

| | |
|---|---|
| **Dépend de** | l'architecture timeline / scénario et la couche dates, livrées sur `feature/dated-mc` — voir [§0](#0-lacquis-sur-lequel-on-branche) |
| **Bloque** | l'AAD sur payoff quelconque, le moteur d'exposition xVA ([`etc/roadmap.md`](../../etc/roadmap.md) §4c) |
| **Branche** | `core/scripting` |
| **Référence** | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021 |

> **Premier lot du blueprint qui porte sur le cœur C++ et non sur `web/`.** Il y
> est parce que c'est un gros morceau de conception et que la convention du
> projet ([`CLAUDE.md`](../../CLAUDE.md)) veut que ceux-là vivent dans
> `blueprint/wp/`. La partie front de ce lot (§8.4) est volontairement mince.

## Objectif

Décrire un produit **en texte** plutôt qu'en C++, et le pricer avec le moteur
Monte-Carlo générique déjà en place. Un script est une suite d'**événements**
`(date, instructions)` ; le moteur ne connaît aucun type de produit.

L'objectif déclaré est une implémentation **fidèle au livre de Savine** :
même découpage (nœuds / visiteurs / évaluateur), mêmes noms là où ils sont
parlants, même mécanique de logique floue. Les écarts sont explicités en
[§12](#12-décisions) — ils viennent tous de l'adaptation à l'architecture déjà
présente ici, pas d'une préférence de style.

## Pourquoi ce n'est pas un 25ᵉ produit

La règle de la roadmap est d'**arrêter d'ajouter des produits**. Le scripting va
dans l'autre sens : il en **retire**.

| | Aujourd'hui | Après |
|---|---|---|
| Autocall | `instruments/equity/autocall.hpp` + `engines/mc/autocall.cpp` (163 l.) | un script |
| Cliquet, TARN, barrière discrète, worst-of à coupons… | un couple instrument + engine chacun | un script chacun |
| Nouveau structuré | ~300 lignes de C++, un test, une entrée de registry, un binding, un schéma Pydantic | un `POST` avec un texte |

Et surtout : l'évaluateur est **templé sur le type numérique**. Le jour où la
tape AAD existe, `Evaluator<Number>` donne **toutes les sensibilités de
n'importe quel payoff scripté** sans écrire une ligne d'engine. C'est le
chaînage `scripting → AAD → xVA` du livre, et c'est ce qui justifie l'effort.

---

## 0. L'acquis sur lequel on branche

Rien de ce qui suit n'est à écrire : c'est livré et testé sur `feature/dated-mc`.

| Brique | Fichier | Ce que le scripting en fait |
|---|---|---|
| `TimeLine`, `merge_timelines`, `add_monitoring_steps`, `event_indices` | [`core/timegrid.hpp`](../../include/quantModeling/core/timegrid.hpp) | la timeline du script, fusionnée avec les pas du modèle |
| `SampleDef`, `Sample<T>`, `Scenario<T>` | [`core/sample.hpp`](../../include/quantModeling/core/sample.hpp) | ce que l'évaluateur lit |
| `ISimulatableProduct<T>` | [`instruments/simulatable.hpp`](../../include/quantModeling/instruments/simulatable.hpp) | **l'interface que `ScriptedProduct` implémente** |
| `ISimulationModel<T>`, `BlackScholesSimModel<T>` | [`models/simulation_model.hpp`](../../include/quantModeling/models/simulation_model.hpp) | inchangés — le modèle ignore qu'il price un script |
| `simulate()` | [`engines/mc/simulation_engine.hpp`](../../include/quantModeling/engines/mc/simulation_engine.hpp) | inchangé — c'est le seul moteur |
| `Date`, `DayCounter`, `Calendar`, `Schedule`, `ValuationContext` | [`core/date.hpp`](../../include/quantModeling/core/date.hpp), [`market/`](../../include/quantModeling/market/) | résolution des dates du script en year-fractions |
| `SimulatableAsian<T>` | [`instruments/equity/simulatable_asian.hpp`](../../include/quantModeling/instruments/equity/simulatable_asian.hpp) | le patron à imiter, et le témoin des tests de parité |

**Le point d'ancrage tient en une phrase : `ScriptedProduct<T>` est un
`ISimulatableProduct<T>` de plus.** Aucun moteur, aucun modèle, aucun sampler
n'est modifié.

---

## 1. Le langage

### 1.1 Un exemple qui tient debout

Un autocall à quatre observations, mémoire de coupon, knock-in terminal :

```
2025-06-16
    spot0 = spot()
    ki = 0

2025-09-15  2025-12-15  2026-03-16
    if spot() < 0.70 * spot0 then ki = 1 endIf
    if spot() >= spot0 then
        pays 1000 * (1 + 0.02 * period)
        alive = 0
    else
        period = period + 1
    endIf

2026-06-15
    if alive = 1 then
        if ki = 1 and spot() < spot0 then
            pays 1000 * spot() / spot0
        else
            pays 1000
        endIf
    endIf
```

Trois choses à voir. Une **ligne de dates partagée** applique le même bloc à
plusieurs dates. Les **variables persistent d'un événement au suivant** (`ki`,
`period`, `alive`) — c'est ce qui rend le path-dependency exprimable. `pays`
inscrit un flux **à la date de l'événement courant**, déflaté par le numéraire
de cette date.

### 1.2 Grammaire (v1)

```ebnf
script      = { event } ;
event       = date { date } NEWLINE INDENT { statement } DEDENT ;
date        = "YYYY-MM-DD" ;

statement   = assign | pays | ifStmt ;
assign      = IDENT "=" expr ;
pays        = "pays" expr ;
ifStmt      = "if" cond "then" { statement }
              [ "else" { statement } ] "endIf" ;

cond        = condOr ;
condOr      = condAnd { "or" condAnd } ;
condAnd     = condElem { "and" condElem } ;
condElem    = "not" condElem | "(" cond ")" | comparison ;
comparison  = expr ( "=" | "!=" | "<" | "<=" | ">" | ">=" ) expr ;

expr        = term { ("+" | "-") term } ;
term        = factor { ("*" | "/") factor } ;
factor      = unary [ "^" factor ] ;                (* associatif à droite *)
unary       = [ "+" | "-" ] atom ;
atom        = NUMBER | IDENT | funcCall | "(" expr ")" ;
funcCall    = ("min"|"max"|"log"|"exp"|"sqrt"|"abs"|"smooth") "(" args ")"
            | "spot" "(" ")" ;
args        = expr { "," expr } ;
```

### 1.3 Périmètre

| | v1 (ce lot) | v2 (plus tard) |
|---|---|---|
| Marché | `spot()` — un seul sous-jacent | `spot(i)`, `df(T)`, `libor(T1,T2)`, `fwd(T)` |
| Dates | dates ISO littérales | `schedule(start, end, 3M, TARGET, MF)` généré par [`Schedule`](../../include/quantModeling/market/schedule.hpp) |
| Instructions | `=`, `pays`, `if/then/else/endIf` | boucles bornées, `assert` |
| Fonctions | `min max log exp sqrt abs smooth` | `pow`, `normcdf` |
| Conditions | comparaisons, `and or not` | — |
| Types | réel unique | booléens comme valeurs |

Pas de boucles, pas de récursion, pas de fonctions utilisateur : un script se
déroule en un nombre d'opérations connu à la compilation. C'est ce qui garantit
qu'on peut le dérouler sur une tape AAD sans exploser la mémoire.

---

## 2. L'AST

`include/quantModeling/scripting/node.hpp`

```cpp
struct Node;
using ExprTree = std::unique_ptr<Node>;      // possession simple, jamais shared_ptr

struct Node
{
    virtual ~Node() = default;
    std::vector<ExprTree> arguments;
    virtual void accept(ConstVisitor &) const = 0;
};
```

La hiérarchie suit celle du livre :

| Famille | Nœuds |
|---|---|
| Feuilles | `NodeConst` (valeur), `NodeVar` (nom + `index` rempli par le `VarIndexer`), `NodeSpot` |
| Arithmétique | `NodeAdd`, `NodeSub`, `NodeMult`, `NodeDiv`, `NodePow`, `NodeUplus`, `NodeUminus` |
| Fonctions | `NodeMin`, `NodeMax`, `NodeLog`, `NodeExp`, `NodeSqrt`, `NodeAbs`, `NodeSmooth` |
| Booléens | `NodeEqual`, `NodeNotEqual`, `NodeSuperior`, `NodeSupEqual`, `NodeInferior`, `NodeInfEqual`, `NodeAnd`, `NodeOr`, `NodeNot` |
| Instructions | `NodeAssign`, `NodePays`, `NodeIf` |
| Racine | `NodeCollect` — sommet d'une instruction, comme dans le livre |

Les nœuds de condition portent les champs que les passes remplissent :

```cpp
struct NodeComparison : Node        // base de =, !=, <, <=, >, >=
{
    double  eps       = 0.0;   // demi-largeur de lissage — DomainProcessor
    bool    discrete  = false; // l'expression ne prend que des valeurs isolées
    bool    alwaysTrue = false, alwaysFalse = false;  // ConstCondProcessor
};

struct NodeIf : Node
{
    std::size_t firstElse = 0;              // index du 1er argument de la branche else
    std::vector<std::size_t> affectedVars;  // slots écrits par l'une des branches
};
```

**L'AST n'est pas templé.** Il décrit la structure ; c'est l'évaluateur qui
porte le type numérique. C'est ce qui permet de parser une fois et d'évaluer en
`double` puis en `Number` sans reconstruire quoi que ce soit.

---

## 3. Lexer et parser

`include/quantModeling/scripting/{token,lexer,parser}.hpp` · `src/scripting/`

**Écrit à la main, sans dépendance** (voir [ADR-S1](#12-décisions)).

- **Lexer** : flux de `Token { Kind, lexeme, line, col }`. Reconnaît nombres,
  identifiants, mots-clés, opérateurs, dates ISO, et l'indentation (les blocs
  d'événement). Insensible à la casse pour les mots-clés.
- **Parser** : descente récursive, une fonction par niveau de la grammaire
  (`parseExpr` → `parseTerm` → `parseFactor` → `parseUnary` → `parseAtom`),
  précédence par la structure des appels — la méthode du livre. `^` associatif
  à droite via la récursion sur `parseFactor`.
- **Erreurs** : `ScriptError` (dérivé de `PricingError`, cohérent avec
  [`core/types.hpp`](../../include/quantModeling/core/types.hpp)) portant
  ligne, colonne et un extrait pointé. Un script mal formé est une erreur
  utilisateur, elle doit remonter jusqu'à l'API telle quelle.

Sortie : `std::vector<Event>` avec `Event { Date date; std::vector<ExprTree> statements; }`.

---

## 4. Les passes de pré-traitement

Toutes sont des **visiteurs**, exécutés **une fois** à la construction du
produit, jamais dans la boucle de chemins.

| Passe | Fichier | Rôle |
|---|---|---|
| `Debugger` | `visitors/debugger.hpp` | dump textuel indenté de l'AST — le premier harnais de test du parser |
| `VarIndexer` | `visitors/var_indexer.hpp` | `nom → slot` ; remplit `NodeVar::index`, produit `variableNames` |
| `ConstCondProcessor` | `visitors/const_cond.hpp` | replie les conditions constantes (`alwaysTrue` / `alwaysFalse`) et élimine la branche morte |
| `IfProcessor` | `visitors/if_processor.hpp` | remplit `NodeIf::affectedVars` — les slots écrits par l'une ou l'autre branche |
| `DomainProcessor` | `visitors/domain_processor.hpp` | propage les domaines, dérive `eps` et `discrete` pour chaque condition |
| `DeflineBuilder` | `visitors/defline_builder.hpp` | **spécifique au repo** : construit `std::vector<SampleDef>` |

### 4.1 `IfProcessor` — pourquoi il est indispensable

`NodeIf::affectedVars` sert deux fois. En évaluation dure, la branche `else`
doit pouvoir **restaurer** les variables que la branche `then` n'a pas écrites.
En évaluation floue (§5.2), c'est **la liste exacte des variables à mélanger**
entre les deux branches. Sans cette passe, le flou est faux ou coûte une copie
de tout l'état.

### 4.2 `DomainProcessor` — le cœur du flou

C'est la passe la plus subtile du livre. Elle propage à travers l'arbre un
`Domain` : un ensemble d'intervalles et de singletons décrivant **les valeurs
que l'expression peut prendre**.

```cpp
struct Bound   { double value; bool infinite; bool plusInf; };
struct Interval{ Bound left, right; bool isSingleton() const; };
class  Domain  { std::set<Interval> intervals; /* union, +, *, min, max, … */ };
```

Elle en tire, pour chaque condition `expr ⋈ 0` :

- **`discrete`** — le domaine de `expr` est un ensemble fini de points isolés
  (typiquement un drapeau `ki = 1`). Alors le lissage n'a **aucun sens** : la
  condition reste dure. C'est ce qui évite de lisser des tests logiques.
- **`eps`** — sinon, la demi-largeur de lissage, dérivée de la distance entre le
  domaine et zéro et d'un `defaultEps` de configuration.
- **`alwaysTrue` / `alwaysFalse`** — le domaine est entièrement d'un côté de
  zéro : la condition disparaît (via `ConstCondProcessor`).

C'est **exactement la brique qui rend le pathwise applicable aux digitales et
aux barrières** — le sujet déjà défriché dans
[`tests/testSmoothing.cpp`](../../tests/testSmoothing.cpp). Le scripting le
généralise à tout payoff au lieu de le coder produit par produit.

### 4.3 `DeflineBuilder` — l'adaptation à ce repo

Savine construit une `defline` à partir des accesseurs marché du script. Ici la
cible est le `SampleDef` déjà défini :

```cpp
struct SampleDef {
    bool numeraire = true;
    std::vector<Time> discount_mats;
    std::vector<Time> forward_mats;
};
```

La passe parcourt les instructions **événement par événement** et note :
`spot()` → l'événement a besoin de `spots[0]` ; `pays` → besoin du `numeraire` ;
(v2) `df(T)` → `discount_mats.push_back(t(T))`. Le résultat est le
`std::vector<SampleDef>` que `ISimulatableProduct::defline()` renvoie, donc ce
que le modèle lit dans `init()` pour dimensionner ses sorties. **Un script qui
ne regarde pas le spot à une date ne le fait pas simuler.**

---

## 5. L'évaluateur

`include/quantModeling/scripting/evaluator.hpp`

### 5.1 `Evaluator<T>` — le chemin chaud

```cpp
template <class T = Real>
class Evaluator : public ConstVisitor
{
    std::vector<T>            variables_;   // dimensionné par le VarIndexer
    std::vector<T>            stack_;       // pile d'évaluation, capacité fixée
    const Scenario<T>*        scenario_ = nullptr;
    std::size_t               eventIndex_ = 0;
    T                         payoff_{};    // accumulateur déflaté
public:
    void initialize();                       // remet variables_ et payoff_ à 0
    void setScenario(const Scenario<T>&, std::size_t eventIdx);
    T    payoff() const { return payoff_; }
};
```

Trois exigences, toutes tenues comme dans le livre :

1. **Parcours post-ordre avec une pile explicite**, pas de retour de fonction —
   les visiteurs poussent et dépilent. Pas de `std::function`, pas de
   `shared_ptr`, aucune allocation dans la boucle.
2. **Aucune allocation par chemin.** `variables_` et `stack_` sont dimensionnés
   une fois ; `initialize()` ne fait que réécrire des `T`.
3. **`NodePays`** dépile la valeur et fait
   `payoff_ += value / scenario_->at(eventIndex_).numeraire;` — la déflation est
   là et nulle part ailleurs, ce qui satisfait le contrat de
   `ISimulatableProduct::payoffs()` (« valeurs déjà divisées par le numéraire »).

### 5.2 `FuzzyEvaluator<T>` — la logique floue

Sous-classe qui remplace les booléens durs par un **degré de vérité** dans
`[0,1]`, empilé sur une pile de `double` séparée (les degrés ne sont jamais des
`Number` : ils ne portent pas de dérivée).

- `visit(NodeComparison)` : si `discrete`, comparaison dure (0 ou 1) ; sinon
  `callSpread(value, eps)` — une rampe linéaire, ou `NodeSmooth` si le script
  l'a demandé explicitement.
- `visit(NodeIf)` : soit `dt` le degré. Si `dt == 0` ou `dt == 1`, on n'évalue
  qu'une branche (le cas courant : **aucun surcoût sur les chemins loin de la
  frontière**). Sinon on évalue `then`, on sauvegarde `affectedVars`, on
  restaure, on évalue `else`, et on mélange
  `v = dt·v_then + (1−dt)·v_else` **sur `affectedVars` uniquement**.
- `and` / `or` / `not` : `min`, `max`, `1−x`.

Le résultat : un payoff **continu et dérivable presque partout** en les entrées
du chemin, donc un delta pathwise correct sur une barrière ou une digitale.

### 5.3 Le dispatch

Le point de performance du sous-système : l'évaluateur visite un nœud par
opération, par événement, **par chemin**. Sur 10⁶ chemins × 5 événements × 30
nœuds, c'est 1,5·10⁸ dispatches.

Retenu : **CRTP** (`template <class V> struct Visitor`) comme dans le livre, qui
supprime la table virtuelle sur le chemin chaud tout en gardant le visiteur
virtuel classique pour les passes froides (§4), où la lisibilité prime.
Alternative examinée en [ADR-S2](#12-décisions).

### 5.4 Thread-safety

L'AST est **const et partagé** ; l'`Evaluator` porte tout l'état mutable. Donc :
**un évaluateur par thread**, l'AST par référence. C'est le même contrat que
`ISimulationModel::clone()` et ça se parallélise sans verrou.

---

## 6. `ScriptedProduct<T>`

`include/quantModeling/instruments/scripted_product.hpp`

```cpp
template <class T = Real>
class ScriptedProduct final : public ISimulatableProduct<T>
{
public:
    ScriptedProduct(const std::string& script, const ValuationContext& ctx,
                    const ScriptSettings& settings);

    const TimeLine&                 timeline() const override;
    const std::vector<SampleDef>&   defline()  const override;
    const std::vector<std::string>& payoff_labels() const override;
    std::size_t                     n_underlyings() const override { return 1; }
    void payoffs(const Scenario<T>&, std::vector<T>& out) const override;
private:
    std::vector<Event>       events_;      // AST, const après construction
    TimeLine                 timeline_;
    std::vector<SampleDef>   defline_;
    mutable Evaluator<T>     evaluator_;   // état de chemin — voir §5.4
};
```

Le constructeur enchaîne : lexer → parser → `VarIndexer` → `ConstCondProcessor`
→ `IfProcessor` → `DomainProcessor` → `DeflineBuilder`, puis résout les `Date`
en `Time` via le `ValuationContext` et appelle `canonical_timeline`.

`ScriptSettings { bool fuzzy = false; double defaultEps = 0.01; }` choisit
l'évaluateur.

---

## 7. Les dates

Le script porte des **dates calendaires**, l'architecture de simulation ne
connaît que `Time`. La couture est déjà écrite :

```
texte du script ──► parser ──► Event{Date} ──► ValuationContext::t() ──► TimeLine
                                                                          │
                                              évaluateur & modèle ────────┘
```

Conséquences pratiques :

- Les événements **au passé strict** (date ≤ date de valorisation) sont retirés
  de la timeline et leurs `spot()` remplacés par des **fixings historiques**
  fournis en entrée. Sans ça, un autocall vivant depuis six mois n'est pas
  priçable. C'est un point de conception à traiter dès la v1, pas après.
- Deux événements séparés de moins de `TIMELINE_EPS` sont fusionnés par
  `canonical_timeline` — leurs instructions sont concaténées dans l'ordre.
- Le générateur de `Schedule` (v2) produit une liste de `Date` que le parser
  consomme comme si elles étaient littérales : **aucun impact sur l'AST**.

---

## 8. Intégration dans la chaîne

### 8.1 Registry et adapters

Nouveau `InstrumentKind::Scripted`. Mais le `ScriptedProduct` passe par
`simulate()`, pas par un `EngineBase` visiteur : l'adapter est direct, sans
entrée dans le `std::variant` de [`pricers/inputs.hpp`](../../include/quantModeling/pricers/inputs.hpp)
(un script est une `std::string`, pas une structure de paramètres).

### 8.2 Bindings

`price_script(script, valuation_date, spot, rate, dividend, vol, day_count,
fuzzy, default_eps, n_paths, seed, sampler)` — même patron que
`price_dated_asian` dans [`src/module.cpp`](../../src/module.cpp), retour par
`pricing_result_to_dict`.

### 8.3 API

`POST /price/scripted` avec `ScriptRequest` (le script en `str`, les paramètres
de marché, les réglages MC). Les erreurs de parsing remontent en **422** avec
ligne et colonne, via le handler `RequestValidationError` existant de
[`api/app/main.py`](../../api/app/main.py).

### 8.4 Front

Une page `/scripting` : éditeur CodeMirror avec coloration du langage,
bibliothèque de scripts d'exemple (autocall, cliquet, barrière), panneau de
résultats réutilisant `Uncertainty` / `EngineTag` de
[WP 01](01-design-system.md). Les erreurs de parsing sont soulignées dans
l'éditeur à partir de la ligne/colonne renvoyées.

**Attention à la route** : ne pas la placer sous `/price/*`, le proxy Vite y
renvoie tout vers l'API (leçon du lot précédent).

### 8.5 Le choix du modèle — et ce qui prévient qu'il est inadapté

Un script décrit un payoff, **jamais** la dynamique dont son prix dépend : le
modèle est un choix distinct, fait à l'appel. Le produit ne transmet au modèle
que trois choses — les dates d'événements, le nombre de sous-jacents
(`spot(i)`) et les `df()` demandés — rien sur la vol, les sauts ou le smile.

`price_script(…, model=…)` sélectionne le modèle ([`script_model_factory.hpp`](../../include/quantModeling/scripting/script_model_factory.hpp)) :

| `model` | Dynamique | Entrées |
|---|---|---|
| `auto` (défaut de l'API) | le modèle que le script exige, voir [§8.6](#86-choix-automatique-du-modèle) | `ticker`, ou `spot` + `vol` sans marché |
| `black_scholes` | une vol plate ; avec `spot(1)`… : Black-Scholes multi-actifs corrélé (`MultiAssetBSSimModel`), une vol plate par actif | `spot`, `vol` ; ou `underlyings` (tickers, ou spot/vol saisis + `correlation`) |
| `local_vol` | surface de Dupire (Euler, `steps_per_year`) | `ticker` : la chaîne d'options **stockée** est calibrée (`calibrate_vol_surface`) ; spot et dividende viennent aussi de la base |
| `heston` | Heston calibré sur la surface SVI (simulation Bates sans sauts, Euler à troncature complète) | `ticker` |
| `slv` | vol stochastique-locale : le Heston calibré × un levier L(S,t) qui reproduit les marginales de Dupire | `ticker` |

**Les données viennent de la base, jamais de Yahoo.** [`market_snapshot.py`](../../api/app/market_snapshot.py)
lit ce que `~/data-ingest` a stocké (snapshot d'options, clôtures, rendements de
dividende) et n'importe pas `yfinance` du tout — propriété structurelle, testée.
Une donnée absente ou trop ancienne est une erreur qui dit quoi rafraîchir
(`ingest run …`), pas un repli en direct. Un snapshot est une **date de marché** :
l'axe des maturités de la surface part du jour de capture, donc le script est
valorisé au dernier snapshot antérieur ou égal à `valuation_date` (au plus 4 jours
avant : week-end + férié), et la réponse le signale (`market_date_shifted`). Un
cours en retard sur la chaîne est accepté jusqu'à 7 jours mais signalé
(`stale_spot`) : spot et surface décriraient alors deux jours différents. Une
ligne de dividende absente veut dire « jamais ingéré » (un non-payeur est stocké
à 0,0), pas « zéro ».

**Le garde-fou.** Une passe d'analyse lit sur l'arbre ce dont le prix dépend
([`script_analyzer.hpp`](../../include/quantModeling/scripting/visitors/script_analyzer.hpp)) — des faits
**structurels**, jamais une estimation numérique :

- `nonlinear_in_spot` : `max`/`min`/`abs`/comparaison appliqués à une valeur
  dérivée du spot → le prix dépend de la loi de spot, donc du smile ;
- `spot_threshold_test` : comparaison d'une valeur dérivée du spot (hors drapeau
  discret) à un niveau — forme d'une barrière, d'une digitale, d'un déclencheur
  d'autocall, les plus sensibles au skew ;
- `path_dependent` : une variable dérivée du spot est affectée à un événement et
  lue à un événement ultérieur (somme courante, niveau de référence, drapeau de
  knock-in) → dépendance à la loi jointe, donc au smile forward.

La « teinte » « dérivé du spot » se propage aux variables, **y compris par le
flot de contrôle** : `if spot() < 70 then ki = 1` teinte `ki` alors que la
valeur affectée est une constante — c'est précisément ainsi qu'un drapeau
enregistre le chemin.

[`advise()`](../../include/quantModeling/scripting/model_advice.hpp) croise ces
drapeaux avec le modèle choisi et renvoie des `warnings` (`code`, `severity`,
`message`) : `flat_vol_smile` (vol plate face à un payoff non linéaire),
`forward_smile` (info : le forward smile de la vol locale est plus plat que
l'observé), `surface_extrapolated` (événements au-delà de la dernière maturité
calibrée). `validate_script` expose l'analyse avant tout pricing.

`spot(i)` (multi-sous-jacents) n'a pas de modèle côté API : `price_script` le
refuse explicitement au lieu de laisser l'évaluateur échouer.

### 8.6 Choix automatique du modèle

La même analyse sert à **choisir** le modèle, pas seulement à avertir
([ADR-S8](#adr-s8--le-modèle-est-choisi-pour-lutilisateur-et-annoncé)) :
[`recommend()`](../../include/quantModeling/scripting/model_advice.hpp) renvoie le
modèle le plus simple qui capture ce dont le prix dépend, parmi ceux que
l'appelant peut calibrer :

| Le script… | Modèle | Pourquoi |
|---|---|---|
| sans `ticker` (pas de surface stockée) | `black_scholes` | seul modèle disponible ; `flat_vol_smile` avertit si le payoff y est sensible |
| linéaire en spot | `local_vol` | tout modèle calé sur les forwards donne le même prix ; la surface fournit spot et dividende |
| dépend du spot de chaque date **séparément** (vanille, digitale, somme d'européennes) | `local_vol` | seules les marginales comptent, que Dupire reproduit exactement ; un modèle stochastique ajouterait du coût, pas de précision |
| porte un **état** d'une date à l'autre (barrière, moyenne, knock-in) | `slv` | le prix dépend du smile forward ; la SLV garde les marginales exactes et y ajoute une dynamique de vol |
| idem, mais la calibration stochastique échoue | `local_vol` | repli annoncé (`stochastic_calibration_failed`) |

Un script qui mêle une vanille et une barrière reçoit **un seul** modèle, celui
de sa partie la plus exigeante : on ne price pas deux morceaux d'un même payoff
sous deux lois du spot, et le modèle calibré reprice de toute façon la partie
vanille. `heston` seul n'est **jamais** recommandé — il ne reprice les vanilles
qu'à son erreur de calibration près — mais reste disponible à la main.

La page `/scripting` a `auto` par défaut ; la réponse porte `model_choice`
(modèle demandé, retenu, raison, calibration : paramètres Heston, erreur du fit
en points de vol, étendue du levier et part de la grille bloquée à ses bornes).
L'événement d'audit `pricing.valuation` enregistre le modèle **retenu** et ses
paramètres (`ModelSpec.params`, `calibration_id = ticker:snapshot`).

**Calibration** ([`stochastic_vol.py`](../../api/app/stochastic_vol.py)), une fois
par (ticker, snapshot, taux), en cache :

1. **Heston** ([`heston_calibration.hpp`](../../include/quantModeling/market/heston_calibration.hpp))
   sur la surface SVI déjà nettoyée, lue à k = z·σ_ATM·√T pour
   z ∈ {−2 … +1,5} et T ≥ 0,05 an. Levenberg-Marquardt sur les prix COS des
   options hors de la monnaie, résidus divisés par la vega de marché — l'écart
   de vol implicite au premier ordre, sans inversion par résidu ; l'erreur
   **exacte** en vol implicite est mesurée à la fin. Huit points de départ en
   parallèle (la vallée κ/ξ est plate). Le pricer COS est vectorisé par
   maturité : la fonction caractéristique ne dépend pas du strike.
2. **Levier SLV** ([`slv_calibration.hpp`](../../include/quantModeling/market/slv_calibration.hpp),
   méthode particulaire de Guyon & Henry-Labordère) sur la grille de Dupire de
   la même surface : 50 000 particules, graine fixe (un replay reproduit le prix).

**Limites connues, affichées plutôt que masquées.** Sur SPX/SPY, Heston ne suit
pas la pente de skew du court terme (≈ 2 points de vol de RMSE, κ souvent à sa
borne basse) : c'est la dynamique qu'on lui emprunte, les marginales viennent du
levier. La SLV est appliquée sans *mixing fraction* (vol de vol pleine) : la
calibrer demanderait des cotations d'exotiques qui ne sont pas stockées. Enfin la
SLV n'est exacte que là où la grille de Dupire l'est ; cette grille couvre
l'intervalle de log-moneyness observé par **toutes** les tranches (étroit quand
une tranche d'un jour est cotée) et hérite du bruit de ∂w/∂T d'une interpolation
linéaire en T.

---

### 8.7 Multi-sous-jacents et bibliothèque de produits

**Plusieurs sous-jacents.** Un script qui lit `spot(1)`, `spot(2)`… (jusqu'à 8)
est pricé sous Black-Scholes multi-actifs corrélé, seul modèle multi-actifs
branché : `recommend()` le choisit (`multi_asset`) et `advise()` signale la vol
plate par actif (`flat_vol_smile`) et la corrélation comme hypothèse
(`correlation`). Entrées ([`multi_asset_market.py`](../../api/app/multi_asset_market.py)),
toutes lues en base et enregistrées pour l'audit : clôture et rendement de
dividende par actif ; vol implicite **à la monnaie** du smile SVI de l'actif à
la dernière date du script, ou à défaut vol réalisée 63 jours (`vol_proxied`) ;
corrélation historique des rendements entre clôtures communes sur un an
(semi-définie positive par construction), avec un avertissement
`correlation_sparse_history` quand l'historique stocké a des trous de plus de
5 jours. Ou bien spot, vol, dividende et corrélation saisis.

**Bibliothèque.** Les produits structurés de Bouzoubaa & Osseiran (*Exotic
Options and Hybrids*, Wiley 2010) sont écrits **en scripts**, sans une ligne de
payoff C++ — c'est ce qui les rend compatibles avec la règle de la roadmap
« arrêter d'ajouter des produits » : le catalogue codé ne grossit pas.
42 scripts lisibles et commentés dans
[`web/src/features/pricing/scripting/library/`](../../web/src/features/pricing/scripting/library/),
un fichier par produit, avec un en-tête (titre, famille, nombre de
sous-jacents, sources, résumé) : vanilles et digitales, barrières et options à
toucher, asiatiques, lookbacks et ladders, cliquets (plafonné, à plancher global,
reverse, Napoléon), notes structurées (reverse convertible, barrière, discount,
bonus, twin-win, capital garanti, Phoenix, Athena, accumulateur, range accrual),
volatilité (variance, vol, corridor) et multi-actifs (basket, worst-of, best-of,
outperformance, rainbow, worst-of reverse convertible et autocall, Himalaya,
Everest, Atlas, Altiplano). Niveaux relatifs (`s0 = spot()` à la date de
strike) pour valoir sur n'importe quel ticker ; la page décale les dates par
semaines entières pour que le produit démarre une semaine après la date de
valorisation. [`test_script_library.py`](../../api/tests/test_script_library.py)
parse et price chaque fichier et vérifie des relations d'ordre (worst-of <
basket < best-of, barrière < vanille, variance swap ≈ écart de variance).

**Hors d'atteinte des scripts, et pourquoi** : les quantos et composites (pas
de modèle actions + change dans les scripts), les hybrides actions-taux (pas de
modèle de taux stochastique branché), les produits rappelables par l'émetteur
et les choosers (il faut une espérance conditionnelle — régression de type
Longstaff-Schwartz — que le langage n'a pas), la vol locale multi-actifs.

### 8.8 Du term sheet au script : le catalogue, la comparaison de modèles, le profil de risque

**Source unique.** Les 42 scripts vivent dans
[`api/app/product_library/`](../../api/app/product_library/) : l'API les possède,
le front les lit par `web/src/shared/products/scripted.gen.json`
(`scripts/gen_product_library.py`, commité, dérive vérifiée par la CI comme
`openapi.json`). Chaque script déclare ses **termes**
(`# param: barrier = 0.60 | percent | Knock-in barrier`) et les assigne au
premier événement ; [`product_templates.py`](../../api/app/product_templates.py)
ne remplace que ces nombres, et décale les dates par semaines entières. Le
fichier reste un script lisible, identique sur la page de scripting.

**Pricing.** `POST /price/scripted-product` (produit + termes + les entrées de
pricing communes à `ScriptRequest`) rend le script et le price ; la réponse
porte le script pricé, et l'audit l'enregistre comme toute valorisation. Sur la
page pricing, 42 entrées de catalogue générées (familles « Scripts · … »), dont
les quatre fiches documentaires désormais pricées (forward-start, cliquet,
Napoléon, corridor). Le modèle de dynamique est un champ du formulaire : `auto`
par défaut, les quatre au choix.

**Comparer les dynamiques.** « Compare the dynamics » price les mêmes termes
sous Black-Scholes, vol locale, Heston et SLV, même graine, même marché :
Black-Scholes sur un ticker prend la vol implicite à la monnaie du smile stocké
à la maturité du script. Un écart n'est affiché comme tel qu'au-delà de deux
erreurs standard combinées. Sur SPY (24/09/2026), un up-and-out call
(barrière 120 %) vaut 19,4 en vol plate, 29,1 en vol locale, 34,4 en SLV.

**Long ou short quoi.** La page produits porte, pour chaque produit, le tableau
de Bouzoubaa & Osseiran : le porteur est-il long ou short chaque paramètre de
marché. **Calculé**, pas affirmé ([`risk_profile.py`](../../api/app/risk_profile.py),
`POST /products/risk-profile`) : marché de référence affiché (spot 100, vol
25 %, taux 3 %, dividende 1 %, corrélation 50 %), niveaux figés à la date de
trade (la date de strike est un fixing passé), choc de chaque paramètre et
signe de la variation ; spot (delta), convexité (gamma), vol, taux,
dividendes, corrélation, et sur un sous-jacent le **skew** (vol locale qui
décroît avec le strike contre une surface plate, même moteur) et la **vol de
vol** (ξ de Heston). Erreurs **appariées** : 8 lots de tirages communs, l'erreur
d'une variation est la dispersion de ses 8 différences ; en deçà de deux
erreurs, « not significant », en deçà de 0,01 % du prix, « negligible ». Les
produits du catalogue écrit à la main sont chiffrés de même, par leur propre
pricer.

**Simulation.** La page simulation ajoute vol locale, Heston et SLV, simulés
par les modèles mêmes du pricer (`simulate_model_paths`,
`engines/mc/path_simulation.hpp`) et calibrés sur la chaîne **stockée**
uniquement (Black-Scholes et SABR gardent leur ancien chemin, repli compris).

## 9. Tests

Conformément à [`CLAUDE.md`](../../CLAUDE.md) : des tests de **propriété**, pas
des égalités numériques.

| Test | Fichier | Propriété |
|---|---|---|
| Round-trip parser | `testScriptParser.cpp` | `parse → Debugger → parse` est un point fixe ; précédence et associativité de `^`, `-`, `and`/`or` |
| Erreurs | `testScriptParser.cpp` | script mal formé → `ScriptError` avec la bonne ligne |
| Variables | `testScriptEval.cpp` | une variable écrite à l'événement *i* est lue à *j > i* ; `else` restaure ce que `then` n'a pas écrit |
| **Parité vanille** | `testScriptParity.cpp` | script « call européen » = `bs_call` fermé à < 3 σ |
| **Parité asiatique** | `testScriptParity.cpp` | script = `SimulatableAsian` à < 3 σ, **même graine** |
| **Parité autocall** | `testScriptParity.cpp` | script = `BSAutocallMCEngine` à < 3 σ — le test qui valide vraiment le langage |
| Domaines | `testScriptDomain.cpp` | `DomainProcessor` détecte `discrete` sur un drapeau 0/1 ; `alwaysTrue` sur `1 > 0` |
| Convergence du flou | `testScriptFuzzy.cpp` | `price(eps) → price(0)` quand `eps → 0`, **monotone** ; ordre mesuré |
| Lissage du delta | `testScriptFuzzy.cpp` | sur une digitale, `delta` par différences finies est **borné** en flou et explose en dur |
| Déterminisme | `testScriptEval.cpp` | même graine, même script → bit-à-bit identique |

Les trois tests de parité sont le critère d'acceptation du lot : **si un script
reproduit `BSAutocallMCEngine` à la précision Monte-Carlo près, le langage
fait son travail.**

---

## 10. Séquencement

| Lot | Contenu | Critère d'acceptation |
|---|---|---|
| **16a** | Token, lexer, AST, parser, `Debugger` | round-trip vert sur un corpus de 20 scripts ; aucune dépendance ajoutée |
| **16b** | `VarIndexer`, `IfProcessor`, `ConstCondProcessor`, `DeflineBuilder`, `Evaluator<Real>` **dur**, `ScriptedProduct` | **parités vanille + asiatique + autocall** vertes |
| **16c** | `DomainProcessor`, `FuzzyEvaluator<T>` | convergence en `eps` et lissage du delta verts |
| **16d** | Registry, binding, route API, page front | un autocall priçable depuis le navigateur |
| **16e** | v2 du langage : `spot(i)`, `df`, `schedule`, fixings historiques | un script multi-sous-jacent price contre `BSMountainMCEngine` |

**16a et 16b constituent le livrable défendable minimal** : « j'ai un langage de
payoff, un parser, un évaluateur visiteur, et un seul moteur MC qui price
n'importe quel script — validé contre mes engines dédiés. »

---

## 11. Hors périmètre v1

- **Exercice optimal / LSMC.** Un script décrit des flux conditionnels, pas une
  décision optimale. Le bermudéen demande une régression et viendra avec le LSMC
  ([`etc/roadmap.md`](../../etc/roadmap.md) §4c).
- **Taux et crédit.** `libor()`, `df()` et les courbes multiples attendent le
  chantier taux. La `SampleDef` a déjà les champs, ils resteront vides.
- **AAD.** Le lot livre `Evaluator<T>` templé et instancié en `Real` seulement.
  L'instanciation `Number` vient avec le [lot 17](17-aad.md) — et c'est grâce à
  [ADR-S3](#adr-s3--last-nest-pas-templé-sur-le-type-numérique) qu'elle ne
  demandera aucune modification de l'AST.
- **Suppression des engines dédiés.** Ils restent comme témoins de test. On ne
  les retire qu'après plusieurs mois de parité verte.

---

## 12. Décisions

### ADR-S1 — Lexer et parser écrits à la main

**Décision.** Aucune dépendance de parsing (ni ANTLR, ni Boost.Spirit, ni
Bison).

**Pourquoi.** La grammaire fait trente règles et le parser du livre tient en
~400 lignes lisibles. Une dépendance de génération apporterait une étape de
build, un langage de spécification à apprendre, et des messages d'erreur qu'on
ne contrôle pas — alors que la qualité des messages d'erreur est un critère du
lot. Le projet a par ailleurs une politique de dépendances minimales.

**Écarté.** *Boost.Spirit* : temps de compilation, erreurs template illisibles.
*ANTLR* : runtime C++ à vendorer, générateur Java au build.

### ADR-S2 — CRTP pour l'évaluateur, visiteur virtuel pour les passes

**Décision.** Deux mécanismes de visite : `Visitor<V>` en CRTP pour
l'évaluateur, `ConstVisitor` virtuel classique pour les passes de
pré-traitement.

**Pourquoi.** 1,5·10⁸ dispatches par pricing sur le chemin chaud contre une
poignée sur le chemin froid. Le CRTP y supprime l'indirection virtuelle et
autorise l'inlining ; sur les passes, le visiteur virtuel est plus court et
plus lisible, et son coût est nul.

**Écarté.** *`std::variant` + `std::visit`* : plus moderne, dispatch par table
de saut, mais impose un jeu de nœuds fermé dans un seul en-tête et s'éloigne du
livre — or la fidélité au livre est un objectif déclaré. À reconsidérer si le
CRTP se révèle pénible à faire évoluer.

### ADR-S3 — L'AST n'est pas templé sur le type numérique

**Décision.** `Node` et sa hiérarchie manipulent des `double` littéraux ; seul
l'`Evaluator<T>` porte `T`.

**Pourquoi.** Un script est parsé une fois et évalué des millions de fois,
potentiellement en deux types (`double` pour le prix, `Number` pour l'AAD).
Templer l'AST obligerait à reparser par type et doublerait le code généré pour
rien : la structure de l'arbre ne dépend pas du type numérique.

### ADR-S4 — `unique_ptr` pour la possession des nœuds

**Décision.** `using ExprTree = std::unique_ptr<Node>`, jamais `shared_ptr`.

**Pourquoi.** Un arbre a un propriétaire unique et une durée de vie statique
après construction. Le comptage atomique du `shared_ptr` n'apporte rien et
coûterait sur un arbre parcouru en boucle. Cohérent avec la convention
d'ownership du cœur C++.

### ADR-S5 — Le script porte des dates calendaires, pas des year-fractions

**Décision.** La syntaxe n'accepte que des dates ISO ; la conversion en `Time`
se fait à la construction du `ScriptedProduct`, via `ValuationContext`.

**Pourquoi.** C'est la raison d'être de la couche dates livrée juste avant : un
autocall se décrit par « le 15 décembre », pas par « 0,4548 ». Et ça rend le
script **indépendant de la date de valorisation** — le même texte se reprice le
lendemain sans réécriture.

### ADR-S6 — `DomainProcessor` insensible au flot en v1

**Décision.** Le domaine d'une variable est l'**union de tous** ses membres
droits d'affectation dans le script (une variable jamais affectée vaut `{0}`,
comme l'initialisation de l'évaluateur). Une condition voit ce domaine global,
pas celui qui l'atteint réellement.

**Pourquoi.** Le livre propage les domaines instruction par instruction et
unionne les branches d'un `if`. La version globale est une **sur-approximation
conservatrice** : elle peut lisser là où une analyse fine replierait
(`alwaysTrue` / `alwaysFalse`), jamais l'inverse — elle ne rend jamais `discrete`
une condition qui ne l'est pas. Suffisant pour les scripts réels, où les
drapeaux ne reçoivent que des littéraux. La version sensible au flot est un
raffinement ultérieur.

### ADR-S7 — Call spread centré, pas de widening d'intervalle

**Décision.** Le lissage d'une comparaison est le call spread centré
`clamp((x + eps)/(2 eps), 0, 1)` (ordre 2), pas la rampe unilatérale (ordre 1,
monotone). Le `Domain` n'a pas de widening : `x^y` et les formes non modélisées
retombent sur la droite réelle.

**Pourquoi.** Le call spread centré est celui du livre et converge plus vite ;
sa convergence n'est pas monotone terme à terme (l'erreur passe sous un plancher
de bruit Monte-Carlo), donc le test de [§9](#9-tests) mesure l'**ordre** dans le
régime où le biais domine plutôt qu'une décroissance stricte. Le widening
d'intervalle (abstraction de `total = total + x`) n'apporte rien tant qu'aucune
condition ne porte sur une variable accumulée.

### ADR-S8 — Le modèle est choisi pour l'utilisateur, et annoncé

**Décision.** L'API accepte `model="auto"` et en fait son défaut : le modèle est
choisi par `recommend()` à partir de l'analyse structurelle du script, annoncé
dans la réponse (`model_choice`) et enregistré dans l'audit. Les quatre modèles
restent sélectionnables à la main.

**Écart au livre.** Andreasen & Savine laissent le modèle entièrement à
l'appelant : le script décrit le produit, le modèle est fourni par ailleurs. Ce
découplage est conservé tel quel dans le cœur (`ScriptedProduct` ne sait rien du
modèle, `make_script_model` ne sait rien du script) ; le choix automatique est
une **politique** posée au-dessus, dans `model_advice.hpp` à côté de `advise()`,
qui lit la même analyse. La version précédente de ce lot écrivait « le choix
reste humain, l'outil le rend explicite » : le mainteneur a tranché pour un
choix par défaut, toujours explicite.

**Pourquoi.** Un utilisateur qui ne connaît pas les produits structurés ne sait
pas qu'une barrière dépend du smile forward et une vanille non. La règle d'un
desk — le modèle le plus simple qui capture ce dont le prix dépend, un seul
modèle par produit — se lit sur l'arbre, sans estimation numérique.

---

## 13. Bibliographie

| Sujet | Référence |
|---|---|
| Scripting, logique floue, xVA | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021 |
| Architecture de simulation, AAD | Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley 2018 |
| Le scripting en salle de marché | Andreasen & Huge, « Random Grids », *Risk*, 2011 |
| Lissage et pathwise | Glasserman, *Monte Carlo Methods in Financial Engineering*, §7.2–7.3 |
| Heston | Heston, « A Closed-Form Solution for Options with Stochastic Volatility », *RFS* 6(2), 1993 |
| Pricing COS | Fang & Oosterlee, « A Novel Pricing Method for European Options Based on Fourier-Cosine Series Expansions », *SIAM J. Sci. Comput.* 31, 2008 |
| Calibration SLV | Guyon & Henry-Labordère, « Being Particular About Calibration », *Risk*, janvier 2012 |
