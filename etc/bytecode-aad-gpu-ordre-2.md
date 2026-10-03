# Bytecode, AAD sur GPU et AAD d'ordre 2 — ce qui dépasse les livres

Les deux livres que le dépôt suit de près s'arrêtent avant trois choses qu'il
fait :

| Sujet | Ce que dit le livre | Ce que fait le dépôt | Lot |
|---|---|---|---|
| Évaluer un script | Andreasen & Savine parcourent l'**arbre** du script avec un visiteur | l'arbre est traduit en **bytecode**, exécuté par une petite machine à pile | WP 19, G2 |
| AAD | Savine enregistre une **tape** sur CPU | sur GPU il n'y a pas de tape : **nombres duaux** ou **adjoint écrit à la main**, chemin par chemin | WP 19, G3 |
| Ordre 2 | Savine prend des **différences finies sur l'AAD** | la méthode du livre, plus l'**adjoint sur tangent** là où il est exact | WP 17, 17i |

Ce document explique chacune sans supposer autre chose que la lecture des deux
livres. Il décrit le code tel qu'il est ; les spécifications et les décisions
sont dans `blueprint/wp/19-gpu.md` (§3, §6, ADR-G1, G4, G6) et
`blueprint/wp/17-aad.md` (§12, ADR-A10). L'ordre de lecture compte : l'AAD sur
GPU s'appuie sur le bytecode.

**Ce qui n'est pas couvert, pour ne pas le laisser croire** : les sensibilités
du CVA (WP 23, lot X8) passent par la tape sur CPU ; elles n'ont pas d'adjoint
sur GPU. L'AAD sur GPU décrite ici est celle des **scripts** de la page
Scripting et du workbench de pricing.

---

## Sommaire

