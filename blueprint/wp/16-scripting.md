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

---

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

---

## 13. Bibliographie

| Sujet | Référence |
|---|---|
| Scripting, logique floue, xVA | Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021 |
| Architecture de simulation, AAD | Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley 2018 |
| Le scripting en salle de marché | Andreasen & Huge, « Random Grids », *Risk*, 2011 |
| Lissage et pathwise | Glasserman, *Monte Carlo Methods in Financial Engineering*, §7.2–7.3 |