- [Partie I — Le bytecode](#partie-i--le-bytecode)
- [Partie II — L'AAD sur GPU](#partie-ii--laad-sur-gpu)
- [Partie III — L'AAD d'ordre 2](#partie-iii--laad-dordre-2)
- [Annexe A — Vocabulaire du GPU](#annexe-a--vocabulaire-du-gpu)
- [Annexe B — Où est le code](#annexe-b--où-est-le-code)
- [Annexe C — Références](#annexe-c--références)

---

# Partie I — Le bytecode

## I.1 Le point de départ : l'arbre et son visiteur

Le parseur du livre de scripting transforme le texte d'un script en un
**arbre**. La ligne

```
pays max(spot() - 100, 0)
```

devient

```
Pays
 └─ Max
     ├─ Sub
     │   ├─ Spot
     │   └─ Const 100
     └─ Const 0
```

Chaque nœud est un objet alloué sur le tas, qui pointe vers ses enfants. Pour
l'évaluer sur un scénario, un **visiteur** (`Evaluator<T>`) descend dans
l'arbre : il visite `Pays`, qui visite `Max`, qui visite `Sub`… chaque visite
est un appel de fonction **virtuelle**, résolu à l'exécution, et le visiteur
range ses résultats intermédiaires dans une pile.

C'est ce que font Andreasen & Savine, et c'est très bien sur un CPU. C'est
aussi exactement ce qu'un GPU ne sait pas faire :

- **des pointeurs partout** : l'arbre vit dans la mémoire de l'hôte ; le
  copier sur la carte demanderait de retraduire chaque pointeur ;
- **des appels virtuels** : un kernel CUDA n'a pas de table de fonctions
  virtuelles utilisable ;
- **de la récursion** : la profondeur de pile d'un thread GPU est petite et
  doit être connue à l'avance ;
- **de l'allocation** : un thread GPU ne peut pas allouer.

Même sur CPU, sauter de nœud en nœud à travers des pointeurs coûte : chaque
saut est une lecture mémoire que le processeur ne peut pas prévoir.

## I.2 L'idée : aplatir l'arbre en une liste d'instructions

Un **bytecode** est la même chose que l'arbre, écrite à plat : un tableau
d'instructions très simples, à exécuter l'une après l'autre. C'est ce que font
Python (les fichiers `.pyc`) et Java (les `.class`) avec leur code source. Le
mot vient de ce que chaque instruction commence par un code d'opération qui
tient sur un octet.

La machine qui l'exécute est une **machine à pile**, c'est-à-dire une
calculatrice en notation polonaise inverse. Pour calculer `(S − 100)` on ne
dit pas « soustrais 100 à S », on dit :

1. pose S sur la pile ;
2. pose 100 sur la pile ;
3. *soustrais* : retire les deux du dessus, pose leur différence.

Les opérandes n'ont pas de nom : une opération prend toujours ce qui est au
sommet de la pile. C'est ce qui rend les instructions si simples.

Le script ci-dessus devient six instructions :

| pc | Instruction | Pile après (S = 120) |
|---|---|---|
| 0 | `Spot 0` | 120 |
| 1 | `Const 100` | 120, 100 |
| 2 | `Sub` | 20 |
| 3 | `Const 0` | 20, 0 |
| 4 | `Max` | 20 |
| 5 | `Pays` | *(vide)* — et `payoff += 20 / numéraire` |

`pc` est le *program counter* : le numéro de l'instruction en cours. On passe
de l'arbre à cette liste par un **parcours postfixe** : les enfants d'abord, de
gauche à droite, le nœud ensuite. C'est l'ordre dans lequel le visiteur du
livre calcule déjà ; le bytecode ne fait que l'écrire une fois pour toutes au
lieu de le redécouvrir à chaque chemin.

## I.3 Une instruction

`scripting/bytecode.hpp` :

```cpp
struct Instr {
    Op op;          // le code d'opération (un octet)
    int32_t a, b;   // deux entiers : un indice, une cible de saut
    double x;       // un réel : une constante, un epsilon de lissage
};
```

Quatre champs fixes : une instruction a toujours la même taille, le programme
est un simple tableau, qu'on copie sur la carte d'un seul `memcpy`. Les
opérations (`enum class Op`) se rangent en cinq familles :

| Famille | Instructions | Effet |
|---|---|---|
| Feuilles | `Const x`, `Var a`, `Spot a`, `Df a` | posent une valeur : une constante, la variable n° a, le spot de l'actif n° a, le facteur d'actualisation n° a |
| Arithmétique | `Add Sub Mul Div Pow Neg Min Max Log Exp Sqrt Abs Smooth` | retirent un ou deux opérandes, posent le résultat |
| Logique dure | `Eq Ne Gt Ge Lt Le And Or Not` | posent 1 ou 0 |
| Logique floue | `FCmpGt`… `FAnd FOr FNot FConst` | posent un **degré de vérité** dans [0, 1], sur une seconde pile |
| Instructions | `Assign a`, `Pays`, `JumpIfFalse a`, `Jump a`, `FIfBegin`, `FIfMid`, `FIfEnd` | affectent, paient, sautent |

Un programme complet (`struct Program`) est : le tableau `code`, et pour chaque
date d'événement la **plage** d'instructions à exécuter ce jour-là
(`event_begin[e]` à `event_begin[e + 1]`).

## I.4 Le `if` : des sauts

Une machine qui lit des instructions dans l'ordre ne sait pas « entrer dans une
branche ». Elle sait **sauter** : changer `pc`. Le script

```
if spot() >= 130 then alive = 0 endIf
```

devient

| pc | Instruction | Commentaire |
|---|---|---|
| 0 | `Spot 0` | |
| 1 | `Const 130` | |
| 2 | `Ge` | pose 1 si spot ≥ 130, 0 sinon |
| 3 | `JumpIfFalse 7` | retire le dessus ; s'il vaut 0, va en 7 |
| 4 | `Const 0` | la branche *then* |
| 5 | `Assign alive` | |
| 6 | `Jump 7` | fin du *then* : saute par-dessus le *else* (vide ici) |
| 7 | … | la suite |

Un détail d'écriture du compilateur, pour qui lira `scripting/compiler.hpp` :
quand il émet le `JumpIfFalse`, il ne connaît pas encore l'adresse où sauter
(le *then* n'est pas encore écrit). Il émet l'instruction avec une cible
vide, écrit le *then*, puis revient **remplir la cible**. C'est le
*backpatching*, et c'est tout ce que font les lignes

```cpp
const int jif = emit(Op::JumpIfFalse);
statements(n, 1, n.firstElse);
const int jmp = emit(Op::Jump);
p_.code[jif].a = pc();        // la cible, maintenant connue
```

## I.5 Le compilateur

`Compiler` est lui-même un visiteur de l'arbre, du même modèle que ceux du
livre : au lieu de *calculer* à chaque nœud, il *émet* l'instruction
correspondante.

```cpp
void binary(const Node &n, Op op) {
    n.arguments[0]->accept(*this);   // émet le code du fils gauche
    n.arguments[1]->accept(*this);   // puis celui du fils droit
    emit(op), pop(), pop(), push();  // puis l'opération
}
```

Il tourne **après** les passes du livre (indexation des variables, conditions
constantes, variables affectées par chaque `if`, domaines) : il en récolte les
résultats. Une variable n'est plus un nom mais un numéro (`Var 3`) ; une
condition toujours vraie n'émet aucun test, seulement sa branche.

`push()` et `pop()` ne touchent aucune pile : ils **comptent**. Le compilateur
suit la hauteur que la pile atteindra à l'exécution et en garde le maximum
(`max_stack`). Deux conséquences :

- la machine sait, avant le premier chemin, combien de cases il lui faut ;
  aucun thread n'alloue jamais rien ;
- un script qui lirait une valeur jamais posée est **refusé à la
  compilation** — par exemple, en mode flou, une comparaison utilisée comme un
  nombre — au lieu de lire hors de la pile à l'exécution.

## I.6 La machine

L'état d'un chemin (`Machine<T>`) :

| Champ | Contenu | Taille |
|---|---|---|
| `vars` | les variables du script | `n_vars` |
| `stack` | la pile de valeurs | `max_stack` |
| `degrees` | la pile des degrés de vérité (mode flou) | `max_degrees` |
| `if_slots`, `if_mode` | la mémoire des `if` flous (I.7) | `n_if_slots`, `n_if_modes` |
| `payoff` | la somme de ce qui a été payé, déflatée | 1 |

L'interpréteur, `run_event`, est une boucle et un `switch` :

```cpp
while (pc < end) {
    const Instr &in = p.code[pc++];
    switch (in.op) {
        case Op::Const: S[sp++] = T(in.x);       break;
        case Op::Spot:  S[sp++] = spots[in.a];   break;
        case Op::Sub: { const T b = S[--sp]; const T a = S[--sp]; S[sp++] = a - b; break; }
        case Op::Assign: m.vars[in.a] = S[--sp]; break;
        case Op::JumpIfFalse: if (dbl(S[--sp]) == 0.0) pc = in.a; break;
        ...
    }
}
```

Il ne manipule que des tableaux et des indices. Il est marqué
`QM_HOST_DEVICE` — une macro qui vaut `__host__ __device__` quand on compile
avec CUDA et rien sinon : **la même fonction** est compilée pour le CPU et pour
le kernel. Et il est **templé sur le type de nombre `T`**, comme tout le code
de pricing depuis le WP 17 : avec `T = double` il calcule un prix, avec
`T = aad::Number` il enregistre la tape du livre, avec `T = Dual<N>` il porte
des dérivées (Partie II).

## I.7 Le `if` flou

En mode flou (livre de scripting, lissage des discontinuités), une condition
n'est plus vraie ou fausse : elle a un degré de vérité `d` ∈ [0, 1]. Quand
0 < d < 1 il faut **exécuter les deux branches** et mélanger leurs effets :

```
variable = d × (valeur après then) + (1 − d) × (valeur après else)
```

L'évaluateur flou du livre sauvegarde pour cela l'état des variables que le
`if` modifie. Le bytecode fait la même chose avec trois instructions :

| Instruction | Ce qu'elle fait |
|---|---|
| `FIfBegin` | retire le degré. S'il vaut 0 : saute au *else*. S'il vaut 1 : continue dans le *then*. Sinon, mode « mélange » : sauvegarde le degré, le payoff et les variables affectées |
| `FIfMid` | fin du *then*. En mode mélange : sauvegarde ce que le *then* a produit, **remet** les variables et le payoff d'avant, pour que le *else* parte du même état |
| `FIfEnd` | en mode mélange : mélange les deux résultats, variable par variable, et le payoff |

Où ranger ces sauvegardes ? L'évaluateur à arbre utilise une pile dynamique.
Ici on exploite une propriété du langage : **un script n'a ni boucle ni
récursion**, donc un `if` donné s'exécute au plus une fois par date, et deux
dates ne s'exécutent jamais en même temps. Le compilateur attribue donc à
chaque `if` un **emplacement fixe** (`FuzzyIf::slot`), et remet son compteur à
zéro à chaque nouvelle date : les emplacements de lundi resservent mardi. Sur
les 42 scripts de la bibliothèque, 19 emplacements suffisent ; sans
réutilisation il en faudrait 3 720 (un script à 249 dates répète le même `if`
249 fois). C'est ce qui fait tenir l'état d'un chemin dans la mémoire locale
d'un thread GPU.

## I.8 Ce que le bytecode ne sait pas faire

`exercise()` et `call()` — l'exercice anticipé par Longstaff-Schwartz — ne
sont pas compilés : le compilateur lève une erreur, et ces scripts restent sur
l'évaluateur à arbre, sur CPU. Une régression entre chemins n'est pas un
calcul « un chemin à la fois ».

## I.9 Ce que cela garantit, et ce que cela rapporte

- **Les mêmes bits que l'arbre.** Le bytecode exécute les mêmes opérations
  flottantes dans le même ordre que les deux évaluateurs du livre. Sur les 42
  scripts, prix durs, prix flous et **chaque risque AAD** sont identiques au
  bit près (`tests/testScriptBytecode.cpp`). L'arbre reste dans le dépôt comme
  **oracle** : c'est lui qui dit si le bytecode a raison.
- **Sur CPU**, le bytecode est environ deux fois plus rapide que l'arbre, et
  c'est lui que `ScriptedProduct` utilise par défaut.
- **Sur GPU**, il rend la chose possible : un thread par chemin avance le
  modèle et interprète le bytecode de chaque date. Mesuré, par chemin : ×100 à
  ×270 selon le script (`blueprint/wp/19-gpu.md` §3).

Une réserve consignée dans l'ADR-G1 : le livre évalue l'arbre ; qu'il décrive
ou non le principe d'une forme compilée n'a pas pu être vérifié à la rédaction
du lot, le livre n'étant pas dans le dépôt. C'est à confirmer par qui l'a sous
les yeux.

---

# Partie II — L'AAD sur GPU

## II.1 Pourquoi la tape de Savine ne monte pas sur la carte

Rappel du livre : chaque opération sur un `Number` crée un **nœud** sur la
tape (ses dérivées locales, des pointeurs vers les adjoints de ses arguments).
À la fin du chemin on parcourt la tape à l'envers pour propager les adjoints.
La tape grandit avec le nombre d'opérations ; le *check-pointing* du chapitre
Monte-Carlo la rembobine après chaque chemin pour qu'elle ne garde qu'un
chemin à la fois.

Sur GPU, cent mille chemins tournent **en même temps**, un par thread
(Annexe A). Il faudrait donc cent mille tapes simultanées. Trois obstacles :

1. **La mémoire.** La tape d'un chemin se compte en mégaoctets (deux, mesurés
   sur le moteur xVA du lot X8). Cent mille fois cela ne tient pas sur une
   carte de 16 Go — partagée, de surcroît, avec le LLM de l'assistant.
2. **L'allocation.** La tape de Savine alloue ses blocs au fur et à mesure
   (`blocklist`). Un thread GPU ne peut pas allouer.
3. **Les pointeurs et la taille variable.** Les nœuds pointent les uns vers
   les autres, et la longueur de la tape dépend du chemin : rien de cela ne se
   prête à des tableaux fixes.

D'où la décision ADR-G4 : **pas de tape générique sur la carte**. À la place,
deux mécanismes, choisis selon le nombre de paramètres :

| | Mode direct (nombres duaux) | Adjoint par chemin |
|---|---|---|
| Pour | Black-Scholes (1 ou 2 actifs), Heston : 4 à 8 paramètres | vol locale : spot, taux, dividende et **chaque point** de la grille σ_loc (363 pour 30 × 12, 1 500 pour 50 × 30) |
| Coût | proportionnel au nombre de paramètres | à peu près constant, quel que soit ce nombre |
| Mémoire | N + 1 réels par nombre, rien d'autre | une *trace* par thread, de taille connue |
| Code à écrire | aucun : le code templé suffit | l'adjoint de chaque instruction du bytecode, et celui du pas du modèle, **à la main** |

C'est la même alternative que dans le livre — mode direct contre mode
adjoint — avec le même critère : le mode direct coûte une passe par entrée,
l'adjoint une passe par sortie. Avec quatre entrées le direct est imbattable ;
avec mille il est exclu.

## II.2 Le mode direct : les nombres duaux

### L'idée

Un nombre dual est un couple (valeur, dérivée). On calcule avec lui comme avec
un réel, et chaque opération met à jour la dérivée par la règle de dérivation
des fonctions composées :

```
(a, a') + (b, b') = (a + b,  a' + b')
(a, a') × (b, b') = (a b,    a' b + a b')
exp(a, a')        = (e^a,    e^a a')
```

On peut le voir comme un calcul avec un ε tel que ε² = 0 :
(a + a′ε)(b + b′ε) = ab + (a′b + ab′)ε. À la fin du calcul, la seconde
composante **est** la dérivée du résultat par rapport à l'entrée qu'on a
« semée » avec une dérivée de 1. Pas de passe arrière, pas de mémoire : la
dérivée voyage avec la valeur.

Pour N paramètres, on porte N dérivées à la fois (`utils/dual.hpp`) :

```cpp
template <int N> struct Dual {
    double v;       // la valeur
    double d[N];    // ∂v/∂θ_1 … ∂v/∂θ_N
};
```

Chaque opération fait une boucle sur les N directions. C'est le coût
proportionnel à N.

### Pourquoi il n'y a presque rien à écrire

Depuis le WP 17, modèles, produits et interpréteur sont écrits sur un type de
nombre générique `T` — c'est la condition de l'AAD du livre. `Dual<N>` est un
type de nombre de plus. La fonction qui simule un chemin et exécute le script,
`script_path<T>` (`engines/mc/script_path.hpp`), s'instancie donc telle quelle
sur `Dual<N>` :

```cpp
const ScriptModelInputs<Dual<N>> in = script_dual_inputs<N>(view);  // les graines
return script_path<Dual<N>>(view, in, draws, 1.0, nullptr);         // le chemin
```

`script_dual_inputs` **sème** : il donne à chaque paramètre la valeur 1 dans sa
propre direction et 0 ailleurs.

| Modèle | Directions, dans cet ordre |
|---|---|
| Black-Scholes, un actif | spot, taux, dividende, vol |
| Black-Scholes, n actifs | taux, puis spot, dividende et vol de chaque actif (1 + 3n) |
| Heston | spot, taux, dividende, v₀, κ, θ, ξ, ρ |

L'ordre est celui des étiquettes du modèle CPU : le vecteur de risques d'un
calcul GPU se compare case par case à celui de `simulate_aad`.

Le payoff d'un chemin revient avec ses N dérivées. La réduction entre chemins
est celle des prix, appliquée à chaque composante (`DualStats` : un
accumulateur de Welford par composante) : on obtient la moyenne et l'erreur
standard de chaque risque.

### Trois points où il faut faire comme la tape

Pour que le GPU donne **le même chiffre** que la tape du CPU, et pas seulement
un chiffre proche, les duaux reprennent ses conventions :

- **aux points anguleux**, le même côté : `max(l, r)` dérive `l` quand
  l > r, `min` quand l < r, `fabs` prend +1 en 0 ;
- **les comparaisons ne regardent que la valeur** : le flot de contrôle n'est
  pas dérivé, exactement comme les comparaisons de `Number` ;
- **rien n'est envoyé vers une constante.** La tape ne propage jamais
  d'adjoint vers une feuille constante ; en mode direct, une dérivée locale
  infinie ou indéfinie multipliée par une dérivée nulle donnerait `NaN`
  (`sqrt` en 0, `pow` d'une base négative — le cas se produit dans le variance
  swap). D'où la petite fonction

  ```cpp
  double times(double df, double dx) { return dx == 0.0 ? 0.0 : df * dx; }
  ```

  par laquelle passe toute règle de chaîne.

### Limites

- Huit directions au plus, donc deux actifs au plus en Black-Scholes.
- La pile d'un thread porte tout l'état du chemin en `Dual<8>` : environ
  13 Ko par thread. Le pilote CUDA réserve cette pile pour tous les threads
  que la carte peut porter et la garde après le calcul — 1,95 Go mesurés. Le
  code remet donc la limite de pile à sa valeur d'avant après chaque calcul
  (`StackLimitRestore`), sans quoi la carte ne rend pas la mémoire au LLM.

## II.3 L'adjoint par chemin : la vol locale

Ici N se compte en centaines : il faut un vrai mode adjoint. Sans tape.

### L'idée

Ce que la tape fait de façon générique — retenir, pour chaque opération, de
quoi la dériver plus tard — on le fait **à la main, pour les seules opérations
qui existent** : les instructions du bytecode (une quarantaine) et le pas du
modèle (un seul, ici). Chacune reçoit son adjointe, écrite une fois.

Reste la question de la mémoire : que faut-il retenir de l'aller pour pouvoir
revenir ? Bien moins qu'une tape :

- la tape retient la **structure** du calcul (qui dépend de qui) : ici elle
  est connue, c'est le programme ;
- elle retient **toutes** les dérivées locales : ici on ne garde que les
  quelques valeurs dont l'adjointe a besoin, et on les recalcule.

Ce qu'on retient s'appelle la **trace** (`Trail`) : une simple pile de réels,
écrite à l'aller, relue à l'envers au retour.

Le schéma d'ensemble d'un chemin (`script_lv_adjoint_path`) :

```
ALLER   pour chaque pas s = 0 … n−1 :
          empiler le spot S (avant le pas)
          avancer le modèle :  S ← pas_vol_locale(S, tirage(s))
          si une date d'événement tombe ici :
            exécuter son bytecode en enregistrant sur la trace

RETOUR  adjoint du payoff = 1
        pour chaque pas s = n−1 … 0 :
          si une date d'événement tombait ici :
            dérouler son bytecode à l'envers  →  ∂payoff/∂S à cette date
          dépiler le spot S d'avant le pas
          adjoint du pas : remonter ∂payoff/∂S d'un pas,
                           et ajouter sa part aux 4 coins de la maille de σ_loc
```

C'est le *check-pointing* du livre poussé au bout : non seulement on ne garde
qu'un chemin, mais de ce chemin on ne garde que le strict nécessaire.

### L'aller : ce qu'on écrit sur la trace

L'interpréteur est le même que pour un prix, avec un **enregistreur** en
paramètre de template (`run_event_impl<T, Rec>`). Avec `NoRecord` l'appel
disparaît à la compilation : le pricing ne paie rien. Avec `Trail`, chaque
instruction exécutée pousse :

1. les valeurs dont son adjointe aura besoin — **seulement si** elle en a
   besoin ;
2. puis son `pc`.

| Instruction | Ce qu'elle pousse avant son `pc` | Pourquoi |
|---|---|---|
| `Add`, `Sub`, `Neg`, `Const`, `Var`, `Spot`, `Assign` | rien | leur adjointe ne dépend d'aucune valeur |
| `Mul`, `Div`, `Pow`, `Min`, `Max` | les deux opérandes | ∂(ab)/∂a = b : il faut b |
| `Log`, `Abs` | l'opérande | ∂log(x) = 1/x |
| `Exp`, `Sqrt` | le **résultat** | ∂e^x = e^x : le résultat suffit |
| `Pays` | la valeur payée et le numéraire | |
| comparaison floue | l'écart x de la condition x ⋈ 0 | pour savoir si on est dans la bande de lissage |
| `FIfBegin`, `FIfMid`, `FIfEnd` | la branche prise ; au mélange, le degré, les payoffs et les valeurs des variables | |

Chaque événement commence par une **marque** (−1 ; un `pc` n'est jamais
négatif), pour que le retour sache où s'arrêter.

Pousser le `pc` en dernier est ce qui rend le retour trivial : en dépilant, on
lit d'abord *quelle* instruction a été exécutée — donc aussi **quelle branche
a été prise**, sans rien enregistrer de plus sur le flot de contrôle.

### Le retour : une pile d'adjoints

`reverse_event` (`scripting/bytecode_adjoint.hpp`) dépile la trace et applique
l'adjointe de chaque instruction. Son état (`AdjointMachine`) est le miroir de
la machine : un adjoint par variable, une **pile d'adjoints**, un adjoint du
payoff.

La règle qui organise tout : *une instruction qui, à l'aller, a retiré k
valeurs et en a posé une, retire au retour un adjoint et en pose k.* La pile
d'adjoints se vide et se remplit exactement à l'envers de la pile de valeurs.

```cpp
case Op::Add:  { const double g = S[--sp]; S[sp++] = g;      S[sp++] = g;      break; }
case Op::Sub:  { const double g = S[--sp]; S[sp++] = g;      S[sp++] = -g;     break; }
case Op::Mul:  { const double b = trail.pop(), x = trail.pop();
                 const double g = S[--sp]; S[sp++] = g * b;  S[sp++] = g * x;  break; }
case Op::Max:  { const double b = trail.pop(), x = trail.pop();
                 const double g = S[--sp];
                 const bool left = x > b;
                 S[sp++] = left ? g : 0.0;  S[sp++] = left ? 0.0 : g;          break; }
```

Les feuilles et les instructions font le lien avec le reste :

| Instruction | Adjointe | En mots |
|---|---|---|
| `Spot a` | `spots_adj[a] += S[--sp]` | l'adjoint au sommet est la sensibilité au spot : c'est la **sortie** |
| `Var a` | `a.vars[a] += S[--sp]` | lire une variable, c'est lui transmettre l'adjoint |
| `Const` | `--sp` | l'adjoint d'une constante est jeté |
| `Assign a` | `S[sp++] = a.vars[a]; a.vars[a] = 0` | l'adjoint de la variable passe à l'expression qu'on y a rangée, et repart de zéro : l'ancienne valeur a été écrasée |
| `Pays` | `S[sp++] = g / num; numeraire_adj += −g v / num²` | g est l'adjoint du payoff (1 au départ) |
| comparaison dure, `JumpIfFalse` | pose 0 | une condition dure ne transmet rien à ses opérandes |

### Un exemple entier

`pays max(spot() - 100, 0)`, avec S = 120 et un numéraire N.

**Aller.** La trace, du fond vers le sommet :

```
−1 │ 0 │ 1 │ 2 │ 3 │ 20  0  4 │ 20  N  5
 ↑    ↑   ↑   ↑   ↑      ↑         ↑
marque Spot Const Sub Const  Max      Pays
                          (opérandes, (valeur, numéraire,
                           puis pc)    puis pc)
```

**Retour.** L'adjoint du payoff vaut 1 ; la pile d'adjoints est vide.

| On dépile | Instruction | Action | Pile d'adjoints |
|---|---|---|---|
| 5, puis N et 20 | `Pays` | pose 1/N ; `numeraire_adj += −20/N²` | 1/N |
| 4, puis 0 et 20 | `Max` | 20 > 0 : l'adjoint va à gauche | 1/N, 0 |
| 3 | `Const` | jette l'adjoint du 0 | 1/N |
| 2 | `Sub` | pose g et −g | 1/N, −1/N |
| 1 | `Const` | jette l'adjoint du 100 | 1/N |
| 0 | `Spot` | `spots_adj[0] += 1/N` | *(vide)* |
| −1 | marque | fin de l'événement | |

Résultat : ∂payoff/∂S = 1/N. Si S valait 80, `Max` enverrait l'adjoint à
droite, vers la constante, et le spot recevrait 0. C'est l'indicatrice
actualisée qu'on attend — obtenue sans tape.

### Le `if` flou, à l'envers

Même logique, plus longue. À l'aller `FIfEnd` calcule
`v = d × v_then + (1 − d) × v_else`. Au retour, l'adjoint de `v` se partage :
`d` fois vers ce que le *then* a produit, `1 − d` fois vers ce que le *else* a
produit, et `(v_then − v_else)` fois vers **le degré** — c'est par ce dernier
terme que la sensibilité traverse la condition lissée. `FIfMid` et `FIfBegin`
défont dans l'ordre inverse les sauvegardes et restaurations de l'aller. Les
emplacements fixes de I.7 servent ici une seconde fois : ils portent les
adjoints des valeurs sauvegardées.

Une comparaison floue transmet à ses opérandes la pente de la rampe,
±1/(2ε), seulement quand la condition est strictement dans la bande de
lissage ; hors de la bande, rien — comme la tape.

### L'adjoint du pas de vol locale, écrit à la main

Le pas, à l'aller (`local_vol_step`) :

```
σ  = max( bilinéaire(grille ; S, t), 10⁻⁶ )
x  = (r − q − σ²/2) Δt + σ √Δt z
S' = S eˣ
```

Son adjointe (`local_vol_step_adjoint`), connaissant ā′ = ∂payoff/∂S′ :

```
ā_S  = ā′ eˣ                          (dérivée directe en S)
ā_x  = ā′ S eˣ
ā_r += ā_x Δt          ā_q −= ā_x Δt
ā_σ  = ā_x ( z √Δt − σ Δt )
```

puis ā_σ est réparti sur les **quatre coins** de la maille où le chemin se
trouvait, avec les poids de l'interpolation bilinéaire :

```
grille[i0, j0] += ā_σ (1 − w_K)(1 − w_T)      grille[i1, j0] += ā_σ w_K (1 − w_T)
grille[i0, j1] += ā_σ (1 − w_K) w_T           grille[i1, j1] += ā_σ w_K w_T
```

et — c'est le terme qu'on oublie — σ dépend de S par le poids w_K = (S − K₀)/ΔK :

```
ā_S += ā_σ × [ (1 − w_T)(g₁₀ − g₀₀) + w_T (g₁₁ − g₀₁) ] / ΔK     (à l'intérieur de la grille)
```

Hors de la grille la vol est plate en S et ce terme est nul ; si le plancher à
10⁻⁶ est actif, rien ne remonte à la grille. C'est la transcription exacte de
ce que la tape déduirait toute seule du code de l'aller ; l'écrire à la main
est ce qui coûte, et ce qui doit être vérifié (II.5).

Chaque pas ne touche que quatre points de la grille : le coût de l'adjoint ne
dépend pas de la taille de la grille. C'est ce qui rend le calcul des 363 ou
1 500 sensibilités à peu près aussi cher qu'un prix.

### Les tirages ne sont pas stockés

L'adjointe du pas a besoin du tirage `z` de ce pas. La tape l'aurait gardé.
Ici le générateur est **Philox**, un générateur *à compteur* : le tirage est
une fonction pure de (graine, chemin, pas), sans état. Au retour on le
**recalcule** (`draws.at(s, 0)`) : quelques opérations entières au lieu d'un
réel stocké par pas et par chemin. C'est une des raisons du choix de Philox
(ADR-G3).

## II.4 Sur la carte

### La taille de la trace est connue d'avance

Un script n'a pas de boucle : à une date donnée, chaque instruction s'exécute
au plus une fois. On peut donc **borner** ce qu'un événement poussera sur la
trace en lisant son bytecode (`trail_bound` : pour chaque instruction, 1 pour
le `pc`, plus 0, 1 ou 2 selon le tableau de II.3). Pour un chemin de vol
locale :

```
taille de la trace = nombre de pas  +  Σ sur les événements de trail_bound
```

C'est la propriété qui remplace l'allocation dynamique de la tape : la mémoire
de chaque thread se réserve **avant** le lancement.

### Un thread, seize chemins, une trace

Le moteur groupe les chemins en *blocs logiques* de 4 096 : 256 threads qui
traitent chacun 16 chemins l'un après l'autre. La trace d'un thread est remise
à zéro à chaque chemin et **resservie** ; sa ligne de gradient, elle, cumule
ses 16 chemins. La mémoire se compte donc par thread, pas par chemin : pour
102 400 chemins, 6 400 threads, soit pour une grille 30 × 12 (363 paramètres)
6 400 × 363 × 8 octets ≈ 19 Mo de lignes de gradient.

Le nombre de blocs par lancement n'est jamais une constante : il est calculé
depuis la mémoire libre de la carte (`cudaMemGetInfo`) et le coût d'un thread
(trace + ligne de gradient), en ne prenant que **la moitié** de ce qui est
libre, parce que les cartes sont partagées. Une carte trop pleine renvoie le
calcul sur le CPU en mode `auto`, avec la raison.

### La disposition en mémoire

La trace du thread `row` n'est pas un tableau contigu. Son élément k est à

```
base[k × rows + row]          (rows = nombre de threads du lancement)
```

c'est-à-dire que les éléments de même rang de tous les threads sont côte à
côte. Les 32 threads d'un *warp* exécutent la même instruction au même moment
(Annexe A) et accèdent donc à 32 cases **voisines** : la carte les lit en une
fois. C'est l'accès *coalescé* ; la disposition inverse, un tableau par
thread, ferait 32 lectures dispersées. La ligne de gradient suit la même
disposition.

### Additionner les gradients sans `atomicAdd`

Tous les chemins contribuent à la même grille de sensibilités. La solution
immédiate est un tableau partagé où chaque thread ajoute sa part par
`atomicAdd`. Elle a été écartée (ADR-G6) : l'addition flottante n'est pas
associative, et l'ordre dans lequel les threads arrivent dépend de
l'ordonnancement — le résultat changerait au dernier bit d'une exécution à
l'autre.

À la place, trois étages dans un ordre **fixé par les indices** :

1. chaque thread accumule dans **sa** ligne de gradient, que personne d'autre
   n'écrit ;
2. un second kernel, `fold_warps`, additionne les 32 lignes de chaque warp,
   dans l'ordre des lanes ;
3. l'hôte additionne les warps dans l'ordre des indices.

Mesuré : le même vecteur de risques **au bit près** pour un ou plusieurs
lancements, sur l'une ou l'autre des deux cartes.

Ces sommes par warp servent une seconde fois. Un warp, c'est 32 threads × 16
chemins = 512 chemins : ce sont les **lots** dont la dispersion donne l'erreur
standard de chaque risque — l'équivalent GPU des lots de 64 chemins de la tape
CPU (ADR-A6).

## II.5 Comment on sait que c'est juste

Un adjoint écrit à la main est faux au premier terme oublié, et rien ne le
signale : le chiffre a l'air plausible. D'où un oracle, et le plus strict
possible.

- **L'oracle est la tape du livre**, sur CPU (`simulate_aad`), qui a reçu pour
  cela l'option Philox : elle tire **exactement les mêmes chemins** que le
  GPU. Les risques doivent donc être égaux **à l'arrondi près**, pas à
  l'erreur Monte-Carlo près. Une erreur de formule ne peut pas se cacher dans
  le bruit.
- **Tout le code par chemin est `QM_HOST_DEVICE`** : l'adjoint du bytecode,
  l'adjoint du pas, les duaux. La CI, qui n'a pas de GPU, les exécute sur CPU
  et les compare à la tape script par script
  (`tests/testScriptAdjoint.cpp` : `BytecodeAdjointMatchesTheTape`,
  `LocalVolAdjointMatchesTheTape`, `ScriptDuals.*`). Le kernel ne fait
  qu'exécuter ce code déjà vérifié.
- **Sur la carte** (`tests/gpu/`, lancés à la main) : 42 scripts, durs et
  flous, 4 096 chemins — prix à 10⁻¹⁰, risques à 10⁻⁸ du plus grand ; en
  pratique 10⁻¹⁵.

Temps mesurés (`build-cuda/qm_gpu_risk_bench`, 102 400 chemins, mêmes chemins
des deux côtés) :

| Script (modèle) | Risques | Tape CPU, 1 thread | 1 V100 | Gain |
|---|---|---|---|---|
| Call (vol locale 30 × 12, pas quotidien) | 363 | 13,0 s | 69 ms | ×189 |
| Up-and-out (vol locale) | 363 | 18,4 s | 140 ms | ×132 |
| Phoenix autocall (vol locale) | 363 | 20,5 s | 107 ms | ×191 |
| Variance swap (vol locale) | 363 | 23,0 s | 180 ms | ×128 |
| Phoenix autocall (Black-Scholes, duaux) | 4 | 140 ms | 5,5 ms | ×25 |
| Phoenix autocall (Heston quotidien, duaux) | 11 | 30,6 s | 69 ms | ×444 |

La comparaison est contre **un** thread CPU ; elle dit ce que vaut la carte,
pas ce que vaudrait un CPU à 56 threads.

## II.6 Ce qui n'est pas fait

Dit dans les diagnostics de chaque calcul, et renvoyé sur le CPU en `auto` :

- les risques **SLV** : il faudrait écrire l'adjoint du pas SLV (grille de
  levier) ;
- **plus de deux actifs** : les duaux s'arrêtent à 8 directions ;
- **Sobol et la stratification** sous AAD : les deux moteurs adjoints sont
  pseudo-aléatoires ;
- l'**exercice anticipé** : pas de bytecode (I.8) ;
- les sensibilités du **CVA** (WP 23) : sur tape, sur CPU. L'adjoint par
  chemin sans tape du moteur d'exposition reste à écrire (amélioration A2 du
  WP 23).

---

# Partie III — L'AAD d'ordre 2

Le livre de Savine s'arrête à l'ordre 1 et indique, pour l'ordre 2, les
différences finies sur l'AAD. Le dépôt a les deux méthodes
(`engines/mc/simulation_engine_aad2.hpp`), qui rendent la même chose : **une
ligne de la hessienne** — pour une direction choisie θ_dir, toutes les
∂²V/∂θ_dir ∂θ_j, avec leurs erreurs standard. Avec le spot pour direction :
gamma, vanna, ∂delta/∂taux…

## III.1 La méthode du livre : un bump sur l'AAD

`simulate_aad_bumped_second_order`. On déplace θ_dir de ±h et on lance l'AAD
des deux côtés :

```
∂²V/∂θ_dir ∂θ_j  ≈  [ ∂V/∂θ_j (θ_dir + h)  −  ∂V/∂θ_j (θ_dir − h) ] / 2h        pour tous les j
```

Deux lancements d'AAD donnent **toute une ligne** ; une troisième, sans bump,
donne le prix et la dérivée première. En bump pur il faudrait de l'ordre de n²
pricings.

Ce que le dépôt ajoute au principe :

- **h** vaut 1 % de la valeur du paramètre (ou 1 % en absolu s'il est nul) ;
  le biais est en h² ;
- **nombres aléatoires communs** : les deux côtés tournent sur la même graine
  Philox, donc sur les mêmes chemins ;
- **l'erreur standard est celle des différences lot par lot.** `simulate_aad`
  rend sur demande les risques de chaque lot de 64 chemins ; on forme
  (Δ⁺ − Δ⁻)/2h **lot par lot**, et c'est la dispersion de ces différences qui
  donne l'erreur. Les deux côtés sont très corrélés ; les traiter comme
  indépendants surestimerait l'erreur d'un ordre de grandeur. C'est le seul
  écart au code du livre dans cette méthode (ADR-A10).

C'est **la méthode par défaut**, et la seule à utiliser sur un payoff scripté
(III.3 dit pourquoi). Mesuré sur un call flou (K = 100, ε = 1, S = 100,
σ = 25 %, 10⁵ chemins) : gamma 0,01543 ± 0,00029 pour 0,01547 en
Black-Scholes ; vanna 0,0698 pour 0,0696.

## III.2 L'extension : l'adjoint sur tangent

`simulate_aad_second_order`. C'est le mode d'ordre 2 classique de la
différentiation algorithmique (Griewank & Walther, ch. 5 ; Naumann, ch. 3),
absent du livre.

### L'idée

La hessienne, c'est le gradient du gradient. On sait calculer :

- **une dérivée directionnelle** par le mode direct (les duaux de II.2, à une
  seule direction) : pour un paramètre θ_dir, on obtient ∂V/∂θ_dir en même
  temps que V ;
- **le gradient de n'importe quelle quantité calculée** par le mode adjoint.

Il suffit de les empiler : calculer ∂V/∂θ_dir en mode direct, *en
enregistrant ce calcul sur la tape*, puis lancer l'adjoint **depuis cette
dérivée** au lieu de le lancer depuis le prix. On obtient le gradient de
∂V/∂θ_dir : la ligne de la hessienne. Sans bump, sans h, exacte chemin par
chemin.

### Le type

`aad/tangent.hpp` :

```cpp
template <class S> struct Tangent {
    S v;   // la valeur
    S d;   // sa dérivée dans UNE direction
};
```

C'est le nombre dual de II.2 à une direction, mais dont les deux composantes
sont d'un type `S` quelconque. Les opérations appliquent la règle de chaîne
**avec les opérations de S** :

```cpp
Tangent<S> operator*(const Tangent<S> &a, const Tangent<S> &b) {
    return { a.v * b.v,  a.d * b.v + a.v * b.d };
}
```

- avec `S = double`, c'est un dual ordinaire ;
- avec `S = aad::Number`, chaque `*` et chaque `+` de ces deux lignes est
  enregistré sur la tape. La tape contient alors le calcul de la valeur **et**
  celui de sa dérivée.

`Tangent<Number>` est appelé `SecondOrderNumber`. Comme modèles et produits
sont templés sur leur type de nombre, ils s'instancient dessus **sans une
ligne de plus** — c'est la troisième fois que cette discipline du WP 17 paie.

### Le déroulé

C'est le Monte-Carlo adjoint du livre (WP 17 §7), avec deux différences
marquées ◀ :

```
feuilles :   pour chaque paramètre j :
               mettre sa valeur sur la tape
               sa tangente = 1 si j est la direction, 0 sinon            ◀
init du modèle, enregistré une fois ; marque
pour chaque lot de 64 chemins :
  pour chaque chemin :
    rembobiner à la marque
    simuler le chemin, calculer le payoff      → result.v  (le prix)
                                                 result.d  (∂prix/∂θ_dir du chemin)
    propager l'adjoint depuis result.d jusqu'à la marque                 ◀
  propager de la marque au début
  lire l'adjoint de chaque paramètre :  ∂²prix/∂θ_dir ∂θ_j   du lot
```

`result.d` donne au passage la dérivée première, et `result.v` le prix.

Le coût est celui d'une AAD sur une tape plus longue (elle porte les
opérations de la valeur et celles de la tangente), au lieu de trois AAD pour
la méthode du livre.

### La symétrie comme contrôle

La hessienne est symétrique : ∂²V/∂S∂σ obtenue avec le spot pour direction
doit égaler celle obtenue avec la vol pour direction. Le test
`TheHessianIsSymmetricPathByPath` le vérifie chemin par chemin, et
`SquaredSpotMatchesItsClosedFormHessian` compare à une forme fermée.

## III.3 La limite, et pourquoi elle exclut les scripts

L'adjoint sur tangent dérive deux fois **le long de chaque chemin**, puis
moyenne. Cela ne donne la bonne dérivée seconde que si l'on peut échanger
dérivée et espérance deux fois, donc si le payoff est **C¹** dans les
paramètres, chemin par chemin : dérivée première continue.

Un payoff dur ne l'est pas : le call (S − K)⁺ a une dérivée en marche
d'escalier, dont la dérivée est nulle presque partout. Tout le gamma est dans
une masse de Dirac en S = K, qu'aucun chemin ne touche. Gamma pathwise : 0.

On pourrait croire que la logique floue règle cela. Elle ne le règle pas, et
le calcul le montre. Le call écrit

```
if spot() > K then pays spot() − K endIf
```

devient, en flou, avec x = S_T − K et la rampe H_ε(x) qui monte de 0 à 1 entre
−ε et +ε :

```
P(x) = x · H_ε(x)
```

Dans la bande, H_ε(x) = (x + ε)/2ε, donc

```
P(x)  = x (x + ε) / 2ε
P'(x) = (2x + ε) / 2ε        qui va de −½ (en x = −ε) à 3⁄2 (en x = +ε)
P''(x) = 1/ε
```

Hors de la bande, P′ vaut 0 à gauche et 1 à droite. **P est continu, mais P′
saute de −½ à chaque bord** : de 0 à −½ en −ε, de 3⁄2 à 1 en +ε. Ces deux
sauts sont deux masses de Dirac de poids −½ dans P″.

Le bilan sur toute la droite : la partie régulière de P″ apporte
(1/ε) × 2ε = **2**, les deux masses apportent **−1**, total 1 — ce qu'il faut,
puisque P′ passe de 0 à 1. Mais la dérivation chemin par chemin ne voit que
la partie régulière : elle rend **deux fois le gamma**.

Mesuré : 0,03073 ± 0,00055, pour 0,03093 prédit par ce calcul, et 0,01547 de
vrai gamma. Le test `AFuzzyCallsPathwiseGammaMissesTheKinksOfItsDerivative`
fixe cette limite pour qu'on ne la redécouvre pas.

La méthode du livre n'a pas ce défaut : le **delta** d'un payoff flou est
continu dans les paramètres, sa différence finie converge vers le gamma du
produit lissé.

Un lissage C¹ (une rampe à raccords lisses) rendrait l'adjoint sur tangent
juste partout. Ce serait changer le lissage du livre de scripting ; ce n'est
pas fait (ADR-A10).

## III.4 Laquelle utiliser

| | Bump sur AAD (livre) | Adjoint sur tangent |
|---|---|---|
| Fonction | `simulate_aad_bumped_second_order` | `simulate_aad_second_order` |
| Coût | trois AAD | une AAD sur une tape plus longue |
| Biais | en h² | aucun |
| Payoffs scriptés, digitales, barrières, vanilles | **oui** | **non** : faux d'un facteur 2 sur un call flou, nul sur un payoff dur |
| Payoffs réguliers (puissances, log-contrats, fonctions lisses) | oui | oui, exact |
| Rôle dans le dépôt | méthode par défaut | payoffs C¹ et vérifications |

Là où les deux sont valides elles s'accordent sur les mêmes chemins
(`BothMethodsAgreeWhereThePayoffIsSmooth`).

Deux précisions sur l'état du code :

- l'ordre 2 est **sur CPU** : il repose sur la tape. Rien de la Partie II ne
  le porte sur GPU ;
- il n'est **pas exposé** par l'API ni par le front à ce jour : les deux
  fonctions sont dans le cœur C++, avec leurs tests
  (`tests/testAADSecondOrder.cpp`).

---

# Annexe A — Vocabulaire du GPU

| Terme | Sens |
|---|---|
| **Hôte / device** | le CPU et sa mémoire / la carte et la sienne. Les données doivent être copiées de l'un à l'autre |
| **Kernel** | une fonction exécutée sur la carte, par des milliers de threads à la fois, chacun avec son indice |
| **Thread** | une exécution du kernel. Ici : un thread traite 16 chemins l'un après l'autre |
| **Warp** | 32 threads qui exécutent **la même instruction au même moment**. Si leurs chemins divergent (`if` pris par certains, pas par d'autres), le warp traite les branches l'une après l'autre : c'est la *divergence*, que la logique floue évite en évaluant les deux branches |
| **Lane** | la place d'un thread dans son warp, de 0 à 31 |
| **Bloc** | un groupe de threads lancés ensemble. Ici 256 threads = 8 warps = 4 096 chemins : un *bloc logique* |
| **SM** | *streaming multiprocessor* : l'unité de calcul de la carte. Une V100 en a 80 |
| **Accès coalescé** | les 32 threads d'un warp lisent 32 cases voisines : une seule lecture mémoire |
| **`atomicAdd`** | addition protégée dans une case partagée ; l'ordre des additions y dépend de l'ordonnancement |
| **`QM_HOST_DEVICE`** | macro du dépôt : `__host__ __device__` sous CUDA, rien sinon. Une fonction ainsi marquée est compilée pour les deux côtés |
| **Philox** | générateur à compteur : tirage = fonction de (graine, chemin, rang). Pas d'état, donc reproductible quel que soit le découpage, et recalculable au retour de l'adjoint |

# Annexe B — Où est le code

| Sujet | Fichier | Ce qu'il contient |
|---|---|---|
| Bytecode | `include/quantModeling/scripting/bytecode.hpp` | `Op`, `Instr`, `Program`, `Machine`, `run_event` |
| | `include/quantModeling/scripting/compiler.hpp` | `Compiler`, `compile_script` |
| | `tests/testScriptBytecode.cpp` | égalité bit à bit avec l'arbre |
| Chemin | `include/quantModeling/engines/mc/script_path.hpp` | `script_path<T>` : un chemin, modèle et script |
| Duaux | `include/quantModeling/utils/dual.hpp` | `Dual<N>` et ses opérations |
| Adjoint | `include/quantModeling/scripting/bytecode_adjoint.hpp` | `Trail`, `trail_bound`, `reverse_event` |
| | `include/quantModeling/engines/mc/script_adjoint.hpp` | graines des duaux, `local_vol_step_adjoint`, `script_lv_adjoint_path` |
| | `tests/testScriptAdjoint.cpp` | égalité avec la tape, sur CPU |
| Kernels | `src/gpu/script.cu` | `ScriptUnit`, `DualUnit`, `AdjointUnit`, `fold_warps` |
| | `include/quantModeling/engines/mc/logical_blocks.hpp` | les blocs logiques et l'ordre des sommes |
| | `benchmarks/` → `build-cuda/qm_gpu_risk_bench` | risques GPU contre tape CPU |
| Ordre 2 | `include/quantModeling/aad/tangent.hpp` | `Tangent<S>` |
| | `include/quantModeling/engines/mc/simulation_engine_aad2.hpp` | les deux méthodes |
| | `tests/testAADSecondOrder.cpp` | symétrie, forme fermée, la limite du flou |

# Annexe C — Références

Les deux premières sont celles que le dépôt suit ; les suivantes couvrent ce
que ce document ajoute.

- Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley,
  2018 — la tape, le Monte-Carlo adjoint, l'ordre 2 par bump.
- Andreasen & Savine, *Modern Computational Finance: Scripting for Derivatives
  and xVA*, Wiley, 2021 — l'arbre, les visiteurs, la logique floue.
- Griewank & Walther, *Evaluating Derivatives*, 2ᵉ éd., SIAM, 2008 — ch. 3
  (mode direct et mode adjoint), ch. 5 (ordre 2, adjoint sur tangent).
- Naumann, *The Art of Differentiating Computer Programs*, SIAM, 2012 — ch. 3,
  les modèles d'ordre 2.
- Giles & Glasserman, « Smoking adjoints: fast Monte Carlo Greeks », *Risk*,
  2006 — l'adjoint écrit à la main, pas par pas, d'une simulation.
- Salmon, Moraes, Dror & Shaw, « Parallel random numbers: as easy as 1, 2,
  3 », *SC'11*, 2011 — Philox et les générateurs à compteur.
- Aho, Lam, Sethi & Ullman, *Compilers: Principles, Techniques, and Tools*,
  2ᵉ éd., 2006 — machines à pile, génération de code, *backpatching* (ch. 6).
