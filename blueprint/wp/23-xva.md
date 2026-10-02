# WP 23 — xVA : exposition, collatéral, marge initiale, ajustements et capital (chantier 4c–4d)

| | |
|---|---|
| **Dépend de** | Hull-White ajusté à la courbe et calibré (WP 21, `models/rates/hull_white_curve.hpp`), swaps / swaptions / bermudans (WP 21), courbes de hazard et bootstrap (WP 20, `market/credit_curve.hpp`), LSMC (`engines/mc/lsm.hpp`, `exercise()` dans les scripts), AAD (WP 17), GPU (WP 19) |
| **Débloque** | La page `/xva` réservée par le [WP 15 §4](15-future-quant-surfaces.md) ; le chantier 5 option C (arrêt optimal par réseaux) comme alternative à la régression |
| **Branches** | une par lot : `feat/xva-x0-formulas`, `feat/xva-x1-exposure`, … (§14) |
| **Lecture principale** | Gregory, *The xVA Challenge: Counterparty Risk, Funding, Collateral, Capital and Initial Margin*, 4ᵉ éd., Wiley 2020 |
| **Lecture d'implémentation** | Savine & Andreasen, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021 ; Savine, *AAD and Parallel Simulations*, 2018 |

Ce document a deux usages. C'est d'abord un **compagnon de lecture** de
Gregory : chaque notion est expliquée, avec ses formules et les chiffres
réglementaires qu'il faut connaître par cœur. C'est ensuite la
**spécification** du chantier : les §13 à §16 disent ce qu'on construit, dans
quel ordre, et comment on le teste.

Les chiffres réglementaires (§11, §5) sont ceux du cadre de Bâle en vigueur
(SA-CCR, CRE52 ; cadre CVA révisé, MAR50 ; marges des dérivés non compensés,
BCBS-IOSCO) et ceux de la méthodologie ISDA SIMM. **Avant de les coder, les
revérifier contre le texte primaire** cité au §18, et citer le paragraphe dans
le code : un chiffre recopié de mémoire n'a pas sa place dans une formule
réglementaire.

---

## Sommaire

0. [Notation et convention de signe](#0-notation-et-convention-de-signe)
1. [Le problème en une page](#1-le-problème-en-une-page)
2. [Vocabulaire](#2-vocabulaire)
3. [Mesurer l'exposition](#3-mesurer-lexposition)
4. [Réduire le risque : netting, clauses, collatéral, compensation centrale](#4-réduire-le-risque--netting-clauses-collatéral-compensation-centrale)
5. [La marge initiale](#5-la-marge-initiale)
6. [Probabilités de défaut et courbes de crédit](#6-probabilités-de-défaut-et-courbes-de-crédit)
7. [CVA et DVA](#7-cva-et-dva)
8. [Wrong-way risk](#8-wrong-way-risk)
9. [FVA et ColVA](#9-fva-et-colva)
10. [MVA](#10-mva)
11. [Capital réglementaire et KVA](#11-capital-réglementaire-et-kva)
12. [Gérer les xVA : le desk, la couverture, les sensibilités](#12-gérer-les-xva--le-desk-la-couverture-les-sensibilités)
13. [Architecture dans quant-modeling](#13-architecture-dans-quant-modeling)
14. [Les lots](#14-les-lots)
15. [Tests de propriété](#15-tests-de-propriété)
16. [Données et limites assumées](#16-données-et-limites-assumées)
17. [Aide-mémoire : les chiffres et ratios à connaître](#17-aide-mémoire--les-chiffres-et-ratios-à-connaître)
18. [Bibliographie](#18-bibliographie)
19. [Questions d'entretien](#19-questions-dentretien)
20. [Améliorations notées](#20-améliorations-notées)

---

## 0. Notation et convention de signe

| Symbole | Sens |
|---|---|
| $I$, $C$ | la banque (*institution*, nous) et la contrepartie |
| $V(t)$ | valeur de marché sans risque (*MtM*) du netting set à $t$, vue de $I$ |
| $C(t)$ | collatéral détenu par $I$ à $t$ (négatif si $I$ en a versé) ; VM et IM séparés quand il le faut : $VM(t)$, $IM_C(t)$ reçue, $IM_I(t)$ versée |
| $E(t) = \max(V(t) - C(t), 0)$ | exposition positive (ce que $I$ perd si $C$ fait défaut à $t$, avant recouvrement) |
| $N(t) = \min(V(t) - C(t), 0)$ | exposition négative (ce que $C$ perd si $I$ fait défaut) |
| $D(t)$ ou $P(0,t)$ | facteur d'actualisation sans risque (OIS) |
| $S_C(t)$, $S_I(t)$ | probabilités de survie de $C$ et de $I$ |
| $PD_C(t_{i-1}, t_i) = S_C(t_{i-1}) - S_C(t_i)$ | probabilité (non conditionnelle) de défaut de $C$ dans l'intervalle |
| $R$, $LGD = 1 - R$ | taux de recouvrement, perte en cas de défaut |
| $\lambda(t)$ | intensité de défaut (*hazard rate*) |
| $s$ | spread de crédit (CDS ou obligataire) |
| $FS_B$, $FS_L$ | spreads de financement emprunteur / prêteur de $I$ |
| $\Phi$, $\varphi$ | fonction de répartition et densité de la loi normale standard |
| MPoR | *margin period of risk* |

**Convention de signe (celle de Gregory).** Un ajustement est **négatif
quand c'est un coût** pour la banque, positif quand c'est un bénéfice :

$$
V_{\text{ajustée}} = V_{\text{sans risque}} + \underbrace{CVA}_{\le 0} + \underbrace{DVA}_{\ge 0} + \underbrace{FVA}_{\lessgtr 0} + \underbrace{MVA}_{\le 0} + \underbrace{KVA}_{\le 0} + ColVA
$$

C'est aussi la convention de flux de trésorerie de l'interface du projet
(payé = négatif) : un CVA s'affichera négatif. Beaucoup de papiers (et Green)
écrivent au contraire $V - CVA$ avec $CVA \ge 0$ ; le code porte la
convention de Gregory et le dit dans la doc de chaque fonction.

---

## 1. Le problème en une page

Le pricing classique (tout le reste de ce dépôt) suppose trois choses fausses :

1. **Personne ne fait défaut.** Un swap vaut $V$ quelle que soit la
   contrepartie. En réalité, si la contrepartie fait défaut quand le swap est
   dans la monnaie pour nous, on perd $LGD \times V^+$.
2. **On se finance au taux sans risque.** En réalité, une banque qui paie un
   flux non collatéralisé l'emprunte à son propre spread.
3. **Le bilan ne coûte rien.** En réalité, chaque transaction consomme du
   capital réglementaire, que les actionnaires rémunèrent à 10–15 %.

Chaque xVA corrige une de ces hypothèses :

| Ajustement | Hypothèse corrigée | Porte sur |
|---|---|---|
| **CVA** (*credit valuation adjustment*) | la contrepartie ne fait pas défaut | exposition positive × crédit de $C$ |
| **DVA** (*debit*) | nous ne faisons pas défaut | exposition négative × crédit de $I$ |
| **FVA** (*funding*) — FCA + FBA | on se finance au sans-risque | besoin de financement × spread de $I$ |
| **ColVA** (*collateral*) | le collatéral est rémunéré au taux d'actualisation | écart de taux, option de livrer le moins cher |
| **MVA** (*margin*) | la marge initiale ne coûte rien | IM future versée × coût de financement |
| **KVA** (*capital*) | le capital est gratuit | capital futur × coût du capital |

**Pourquoi c'est devenu central.** Pendant la crise de 2008, le Comité de Bâle
a estimé qu'environ **deux tiers des pertes de risque de contrepartie venaient
de la variation du CVA**, pas des défauts eux-mêmes (le marché repricait le
risque de crédit des contreparties encore vivantes). D'où, dans Bâle III, une
charge en capital spécifique au risque de CVA (§11.4), puis la généralisation
du collatéral, de la compensation centrale et de la marge initiale
obligatoire, qui ont à leur tour créé FVA, MVA et KVA. Gregory raconte ce
basculement : on est passé d'un risque de contrepartie « géré par des
limites » à un ensemble d'ajustements de valeur *pricés* dans chaque
transaction et gérés par un desk xVA.

**Le schéma de calcul** est le même pour tous les ajustements : on projette
une quantité future (exposition, besoin de financement, IM, capital), on
l'intègre contre un taux de coût (probabilité de défaut, spread) et on
actualise :

$$
XVA = -\sum_i \mathbb{E}\big[\,\text{quantité}(t_i)\,\big] \times \text{coût}(t_{i-1}, t_i) \times \text{survie jointe} \times D(t_i)
$$

Tout le travail d'implémentation est dans le $\mathbb{E}[\cdot]$ : simuler
les facteurs de risque, repricer le portefeuille à chaque date future sur
chaque chemin, appliquer le netting et le collatéral. C'est le **moteur
d'exposition** (§13) — et c'est pour ça que le chantier vient après le GPU et
l'AAD.

---

## 2. Vocabulaire

| Terme | Définition |
|---|---|
| **Risque de contrepartie** (CCR) | Risque qu'une contrepartie à un dérivé fasse défaut avant la dernière échéance, alors que la valeur du contrat est positive pour nous. Bilatéral par nature : l'exposition change de signe. |
| **Exposition** | Ce qu'on perdrait en cas de défaut, avant recouvrement : $\max(V - C, 0)$. Toujours $\ge 0$. |
| **Replacement cost** (RC) | Coût de remplacement du contrat chez une autre contrepartie = l'exposition au moment du défaut. |
| **Close-out** | Procédure de résiliation après défaut (ISDA Master Agreement 1992 / 2002) : on calcule un *close-out amount*, on fait le netting, on réclame le solde dans la faillite. |
| **Netting set** | Ensemble des transactions couvertes par un même accord de netting juridiquement valide. Les expositions se compensent **à l'intérieur** du netting set, jamais entre deux. |
| **CSA** (*Credit Support Annex*) | Annexe de l'ISDA qui fixe les règles du collatéral : seuil, MTA, montant indépendant, arrondi, collatéral éligible, décotes, taux de rémunération, fréquence d'appel. |
| **VM** (*variation margin*) | Collatéral qui suit la valeur du portefeuille ; il réduit l'exposition courante. Rehypothéquable (réutilisable). |
| **IM** (*initial margin*) | Collatéral supplémentaire qui couvre la **variation possible** de valeur pendant la MPoR ; ségrégué, non réutilisable, versé par les deux parties. |
| **IA** (*independent amount*) | Ancêtre contractuel de l'IM : montant fixe demandé à une contrepartie jugée risquée, souvent unilatéral. |
| **Seuil** (*threshold*, $H$) | Exposition en dessous de laquelle on n'appelle pas de collatéral. |
| **MTA** (*minimum transfer amount*) | Montant minimal d'un appel de marge (évite les virements minuscules). |
| **MPoR** | Durée entre le dernier collatéral effectivement reçu et le close-out, pendant laquelle l'exposition peut dériver sans être couverte : délai de détection du défaut, contestation, liquidation. 10 jours ouvrés en bilatéral par défaut (§17). |
| **CCP** | Chambre de compensation centrale ; devient contrepartie de chaque membre. Exige VM + IM + contribution au *default fund*. |
| **PD**, **LGD**, **EAD** | Probabilité de défaut, perte en cas de défaut, exposition au défaut : les trois ingrédients d'une perte de crédit, $\mathbb{E}[\text{perte}] = PD \times LGD \times EAD$. |
| **Wrong-way risk** (WWR) | Dépendance défavorable entre exposition et qualité de crédit de la contrepartie (§8). |
| **RWA** | *Risk-weighted assets* ; le capital minimal est 8 % des RWA (plus les coussins). |

---

## 3. Mesurer l'exposition

### 3.1 Les métriques

Toutes sont des fonctions du temps calculées sur la distribution de
$V(t) - C(t)$ à chaque date future :

| Métrique | Définition | Usage |
|---|---|---|
| **EFV** — *expected future value* | $\mathbb{E}[V(t) - C(t)]$ | FVA symétrique ; contrôle (martingale) |
| **EE** — *expected exposure* | $EE(t) = \mathbb{E}[\max(V(t) - C(t), 0)]$ | CVA, FCA |
| **ENE** / NEE — *expected negative exposure* | $ENE(t) = \mathbb{E}[\min(V(t) - C(t), 0)]$ | DVA, FBA |
| **PFE$_\alpha$** — *potential future exposure* | quantile $\alpha$ de $\max(V(t)-C(t), 0)$ (souvent de $V(t)-C(t)$, ce qui revient au même pour $\alpha$ élevé), $\alpha = 95\%$ ou $99\%$ | limites de crédit |
| **MPFE** | $\max_t PFE_\alpha(t)$ | la limite est comparée à ce pic |
| **EPE** — *expected positive exposure* | $\frac{1}{T}\int_0^T EE(t)\,dt$ | résumé scalaire ; capital |
| **Effective EE** | $EEE(t_k) = \max\big(EEE(t_{k-1}), EE(t_k)\big)$ : l'EE rendue non décroissante | capital IMM : on suppose que les transactions qui arrivent à échéance sont remplacées |
| **EEPE** — *effective EPE* | moyenne pondérée par le temps de l'Effective EE **sur la première année** (ou jusqu'à l'échéance si elle est plus courte) | $EAD_{IMM} = \alpha \times EEPE$ |
| **EAD** | exposition au défaut utilisée par le capital | §11 |

Identité à tester : $EE(t) + ENE(t) = EFV(t)$ chemin par chemin, donc en
moyenne.

**Actualisé ou non.** Pour le CVA on intègre l'EE **actualisée**,
$EE^*(t) = \mathbb{E}[D(t)\max(V(t)-C(t),0)]$ (sous la mesure risque-neutre
avec le numéraire du compte bancaire, ou $P(0,t)\,\mathbb{E}^{t}[\cdot]$ sous
la mesure $t$-forward). Pour les limites (PFE) et le capital IMM on rapporte
des expositions non actualisées.

**Mesure risque-neutre ou historique.** Le CVA est un *prix* : facteurs
simulés sous la mesure risque-neutre, calibrés sur les prix d'options. La
PFE et les limites sont des *mesures de risque* : Gregory rappelle qu'elles
devraient l'être sous la mesure historique (dérives et volatilités
historiques). En pratique beaucoup de banques simulent tout en risque-neutre
par économie ; le moteur doit accepter les deux calibrations sans changer de
code.

**Décision (mainteneur, 2026-10-01) : on suit Gregory.** Calibration
risque-neutre pour ce qui est un prix ($EE^*$, CVA, DVA, FVA) ; calibration
historique (dérive et volatilité estimées sur l'historique de taux FRED de la
base) pour ce qui est une mesure de risque (PFE, limites, profils EPE / EEPE
affichés). Un seul moteur, deux jeux de paramètres. Fait : lot X1b (§14.4).

### 3.2 Formules fermées : la loi normale

Si $V(t) \sim \mathcal{N}(\mu, \sigma^2)$ (sans collatéral) :

$$
EE = \mu\,\Phi\!\left(\frac{\mu}{\sigma}\right) + \sigma\,\varphi\!\left(\frac{\mu}{\sigma}\right),\qquad
ENE = \mu\,\Phi\!\left(-\frac{\mu}{\sigma}\right) - \sigma\,\varphi\!\left(\frac{\mu}{\sigma}\right),\qquad
PFE_\alpha = \mu + \sigma\,\Phi^{-1}(\alpha)
$$

Pour $\mu = 0$ : $EE = \sigma\,\varphi(0) \approx 0{,}40\,\sigma$,
$PFE_{95} = 1{,}645\,\sigma$, $PFE_{99} = 2{,}33\,\sigma$. **Le « 0,4 » de
Gregory** vient de là ($\varphi(0) = 1/\sqrt{2\pi} = 0{,}3989$).

Ces formules sont les **oracles** du lot X0 : un moteur de simulation qui ne
les retrouve pas sur un produit gaussien a un bug.

### 3.3 Les profils types

| Produit | Profil d'EE | Pourquoi |
|---|---|---|
| Forward, FX forward | $\approx 0{,}4\,\sigma\sqrt{t}$, croissant jusqu'à l'échéance | un seul flux final ; la diffusion grandit en $\sqrt{t}$ |
| Swap de taux | $\approx 0{,}4\,\sigma\sqrt{t}\,(T - t)$ (à une constante près), en cloche, maximum vers $t = T/3$ | deux effets opposés : la diffusion des taux croît en $\sqrt t$, la duration restante décroît en $T - t$ |
| Swap de devises (cross-currency) | croissant, dominé par l'échange final de notionnel | le risque FX sur le notionnel final ne s'amortit pas |
| Option achetée | $EE^*(t) = V(0)$ constante (actualisée), aucune ENE | la valeur actualisée est une martingale positive, sans flux intermédiaire |
| Option vendue (prime payée d'avance) | EE nulle | l'exposition est toujours négative |
| CDS | faible, puis un saut au défaut de la référence | le risque est concentré dans un événement |

Deux détails qui comptent à l'implémentation :

- **Les pics de paiement.** Juste après un flux, la valeur du swap saute.
  Si la grille de simulation ignore les dates de flux, on rate ces pics, et
  surtout avec collatéral (§4.4), où l'essentiel de l'exposition résiduelle
  vient des flux payés pendant la MPoR. La grille doit contenir les dates
  de flux.
- **L'EPE d'un forward** vaut $\frac{1}{T}\int_0^T 0{,}4\sigma\sqrt{t}\,dt = \frac{2}{3}\times0{,}4\,\sigma\sqrt{T} \approx 0{,}27\,\sigma\sqrt T$.

### 3.4 De la simulation à l'exposition

L'algorithme de référence (Gregory, et Pykhtin & Zhu 2007) :

1. **Grille de dates** $0 = t_0 < t_1 < \dots < t_n$ : serrée au début
   (quotidienne ou hebdomadaire sur le premier mois, là où la MPoR et l'EEPE
   se jouent), puis mensuelle, puis trimestrielle jusqu'à 30 ans ; plus
   **toutes les dates de flux** ; plus, avec collatéral, les dates décalées
   $t_i - MPoR$.
2. **Simulation des facteurs de risque** sur la grille : taux (Hull-White par
   devise), FX, actions, crédit (pour le WWR), avec leurs corrélations.
3. **Repricing** de chaque transaction à chaque date sur chaque chemin :
   formule fermée quand elle existe (swap sous Hull-White : obligations
   zéro-coupon en forme fermée), sinon approximation par régression
   (*American Monte Carlo*, §13.4).
4. **Agrégation** par netting set, puis application du collatéral.
5. **Statistiques** : EE, ENE, PFE, EFV… par date, et contributions par
   transaction.

C'est $N_{\text{chemins}} \times N_{\text{dates}} \times N_{\text{trades}}$
évaluations : $10^4 \times 300 \times 10^3 = 3 \times 10^9$ pour un
portefeuille modeste. C'est le calcul type des batchs de nuit des banques, et
la raison du GPU (§13.6).

---

## 4. Réduire le risque : netting, clauses, collatéral, compensation centrale

### 4.1 Netting

- **Payment netting** : les flux d'une même date et d'une même devise se
  règlent pour leur solde (réduit le risque de règlement, pas le CCR).
- **Close-out netting** : au défaut, toutes les transactions du netting set
  sont résiliées et leurs valeurs **additionnées** ; la perte porte sur le
  solde positif :

$$
E_{\text{netté}}(t) = \max\Big(\sum_k V_k(t), 0\Big) \;\le\; \sum_k \max\big(V_k(t), 0\big) = E_{\text{brut}}(t)
$$

L'inégalité (sous-additivité de $x \mapsto x^+$) est un test.

**Le facteur de netting de Gregory.** Pour $n$ expositions normales de même
variance et de corrélation moyenne $\bar\rho$, le rapport exposition nettée /
exposition brute vaut

$$
\text{facteur de netting} = \frac{\sqrt{n + n(n-1)\bar\rho}}{n}
$$

$= 1/\sqrt{n}$ pour des transactions indépendantes, $= 1$ pour des
transactions parfaitement corrélées. Autrement dit, le netting rapporte
beaucoup sur un portefeuille diversifié, rien sur un portefeuille
directionnel.

**Contributions, incrémentales et marginales.**

- **EE incrémentale** d'une nouvelle transaction : $EE(NS + \text{trade}) - EE(NS)$.
  C'est ce qu'on facture au moment de traiter. Elle peut être négative (la
  transaction réduit le risque du netting set).
- **EE marginale (allocation d'Euler)** : pour répartir l'EE d'un netting set
  sur ses transactions de façon que la somme retombe exactement sur le total.
  Sans collatéral, $x\mapsto x^+$ est homogène de degré 1, donc
$$
EE_{NS}(t) = \sum_k \mathbb{E}\big[V_k(t)\,\mathbf{1}_{\{V_{NS}(t) > 0\}}\big]
$$
  Chaque contribution se lit sur **les mêmes chemins** que le total : aucune
  simulation supplémentaire. Même chose pour le CVA marginal. Avec seuil et
  MTA l'homogénéité est perdue et l'allocation n'est qu'approchée — à dire.

### 4.2 Clauses de résiliation et de reset

- **Break clause** (mutuelle ou optionnelle) : droit de résilier à des dates
  fixées. Si on suppose qu'on l'exerce, l'exposition s'arrête à la première
  date ; Gregory insiste sur le fait que les banques l'exercent rarement pour
  des raisons commerciales, d'où un traitement prudent.
- **Reset** : la transaction est remise à la monnaie à des dates fixées
  (paiement de la valeur), ce qui ramène l'exposition à zéro périodiquement.
- **Clauses de déclenchement** (*additional termination events*, dégradation
  de notation) : peu efficaces précisément quand on en aurait besoin.

Côté moteur : une break clause est une troncature de la grille, un reset un
flux de valeur ; les deux se modélisent sans nouveau produit.

### 4.3 Collatéral : les paramètres du CSA

| Paramètre | Effet sur le collatéral appelé |
|---|---|
| Seuil $H_C$ (pour $C$), $H_I$ (pour $I$) | on n'appelle que l'exposition au-dessus du seuil |
| MTA | pas d'appel si la variation requise est inférieure à la MTA |
| Arrondi | le montant appelé est arrondi (par exemple à 10 000) |
| IA / IM | montant additionnel indépendant de $V$ |
| Fréquence | quotidienne depuis les règles de marge (§5.1) |
| Décote (*haircut*) | un titre de valeur $X$ ne compte que pour $(1-h)X$ |
| Unilatéral / bilatéral | qui verse |
| Taux de rémunération | taux payé sur le collatéral cash reçu (souvent €STR / SOFR / Fed Funds) |

**Collatéral requis** (VM, sans MTA ni arrondi) :

$$
C(t) = \max\big(V(t) - H_C, 0\big) - \max\big(-V(t) - H_I, 0\big)
$$

La MTA rend l'appel dépendant du collatéral déjà détenu :
on ne met à jour $C$ que si $|C_{\text{requis}} - C_{\text{détenu}}| \ge MTA$.
C'est une récursion **par chemin**, séquentielle dans le temps.

### 4.4 La période de marge en risque (MPoR)

Même sous un CSA parfait (seuil nul, appels quotidiens), il reste un risque :
entre le dernier collatéral reçu et le close-out, la valeur dérive.

**Modèle classique** (Gregory ; exigé par l'IMM) : le collatéral à $t$ est
celui qui correspondait à la valeur à $t - MPoR$ :

$$
E(t) = \max\big(V(t) - C(t - MPoR),\,0\big)
$$

Approximation fermée avec seuil nul : si $V$ diffuse avec une volatilité
annuelle $\sigma_V$, l'exposition résiduelle vaut

$$
EE_{\text{collat}} \approx 0{,}4\,\sigma_V\,\sqrt{MPoR}
$$

soit, avec 10 jours ouvrés ($\sqrt{10/250} = 0{,}2$), environ **8 % de
$\sigma_V$** : c'est pour ça qu'un portefeuille bien collatéralisé a un CVA
petit mais non nul.

**Modèle avancé** (Andersen, Pykhtin & Sokol 2017, que Gregory reprend) :
pendant la MPoR, chaque partie arrête de payer la VM à des moments
différents, et **les flux des transactions** continuent ou non. Le cas qui
fait mal : nous payons un flux à la contrepartie pendant la MPoR alors
qu'elle ne paie plus le sien (ni la VM). Les pics d'exposition autour des
dates de flux deviennent alors dominants. Deux variantes dans le papier :
« Classical+ » (les deux parties arrêtent tout paiement de flux en même temps
que la VM) et « Classical− » (les flux continuent jusqu'au bout) ; le
modèle complet distingue les dates d'arrêt de chaque partie.

Conséquence pour la grille : on a besoin de $V$ aux paires
$(t_i - MPoR,\,t_i)$ **et** de la liste des flux payés dans l'intervalle, par
chemin.

### 4.5 Risques résiduels du collatéral

Gregory en fait la liste, à connaître : risque de marché (MPoR), risque
opérationnel, risque de liquidité (vendre le collatéral reçu), risque de
crédit du collatéral (titre émis par un émetteur corrélé à la contrepartie :
wrong-way), risque de FX (collatéral dans une autre devise), risque de
financement (le collatéral qu'on doit verser, §9), risque juridique
(ségrégation).

### 4.6 Compensation centrale (CCP)

- Obligatoire depuis le G20 de Pittsburgh (2009) pour les dérivés standardisés
  (swaps de taux vanilles, indices CDS).
- **Novation** : la CCP s'interpose, devient contrepartie de chacun ;
  netting multilatéral.
- Exige **VM** (quotidienne, souvent intrajournalière), **IM** (§5.4) et une
  contribution au **default fund**.
- **Cascade de défaut** (*default waterfall*), dans l'ordre : IM du membre
  défaillant → sa contribution au default fund → *skin in the game* de la CCP
  → contributions des membres survivants → appels supplémentaires
  (*assessment rights*) → mesures de fin (réduction des gains, *tear-up*).
- Dimensionnement du default fund : **Cover 2** (défaut simultané des deux
  membres les plus exposés en scénario extrême).
- Le CCR ne disparaît pas : il devient un risque de *mutualisation* (le
  default fund) et un coût de financement (IM, donc MVA).

---

## 5. La marge initiale

L'IM couvre **la perte possible pendant la MPoR** avec une confiance élevée,
là où la VM couvre la perte déjà constatée. Avec une IM à 99 % sur
10 jours, l'exposition résiduelle devient faible : le problème se déplace du
risque de crédit vers le **coût de financement** de l'IM (MVA, §10).

### 5.1 Les règles de marge des dérivés non compensés (UMR, BCBS-IOSCO)

- **VM** obligatoire, seuil nul, pour toutes les entités couvertes (depuis
  mars 2017).
- **IM** obligatoire en six phases (septembre 2016 → septembre 2022), selon
  l'encours notionnel moyen (AANA) de dérivés non compensés du groupe :
  3 000 Md€ en phase 1, jusqu'à **8 Md€** en phase 6.
- **Confiance 99 %** unilatérale, **horizon 10 jours**, période calibrée
  incluant un stress.
- IM **versée par les deux parties, brute** (pas de netting entre IM versée et
  reçue), **ségréguée**, pas de réutilisation (sauf exception encadrée).
- **Seuil d'IM** jusqu'à **50 M€** par groupe de contreparties ; **MTA**
  jusqu'à **500 k€** (VM + IM combinées).
- Deux méthodes : la grille réglementaire (§5.2) ou un modèle agréé, en
  pratique **ISDA SIMM** (§5.3).

### 5.2 La méthode standard (grille)

IM brute $= \sum_k \text{notionnel}_k \times \%_{\text{classe}}$ :

| Classe | % du notionnel |
|---|---|
| Crédit, 0–2 ans | 2 % |
| Crédit, 2–5 ans | 5 % |
| Crédit, 5 ans et plus | 10 % |
| Matières premières | 15 % |
| Actions | 15 % |
| Change | 6 % |
| Taux, 0–2 ans | 1 % |
| Taux, 2–5 ans | 2 % |
| Taux, 5 ans et plus | 4 % |
| Autres | 15 % |

Puis la reconnaissance partielle du netting :

$$
IM_{\text{nette}} = 0{,}4 \times IM_{\text{brute}} + 0{,}6 \times NGR \times IM_{\text{brute}},\qquad
NGR = \frac{\text{coût de remplacement net}}{\text{coût de remplacement brut}} = \frac{\max(\sum_k V_k, 0)}{\sum_k \max(V_k, 0)}
$$

Simple, et très conservatrice : c'est ce qui pousse tout le marché vers SIMM.

### 5.3 ISDA SIMM

Modèle de sensibilités : on part des **deltas, vegas et courbures** du
portefeuille, au format **CRIF** (*Common Risk Interchange Format*), on les
multiplie par des poids de risque calibrés par l'ISDA (révisés chaque année)
et on agrège avec des corrélations. Il imite une VaR 99 % 10 jours sans
simulation, et surtout il est **le même pour les deux parties**, ce qui
évite les litiges.

**Structure.**

- 4 classes de produits : *RatesFX*, *Credit*, *Equity*, *Commodity*.
- 6 classes de risque : taux, crédit qualifiant, crédit non qualifiant,
  actions, matières premières, change.
- 4 types de marge par classe de risque : delta, vega, courbure, corrélation
  de base (tranches de crédit).
- Taux : tenors 2s, 1m, 3m, 6m, 1y, 2y, 3y, 5y, 10y, 15y, 20y, 30y, par
  sous-courbe et par devise (bucket = devise).

**Delta, pour une classe de risque.** Pour chaque facteur $k$ du bucket $b$ :

$$
WS_k = RW_k \; s_k \; CR_b
$$

($s_k$ : sensibilité, $RW_k$ : poids de risque, $CR_b$ : facteur de
concentration, $\ge 1$ au-delà d'un seuil de concentration). Dans le bucket :

$$
K_b = \sqrt{\sum_k WS_k^2 + \sum_k \sum_{l \ne k} \rho_{kl}\, f_{kl}\, WS_k WS_l}
$$

($f_{kl} = \min(CR_k, CR_l)/\max(CR_k, CR_l)$). Entre buckets :

$$
\text{DeltaMargin} = \sqrt{\sum_b K_b^2 + \sum_b \sum_{c \ne b} \gamma_{bc}\, g_{bc}\, S_b S_c},\qquad
S_b = \max\Big(\min\Big(\sum_{k\in b} WS_k,\; K_b\Big),\, -K_b\Big)
$$

**Vega** : même agrégation, sur les $VR_k = \sum_i RW^{vega}\,\sigma_{ki}\,\partial V/\partial\sigma_i$
(sensibilité à la vol, multipliée par la vol : un « vega en variance »).

**Courbure** : dérivée des vegas, pour approximer le gamma :
$CVR_k = \sum_i SF(t_{ki})\,\sigma_{ki}\,\partial V/\partial \sigma_i$ avec
$SF(t) = 0{,}5 \min(1, 14\,\text{j} / t)$. On agrège comme le delta mais
avec les **carrés** des corrélations,
$K_b = \sqrt{\sum_k CVR_k^2 + \sum_{k\ne l}\rho_{kl}^2\,CVR_k CVR_l}$ et
$K = \sqrt{\sum_b K_b^2 + \sum_{b\ne c}\gamma_{bc}^2\,S_b S_c}$ (avec $S_b$
borné par $\pm K_b$ comme pour le delta), puis

$$
\text{CurvatureMargin} = \max\Big(\sum_{b,k} CVR_{b,k} + \lambda\,K,\ 0\Big),\quad
\lambda = \big(\Phi^{-1}(0{,}995)^2 - 1\big)(1 + \theta) - \theta,\quad
\theta = \min\Big(\frac{\sum CVR}{\sum |CVR|}, 0\Big)
$$

(La méthodologie applique en plus un facteur d'échelle à la courbure des
taux ; voir la version en vigueur.)

**Agrégation finale.** Par classe de risque
$IM_r = \text{Delta} + \text{Vega} + \text{Courbure} + \text{BaseCorr}$ ; par
classe de produits
$SIMM_p = \sqrt{\sum_r IM_r^2 + \sum_r \sum_{s\ne r}\psi_{rs} IM_r IM_s}$ ;
total $= \sum_p SIMM_p$ (plus d'éventuels add-ons).

**À savoir pour l'implémentation.** La méthodologie et les paramètres sont
publiés par l'ISDA ; l'**utilisation** commerciale demande une licence. Pour
un projet personnel, coder la méthodologie publique est défendable ; les
tests unitaires officiels de l'ISDA sont réservés aux licenciés. Ce
qu'on a de mieux que la plupart des implémentations : **les sensibilités CRIF
sortent de notre AAD** (WP 17) — un portefeuille entier en une passe
adjointe.

### 5.4 IM des CCP

Modèles de VaR / Expected Shortfall historiques (typiquement 99 % à 99,7 %,
horizon 5 jours pour les dérivés compensés, fenêtre avec période de stress),
plus des add-ons de liquidité et de concentration. On ne réplique pas un
modèle de CCP : on l'approche par une VaR historique filtrée sur nos
facteurs, suffisante pour le MVA.

### 5.5 L'IM et l'exposition

Avec IM reçue $IM_C(t)$ :

$$
E(t) = \max\big(V(t) - VM(t - MPoR) - IM_C(t - MPoR),\ 0\big)
$$

Avec une IM à 99 % de la variation sur la MPoR, $\mathbb{P}(E > 0) \le 1\%$
par construction et l'EE est réduite d'un ordre de grandeur : le CVA devient
petit, le MVA grand. **L'IM versée** ne crée pas d'exposition (elle est
ségréguée : la contrepartie ne peut pas la perdre) — sauf en cas de défaut du
dépositaire.

### 5.6 Projeter l'IM future (DIM)

Le MVA et l'exposition sous IM demandent $IM(t)$ **sur chaque chemin et à
chaque date future** : c'est le problème difficile du sujet.

| Méthode | Principe | Coût | Remarque |
|---|---|---|---|
| **Profil déterministe** | IM d'aujourd'hui × décroissance (par exemple $\propto \sqrt{\text{durée restante}}$) | nul | biaisée, mais c'est le point de départ de tout le monde |
| **Régression** (Anfuso, Aziz, Giltinan & Loukopoulos 2017) | la variance conditionnelle de $\Delta V$ sur la MPoR est régressée sur l'état du chemin ; $IM(t) \approx \Phi^{-1}(0{,}99)\,\hat\sigma(t)$, recalée sur la SIMM d'aujourd'hui | faible | la méthode standard ; backtestable |
| **SIMM par chemin** | les sensibilités sont recalculées sur chaque chemin et chaque date, puis passées dans SIMM | énorme ($N_{\text{chemins}}\times N_{\text{dates}}$ calculs de sensibilités) | c'est précisément ce que l'AAD sur GPU rend faisable : **cible ambitieuse, lot X5b** |
| **Réseaux de neurones** | apprendre $\text{état} \mapsto IM$ | entraînement | lien avec le chantier 5 |

Le test de qualité d'une DIM est un **backtest** : on compare la distribution
de l'IM projetée à l'IM réellement calculée en rejouant l'historique (Anfuso
et al. décrivent la procédure).

---

## 6. Probabilités de défaut et courbes de crédit

### 6.1 Réelle ou risque-neutre

- **Probabilités historiques** (agences de notation, défauts observés) :
  pour le capital économique, les limites, les provisions.
- **Probabilités risque-neutres** (implicites des CDS ou des spreads
  obligataires) : pour le CVA, qui est un prix et doit être couvrable. IFRS 13
  impose la juste valeur, donc les spreads de marché.

Les secondes sont bien plus élevées que les premières (prime de risque de
défaut, prime de liquidité) ; Gregory discute longuement ce choix et conclut,
comme la réglementation, pour le risque-neutre.

### 6.2 Le « triangle du crédit »

Avec un spread $s$ et un recouvrement $R$ constants :

$$
\lambda \approx \frac{s}{1 - R} = \frac{s}{LGD},\qquad S(t) = e^{-\lambda t},\qquad PD(t_1,t_2) \approx e^{-s t_1/LGD} - e^{-s t_2/LGD}
$$

Exemple : $s = 150$ pb, $R = 40\%$ → $\lambda = 2{,}5\%$ par an →
$PD(0, 5\text{ ans}) = 1 - e^{-0{,}125} = 11{,}8\%$.

La courbe de hazard constante par morceaux et son bootstrap depuis des
CDS existent déjà (WP 20, `market/credit_curve.hpp`, `credit_bootstrap.hpp`).

### 6.3 Recouvrement

Conventions de marché CDS : **40 %** pour le senior non garanti, **25 %**
pour le subordonné. Le recouvrement *réalisé* après une faillite peut être
très différent de celui du CDS (le règlement par enchère fixe un prix, pas le
recouvrement final). Le CVA est **presque linéaire en LGD** à spread fixé…
sauf que le hazard lui-même dépend de LGD via le triangle : à spread
constant, le CVA est **peu sensible** à $R$ (les deux effets se compensent au
premier ordre). Bon exercice de test (§15).

### 6.4 Courbes proxy

La plupart des contreparties n'ont **pas de CDS liquide**. Le marché (et Bâle
III) accepte une courbe **proxy** construite depuis des contreparties
comparables par notation × secteur × région. Deux méthodes chez Gregory :

- **Mapping** : on affecte à la contrepartie la courbe d'un indice ou d'un
  panier (par notation et secteur).
- **Régression transversale** (Chourdakis et al. 2013) : sur l'univers des
  CDS liquides,
$$
\ln s_{i} = \beta_0 + \beta_{\text{secteur}(i)} + \beta_{\text{région}(i)} + \beta_{\text{notation}(i)} + \beta_{\text{séniorité}(i)} + \varepsilon_i
$$
  puis on lit le spread de la contrepartie sans CDS sur ses caractéristiques.

**Chez nous** : les OAS ICE BofA par notation (AAA → CCC) et par maturité
de `data-ingest` (WP 20) sont exactement une matière de mapping par notation.
C'est la seule source réaliste, et la documentation doit dire que la base
CDS-obligation et la prime de liquidité en font une approximation.

---

## 7. CVA et DVA

### 7.1 CVA unilatéral

Hypothèses : $I$ ne fait pas défaut, défaut de $C$ indépendant de
l'exposition, recouvrement constant. La perte actualisée est
$LGD \cdot D(\tau)\,E(\tau)\,\mathbf{1}_{\tau \le T}$ ; son espérance donne

$$
CVA = -LGD \int_0^T EE^*(t)\, dPD_C(t) \;\approx\; -LGD \sum_{i=1}^{n} EE^*(t_i)\; PD_C(t_{i-1}, t_i)
$$

avec $EE^*$ actualisée. Variante plus précise : évaluer l'EE au milieu de
l'intervalle, ou moyenner $EE^*(t_{i-1})$ et $EE^*(t_i)$.

### 7.2 Le CVA comme spread

Si le hazard est constant et les taux nuls, $dPD \approx \lambda\,dt$ et
$LGD\,\lambda = s$ :

$$
CVA \approx -s \int_0^T EE^*(t)\,dt = -s \times EPE \times T
\quad\Longleftrightarrow\quad
\frac{CVA}{\text{durée risquée}} \approx -\,EPE \times s
$$

**Le CVA exprimé en spread courant est l'EPE (en % du notionnel) multipliée
par le spread de crédit de la contrepartie.** C'est l'approximation de
Gregory à connaître par cœur : un swap 10 ans d'EPE 3 % face à une
contrepartie à 200 pb coûte ≈ 6 pb par an.

### 7.3 CVA d'une option achetée

$EE^*(t) = V(0)$ (valeur actualisée martingale, positive, sans flux
intermédiaire), donc

$$
CVA = -LGD \times V(0) \times PD_C(0, T)
$$

**C'est le premier test du moteur** (lot X1) : il ne dépend ni du modèle ni
de la grille.

### 7.4 CVA bilatéral : CVA + DVA

Si les deux parties peuvent faire défaut, seul le **premier défaut** compte :
après, le contrat est résilié. En supposant l'indépendance des deux défauts :

$$
CVA = -LGD_C \sum_i EE^*(t_i)\; S_I(t_{i-1})\; PD_C(t_{i-1}, t_i)
$$

$$
DVA = -LGD_I \sum_i ENE^*(t_i)\; S_C(t_{i-1})\; PD_I(t_{i-1}, t_i)
$$

($ENE \le 0$ donc $DVA \ge 0$.) Le facteur de survie de l'autre partie est
la correction « premier défaut » ; si on l'omet, on double-compte.

**Symétrie** : le CVA de $I$ sur $C$ est exactement le DVA de $C$ sur $I$ ;
BCVA est donc symétrique entre les deux parties, et c'est ce qui permet de se
mettre d'accord sur un prix. Test (§15).

### 7.5 La controverse du DVA

- Le DVA est un **gain quand notre propre crédit se dégrade** : absurde en
  gestion, et impossible à monétiser (on ne peut pas vendre de la protection
  sur soi-même, et racheter sa dette n'est qu'une approximation).
- IFRS 13 l'impose pourtant en comptabilité (risque de non-performance dans
  la juste valeur).
- Bâle III **retire les gains de DVA du capital CET1** : le régulateur ne
  croit pas à ce bénéfice.
- Le DVA recoupe le **FBA** (§9.3) : les deux monétisent notre propre spread,
  il ne faut pas les additionner sans réfléchir.

### 7.6 Exemple chiffré à reproduire (lot X3)

Swap payeur 5 ans, notionnel 100, spread de la contrepartie 200 pb à
$R = 40\%$, EE issue du moteur ; comparer :

1. la somme discrète du §7.1 ;
2. l'approximation du §7.2 ($-s \times EPE \times T$) ;
3. la même chose avec seuil, MTA et MPoR (§4) ;
4. avec IM (§5).

C'est le tableau « effet des mitigants » du WP 15 §4.

---

## 8. Wrong-way risk

### 8.1 Définition et exemples

**Wrong-way risk (WWR)** : l'exposition augmente quand la probabilité de
défaut de la contrepartie augmente. **Right-way risk** : l'inverse.

- **WWR général** : corrélation macro (une entreprise emprunteuse à taux
  variable qui a swappé en fixe : quand les taux baissent, notre swap
  receveur vaut plus… et son activité ralentit).
- **WWR spécifique** : lien structurel. Exemples canoniques :
  - une **banque d'un pays** qui nous vend de la protection CDS sur ce pays ;
  - un **FX forward** contre une contrepartie d'un pays émergent : la
    devise s'effondre au moment du défaut souverain (saut au défaut) ;
  - un **producteur de matières premières** qui couvre sa production ;
  - du **collatéral** émis par la contrepartie elle-même ou par une entité
    corrélée.
- Bâle III interdit de reconnaître le bénéfice d'une transaction à WWR
  spécifique dans le netting et impose un traitement séparé.

### 8.2 Méthodes (de la plus simple à la plus complète)

| Méthode | Principe | Référence |
|---|---|---|
| **Multiplicateur α** | le 1,4 réglementaire couvre en partie le WWR général | Bâle (IMM) |
| **Exposition conditionnelle** | remplacer $EE(t)$ par $\mathbb{E}[E(t)\mid\tau = t]$ | Gregory |
| **Hazard fonction de l'exposition** | $\lambda(t) = \exp\big(a(t) + b\,V(t)\big)$, $a(t)$ calé pour retrouver la courbe de survie de marché ; $b$ règle le WWR ($b>0$) ou le right-way ($b<0$) | Hull & White 2012 |
| **Hazard stochastique corrélé** | intensité CIR++ corrélée aux facteurs de marché | Brigo & Pallavicini 2007 |
| **Copule / défaut corrélé** | temps de défaut gaussien corrélé au facteur de marché | Gregory (modèle « simple » à corrélation) |
| **Saut au défaut** | la devise ou l'actif saute d'un facteur fixé au moment du défaut | pour les souverains et le FX |

**Choix pour le lot X6 : Hull & White 2012.** C'est le plus simple à brancher
sur un moteur existant (le hazard est une fonction de $V$ déjà simulée), il a
un seul paramètre interprétable, et il inclut l'indépendance ($b = 0$) comme
cas particulier : c'est le test de non-régression.

Mesure à publier : le **ratio CVA avec WWR / CVA indépendant** en fonction de
$b$, par type de portefeuille (payeur / receveur).

---

## 9. FVA et ColVA

### 9.1 D'où vient le coût de financement

Cas type : la banque traite un swap **non collatéralisé** avec une entreprise
et le couvre avec un swap miroir **collatéralisé** avec une autre banque. Quand
le swap client est dans la monnaie pour nous, la couverture est hors de la
monnaie : nous **versons** du collatéral sur la couverture sans en recevoir
du client. Ce collatéral, il faut l'emprunter, à notre spread de financement.
Symétriquement, quand le swap client est hors de la monnaie, nous
**recevons** du collatéral sur la couverture sans en verser : un bénéfice de
financement.

### 9.2 FCA et FBA

$$
FCA = -\sum_i EE^*(t_i)\; S_C(t_i)\,S_I(t_i)\; FS_B(t_{i-1}, t_i)\; \Delta t_i
$$

$$
FBA = -\sum_i ENE^*(t_i)\; S_C(t_i)\,S_I(t_i)\; FS_L(t_{i-1}, t_i)\; \Delta t_i
$$

$FVA = FCA + FBA$ ; $FCA \le 0$ (coût), $FBA \ge 0$ (bénéfice). Avec un
spread symétrique $FS_B = FS_L = FS$ :

$$
FVA = -\sum_i EFV^*(t_i)\; S_C S_I\; FS\; \Delta t_i
$$

L'exposition de financement est $V - C$ où $C$ **n'inclut que le collatéral
réutilisable** : l'IM ségréguée ne finance rien (elle relève du MVA).

### 9.3 Le débat

- **Hull & White (2012)** : le FVA ne devrait pas entrer dans la valorisation
  (Modigliani-Miller ; le coût de financement est compensé par le gain
  « DVA2 » sur notre propre dette). Réponse de l'industrie : les banques ont
  commencé à comptabiliser le FVA (JP Morgan a pris une charge d'environ
  1,5 Md$ en 2014), parce que le coût est réel pour le desk qui se finance
  auprès de sa trésorerie.
- **Recouvrement DVA / FBA** : les deux reposent sur notre spread. Pratiques
  cohérentes : CVA + DVA + FCA (sans FBA) ou CVA + FCA + FBA avec un DVA
  limité à la part de crédit non liée au financement. Gregory présente
  plusieurs combinaisons ; le code expose les quatre composantes séparément
  et laisse le choix de l'agrégat à l'appelant, **documenté**.
- **Cadres théoriques** : Piterbarg (2010, actualisation au taux du
  collatéral, taux de financement pour la partie non collatéralisée) ;
  Burgard & Kjaer (2011, EDP de réplication avec défaut bilatéral et
  financement) ; Brigo, Morini & Pallavicini (2013, espérances récursives,
  non linéaires quand $FS_B \ne FS_L$).

### 9.4 ColVA

- **Taux de rémunération** : si le collatéral cash est rémunéré à un taux
  $r_c$ différent du taux d'actualisation $r$, $ColVA = -\sum_i \mathbb{E}[C^*(t_i)]\,(r_c - r)\,\Delta t_i$.
  C'est ce qui justifie l'**actualisation OIS** des transactions
  collatéralisées (Piterbarg 2010) : la courbe OIS du WP 21 en est l'application.
- **Option du collatéral le moins cher** (*cheapest-to-deliver*) : quand le
  CSA autorise plusieurs devises ou titres, le verseur choisit le moins cher,
  ce qui revient à actualiser au maximum des taux de collatéral (option à
  valoriser).
- Décotes et collatéral non-cash : les titres ont un coût de repo.

---

## 10. MVA

$$
MVA = -\sum_i \mathbb{E}\big[IM_I(t_i)\big]\; S_C(t_i)\,S_I(t_i)\;\big(FS_B(t_{i-1}, t_i) - s_{IM}\big)\;D(t_i)\;\Delta t_i
$$

($IM_I$ : l'IM que **nous versons** ; $s_{IM}$ : spread de rémunération de
l'IM ségréguée, souvent nul ou négatif.) Tout le difficile est
$\mathbb{E}[IM_I(t)]$, donc la DIM du §5.6. Ordre de grandeur (Gregory) :
pour un swap compensé, le MVA peut dépasser le CVA qu'il remplace ; c'est le
coût caché de la compensation obligatoire.

Référence d'implémentation : Green & Kenyon (2015), *MVA by replication and
regression* — exactement notre approche par régression.

---

## 11. Capital réglementaire et KVA

Trois charges de capital portent sur les dérivés, en plus du risque de
marché : le **capital de risque de contrepartie** (perte de défaut), le
**capital CVA** (perte de valeur du CVA), et le **ratio de levier**.

$$
\text{Capital minimal} = 8\% \times RWA,\qquad RWA = 12{,}5 \times K
$$

(plus les coussins : conservation 2,5 %, contracyclique, systémique ; le
CET1 minimal est 4,5 %.)

### 11.1 EAD par SA-CCR (CRE52)

Méthode standard depuis 2017, qui remplace CEM et SM. **À implémenter
entièrement : c'est la formule que toute banque calcule.**

$$
EAD = \alpha\,\big(RC + PFE\big),\qquad \alpha = 1{,}4
$$

**Replacement cost.**

$$
RC_{\text{non margé}} = \max(V - C, 0),\qquad
RC_{\text{margé}} = \max\big(V - C,\; TH + MTA - NICA,\; 0\big)
$$

($NICA$ : collatéral indépendant net, IM et IA ; $TH$, $MTA$ : ceux du CSA.)

**PFE et multiplicateur** (reconnaissance de la sur-collatéralisation et de
la valeur négative) :

$$
PFE = \text{multiplicateur} \times AddOn^{\text{agrégé}},\qquad
\text{multiplicateur} = \min\left(1,\; F + (1 - F)\exp\left(\frac{V - C}{2(1 - F)\,AddOn^{\text{agrégé}}}\right)\right),\quad F = 5\%
$$

$AddOn^{\text{agrégé}} = \sum_{\text{classes}} AddOn_{\text{classe}}$ (aucune
diversification entre classes).

**Notionnel ajusté, delta, facteur de maturité.** Pour chaque transaction :

$$
D_k = \delta_k \times d_k \times MF_k
$$

- **Taux et crédit** : $d = \text{notionnel} \times SD$, durée de supervision
  $SD = \dfrac{e^{-0{,}05 S} - e^{-0{,}05 E}}{0{,}05}$ ($S$, $E$ : début et fin
  en années, $S \ge 0$, $E \ge 10$ jours ouvrés). Change : notionnel dans la
  devise de reporting. Actions, matières premières : prix × quantité.
- **Delta** $\delta$ : $+1$ long, $-1$ court pour le linéaire ; pour une
  option,
$$
\delta = \pm\,\Phi\!\left(\pm\frac{\ln(P/K) + \tfrac{1}{2}\sigma^2 T}{\sigma\sqrt{T}}\right)
$$
  (call acheté $+\Phi(d)$, call vendu $-\Phi(d)$, put acheté $-\Phi(-d)$, put
  vendu $+\Phi(-d)$ ; $\sigma$ = volatilité de supervision ; décalage $\lambda$
  permis quand $P$ ou $K$ est négatif).
- **Facteur de maturité** :
  $MF_{\text{non margé}} = \sqrt{\min(M, 1)/1}$ (en années, $M \ge 10$ jours
  ouvrés) ; $MF_{\text{margé}} = \tfrac{3}{2}\sqrt{MPoR/1\text{ an}}$.

**Add-on par classe.**

- **Taux** : un ensemble de couverture par devise, trois buckets de maturité
  (< 1 an, 1–5 ans, > 5 ans) :
$$
EN = \sqrt{D_1^2 + D_2^2 + D_3^2 + 1{,}4\,D_1 D_2 + 1{,}4\,D_2 D_3 + 0{,}6\,D_1 D_3},\qquad AddOn_{IR} = \sum_{\text{devises}} SF_{IR} \times EN
$$
- **Change** : par paire de devises, $AddOn = SF_{FX}\,\big|\sum_k D_k\big|$.
- **Crédit, actions** : par entité puis agrégation à un facteur
$$
AddOn = \sqrt{\Big(\sum_k \rho_k\, AddOn_k\Big)^2 + \sum_k (1 - \rho_k^2)\, AddOn_k^2},\qquad AddOn_k = SF_k \sum_{j\in k} D_j
$$
- **Matières premières** : même formule par ensemble (énergie, métaux,
  agriculture, autres), $\rho = 40\%$.

**Paramètres de supervision.**

| Classe | Sous-classe | Facteur $SF$ | Corrélation $\rho$ | Vol d'option $\sigma$ |
|---|---|---|---|---|
| Taux | | 0,5 % | — | 50 % |
| Change | | 4,0 % | — | 15 % |
| Crédit, single name | AAA / AA / A / BBB | 0,38 % / 0,38 % / 0,42 % / 0,54 % | 50 % | 100 % |
| | BB / B / CCC | 1,06 % / 1,6 % / 6,0 % | 50 % | 100 % |
| Crédit, indice | IG / SG | 0,38 % / 1,06 % | 80 % | 80 % |
| Actions | single name | 32 % | 50 % | 120 % |
| | indice | 20 % | 80 % | 75 % |
| Matières premières | électricité | 40 % | 40 % | 150 % |
| | autres (pétrole, gaz, métaux, agricole…) | 18 % | 40 % | 70 % |

**MPoR réglementaire** (pour $MF_{\text{margé}}$ et l'IMM) : 10 jours ouvrés
en bilatéral avec appels quotidiens ; 5 jours pour les transactions de
clients compensées ; **20 jours** si le netting set dépasse 5 000
transactions ou contient du collatéral illiquide ou des dérivés difficiles à
remplacer ; doublée après plus de deux litiges de marge sur deux trimestres.
Avec des appels tous les $N$ jours, ajouter $N - 1$.

**Validation** : le texte du Comité (BCBS 279, 2014) contient des exemples
chiffrés complets de netting sets ; les reproduire à l'unité près est le
critère d'acceptation du lot X0.

### 11.2 EAD par modèle interne (IMM, CRE53)

$$
EAD = \alpha \times EEPE,\qquad \alpha = 1{,}4\ (\text{plancher } 1{,}2 \text{ si la banque l'estime elle-même})
$$

**Pourquoi α.** L'EEPE suppose une exposition déterministe ; α corrige la
granularité du portefeuille, l'aléa de l'exposition et une partie du WWR
général. Défini comme le rapport entre le capital économique d'une
simulation complète (exposition et défauts joints) et celui calculé avec
l'EPE comme exposition déterministe. Le 1,4 vient des travaux de Canabarro,
Picoult et Wilde (2003), qui trouvaient ~1,1 sur des portefeuilles réels ;
le régulateur a pris une marge.

L'EEPE doit être calculée aussi sur une **calibration stressée** (trois ans
incluant une période de stress), et on retient la plus grande. C'est le
moteur d'exposition du lot X1 appliqué avec une autre calibration.

### 11.3 Capital de défaut : la formule IRB

$$
K = LGD \left[\Phi\!\left(\frac{\Phi^{-1}(PD) + \sqrt{R}\,\Phi^{-1}(0{,}999)}{\sqrt{1 - R}}\right) - PD\right] \times \frac{1 + (M - 2{,}5)\,b}{1 - 1{,}5\,b}
$$

$$
R = 0{,}12\,\frac{1 - e^{-50\,PD}}{1 - e^{-50}} + 0{,}24\left(1 - \frac{1 - e^{-50\,PD}}{1 - e^{-50}}\right),\qquad
b = \big(0{,}11852 - 0{,}05478 \ln PD\big)^2
$$

C'est la **perte au quantile 99,9 %** d'un modèle de Vasicek à un facteur
(ASRF), moins la perte attendue, avec un ajustement de maturité. La
corrélation d'actifs $R$ est multipliée par **1,25** pour les grandes
institutions financières (actifs ≥ 100 Md$) et les financières non régulées.
$RWA = 12{,}5 \times K \times EAD$. Approche standard (sans IRB) : poids de
risque par notation.

### 11.4 Capital CVA (MAR50)

Cadre révisé de 2020 (entré en vigueur avec la finalisation de Bâle III) ;
deux méthodes, et un raccourci :

- **Raccourci** : si le notionnel des dérivés non compensés est
  **≤ 100 Md€**, la banque peut prendre capital CVA = 100 % du capital de
  risque de contrepartie.

**BA-CVA (méthode de base).** Par contrepartie $c$ :

$$
SCVA_c = \frac{1}{\alpha}\; RW_c \sum_{NS} M_{NS}\; EAD_{NS}\; DF_{NS},\qquad \alpha = 1{,}4
$$

($M$ : maturité effective ; $DF = \frac{1 - e^{-0{,}05 M}}{0{,}05 M}$ pour
une EAD en SA-CCR, $DF = 1$ pour une EAD IMM, déjà actualisée.) Version
réduite (sans couvertures) :

$$
K_{\text{réduit}} = \sqrt{\Big(\rho \sum_c SCVA_c\Big)^2 + (1 - \rho^2)\sum_c SCVA_c^2},\quad \rho = 50\%,\qquad
K_{BA\text{-}CVA} = DS \times K_{\text{réduit}},\quad DS = 0{,}65
$$

Version complète (couvertures CDS reconnues) :
$K = DS\,\big(\beta\,K_{\text{réduit}} + (1-\beta)\,K_{\text{couvert}}\big)$,
$\beta = 0{,}25$, avec
$K_{\text{couvert}} = \sqrt{\big(\rho\sum_c (SCVA_c - SNH_c) - IH\big)^2 + (1-\rho^2)\sum_c (SCVA_c - SNH_c)^2 + \sum_c HMA_c}$
($SNH$ : couvertures single-name, $IH$ : couvertures indice, $HMA$ :
désalignement des couvertures).

Poids de risque $RW_c$ :

| Secteur | IG | HY / non noté |
|---|---|---|
| Souverains, banques centrales, BMD | 0,5 % | 2,0 % |
| Collectivités locales, entités publiques, éducation | 1,0 % | 4,0 % |
| Financières (y compris garanties par l'État) | 5,0 % | 12,0 % |
| Matériaux, énergie, industrie, agriculture, mines | 3,0 % | 7,0 % |
| Consommation, transport, stockage, services administratifs | 3,0 % | 8,5 % |
| Technologie, télécommunications | 2,0 % | 5,5 % |
| Santé, services aux collectivités, activités professionnelles | 1,5 % | 5,0 % |
| Autres | 5,0 % | 12,0 % |

**SA-CVA (méthode standard, sensibilités).** Le calcul du capital de marché
FRTB appliqué au **CVA comptable** : sensibilités delta (et vega) du CVA aux
taux, au change, aux spreads de contrepartie, aux spreads de référence, aux
actions et aux matières premières ; $WS_k = RW_k\,s_k$ ; agrégation par
bucket avec un terme de non-reconnaissance des couvertures ($R = 0{,}01$),
puis entre buckets ; multiplicateur $m_{CVA} = 1{,}25$. Exige un modèle de CVA
approuvé **et ses sensibilités** : c'est l'usage réglementaire de l'AAD du
lot X8. Paramètres complets dans MAR50 (à lire, pas à recopier ici).

### 11.5 Capital sur les CCP

- Expositions de transaction envers une CCP qualifiée (QCCP) : poids de
  risque **2 %**.
- Contribution au default fund :
  $K_{CM,i} = \max\!\Big(K_{CCP}\,\frac{DF_i}{DF_{CCP} + DF_{CM}},\ 8\% \times 2\% \times DF_i\Big)$,
  avec $K_{CCP}$ le capital hypothétique de la CCP ($\sum EAD_i \times 20\% \times 8\%$).

### 11.6 Le ratio de levier

Fonds propres Tier 1 / exposition totale non pondérée ≥ **3 %** (plus un
coussin pour les banques systémiques). Les dérivés y entrent par l'EAD
SA-CCR, avec une reconnaissance limitée du collatéral : c'est souvent la
contrainte liante pour les portefeuilles collatéralisés.

### 11.7 KVA

$$
KVA = -\sum_i \mathbb{E}\big[K(t_i)\big]\; CC\; S_C(t_i)\,S_I(t_i)\; D(t_i)\;\Delta t_i
$$

$K(t)$ : capital total projeté (défaut + CVA + marché, éventuellement levier) ;
$CC$ : coût du capital, typiquement **10 à 15 %** (rendement exigé par les
actionnaires, net du rendement sans risque du capital placé). Projeter
$K(t)$ en SA-CCR demande $V(t)$ par chemin (pour le RC et le multiplicateur)
mais le PFE est surtout déterministe : c'est bien moins lourd que la DIM.
Référence : Green, Kenyon & Dennis (2014).

---

## 12. Gérer les xVA : le desk, la couverture, les sensibilités

- **Le desk xVA** centralise le risque de contrepartie de la banque : chaque
  desk de trading lui « achète » la protection au prix du CVA incrémental, le
  desk xVA gère le portefeuille de CVA agrégé (netting entre desks).
- **Couvertures** : les sensibilités du CVA au spread de crédit de la
  contrepartie (CDS single-name si possible, indices sinon) et **aux facteurs
  de marché** (le CVA d'un swap payeur est une option sur le taux : on couvre
  son delta taux). Le **cross-gamma** crédit × marché n'est pas couvrable
  proprement.
- **Nombre de sensibilités** : un portefeuille de banque a des milliers de
  contreparties × des centaines de piliers de courbe. Le bump-and-reprice
  est **physiquement impossible** dans la nuit : c'est le cas d'usage
  industriel qui a fait adopter l'AAD (Capriotti & Giles ; Savine). Chez nous,
  c'est le lot X8 et le point de convergence de tout le dépôt.
- **P&L explain** : la variation quotidienne du CVA doit s'expliquer par ses
  sensibilités (thêta, deltas, spreads, nouvelles transactions). Un xVA qui
  ne s'explique pas ne se gère pas.
- **Le lissage** : $\max(V, 0)$ est non dérivable en $V = 0$, comme une
  digitale ; la dérivée par chemin reste juste en espérance (l'ensemble
  $\{V = 0\}$ est de mesure nulle) mais bruitée. La logique floue du WP 16
  (`fuzzy_evaluator`) et les proxies de régression **lisses** règlent la
  question dans le cadre de Savine & Andreasen.

---

## 13. Architecture dans quant-modeling

### 13.1 Principe : suivre Savine & Andreasen

Le second livre de Savine (avec Andreasen), *Scripting for Derivatives and
xVA*, traite le xVA **comme un produit scripté** : le netting set est agrégé
en un seul script, le CVA est lui-même un « payoff » sur ce script, les
valeurs futures sont estimées par **régression** (les mêmes proxies que le
LSMC), et l'AAD appliquée à l'ensemble donne d'un coup toutes les
sensibilités du CVA. Notre scripting (WP 16) et notre LSMC (`exercise()`)
suivent déjà ce livre ; le chantier xVA le suit aussi. **Les écarts sont
consignés en ADR** (règle du projet pour les chantiers qui suivent Savine),
en particulier les deux prévus :

- **ADR-X1 : repricing fermé quand il existe.** Les swaps et swaptions
  européennes sous Hull-White se repricent **exactement** à chaque date
  (obligations zéro-coupon en forme fermée, `HullWhiteCurveModel::zcb`,
  Jamshidian pour les swaptions). La régression est réservée à ce qui n'a
  pas de forme fermée (bermudans, callables, scripts quelconques). Raison :
  une exposition exacte est l'oracle de l'exposition par régression, et le
  cas des swaps est l'essentiel du portefeuille.
- **ADR-X2 : collatéral et IM hors du script.** Le collatéral (récursion
  sur la MTA, MPoR) et la DIM sont des post-traitements C++ sur la matrice
  $V(\text{chemin}, \text{date})$, pas des instructions du langage : ils ne
  dépendent pas du produit et le langage n'a pas à les exprimer.

### 13.2 Découpage (respecter les couches du dépôt)

| Couche | Ce qui arrive | Où |
|---|---|---|
| `market/` | courbes proxy par notation (sur les OAS ICE BofA), spreads de financement, paramètres de CSA sous forme de données | `market/credit_proxy.hpp`, `market/csa.hpp` |
| `models/` | modèle joint des facteurs : Hull-White par devise + FX + actions + hazard (WWR), corrélations ; calibration risque-neutre ou historique | `models/xva/joint_factor_model.hpp` |
| `engines/xva/` | le moteur d'exposition : grille, simulation, repricing (fermé ou par régression), matrice $V$ ; CPU puis GPU | `engines/xva/exposure_engine.hpp` |
| `risk/` (nouveau) | ce qui ne dépend ni du produit ni du modèle : netting, collatéral, métriques, intégrales xVA, allocation d'Euler, SA-CCR, BA-CVA, IRB, SIMM, grille d'IM | `risk/exposure_metrics.hpp`, `risk/collateral.hpp`, `risk/xva.hpp`, `risk/regulatory/sa_ccr.hpp`, … |
| `pricers/` | adaptateur « portefeuille + CSA + contrepartie → rapport xVA » | `pricers/adapters/xva.hpp` |
| bindings / API | `POST /api/xva/netting-set` ; événement d'audit `xva.valuation` par `api/app/audit/` (rejouable comme `pricing.valuation`) | `api/app/routers/xva.py` |
| front | la page `/xva` du WP 15 §4 | `web/src/features/xva/` |

Règles du dépôt qui s'appliquent : aucun engine ne branche sur le type de
produit (le repricing passe par une interface « valeur future » fournie par
chaque instrument, ou par le script) ; aucun instrument ne lit de marché ;
`unique_ptr` et références non propriétaires plutôt que `shared_ptr`.

### 13.3 Le moteur d'exposition

- **Sortie centrale** : une matrice $V_{k}(\omega, t_i)$ par transaction (ou
  directement par netting set quand l'allocation n'est pas demandée),
  $N_{\text{chemins}} \times N_{\text{dates}}$, plus les flux payés entre
  deux dates (pour le modèle de MPoR avancé). À $10^4$ chemins × 400 dates en
  double, 32 Mo par netting set : tient en mémoire GPU sans découpage.
- **Mesure** : simulation de Hull-White dans l'état $x(t)$ sous la mesure
  $T^*$-forward (`HullWhiteCurveModel::transition`), numéraire $P(t, T^*)$ ;
  $EE^*(t) = P(0,T^*)\,\mathbb{E}^{T^*}\!\big[E(t)/P(t,T^*)\big]$.
- **Reproductibilité** : Philox par indice de chemin et réduction dans un
  ordre fixe (comme WP 19) — le même rapport xVA sur 1 thread, 56 threads, 1
  ou 2 V100. Condition du replay (`POST /api/admin/replay/{id}`).
- **Chemins communs** : toutes les transactions, tous les netting sets, et
  les calculs avec et sans collatéral utilisent **les mêmes chemins** : les
  différences (incrémental, effet du CSA) sont alors peu bruitées.

### 13.4 AMC : repricing par régression

Pour une transaction sans forme fermée, $V(t_i) = \mathbb{E}[\text{flux futurs actualisés}\mid\mathcal F_{t_i}]$
est estimée par régression des flux futurs réalisés sur des fonctions de
base de l'état (Longstaff-Schwartz, Carriere) :

1. passe pilote sur des chemins indépendants → coefficients de régression par
   date ;
2. passe principale : $\hat V(t_i) = \beta_i^\top \psi(\text{état}_i)$.

Chaque produit avec exercice (bermudan physique) doit **suivre son état
d'exercice** chemin par chemin : après exercice, la swaption devient un
swap et son exposition continue. Le LSMC existant (`engines/mc/lsm.hpp`)
fournit la passe pilote et les bases polynomiales ; il faut l'étendre des
seules dates d'exercice à **toute la grille d'exposition**.

Pièges connus (à tester) : biais de régression dans les queues (PFE à 99 %),
choix des régresseurs (le taux swap résiduel plutôt que $x$ seul), et
régression sur les chemins de la passe principale (biais haut) contre
passe pilote séparée (notre choix, comme pour le LSMC).

### 13.5 AAD : les sensibilités du CVA

Le CVA est une espérance d'une fonction lisse par morceaux des facteurs :
son adjoint par chemin se calcule avec la tape (WP 17) en **une passe
arrière** pour toutes les sensibilités (piliers de courbe OIS et de
projection, $\sigma$ et $a$ de Hull-White, piliers de hazard de chaque
contrepartie, spreads de financement). Points de conception :

- **Différencier à travers la régression** : on fige les coefficients de la
  passe pilote (ils ne dépendent des paramètres qu'au second ordre dans
  l'espérance — l'argument est dans Savine & Andreasen et doit être redonné
  dans l'ADR).
- **Différencier à travers la calibration** Hull-White (pour des
  sensibilités aux vols de swaptions de marché) : théorème des fonctions
  implicites, comme le superbucket du lot 17h.
- **Mémoire** : check-pointing par chemin (lot 17b), pas une tape du
  portefeuille entier.
- **GPU** : adjoint par chemin sans tape, comme la vol locale du WP 19, une
  fois la version CPU validée.
- **Critère** : sensibilités AAD = différences finies à l'erreur MC près
  sur nombres aléatoires communs, et coût AAD / coût d'un pricing mesuré
  (le tableau du chantier 2 appliqué au CVA).

### 13.6 GPU

Le repricing de swaps sous Hull-White en forme fermée est un kernel
parfaitement parallèle (chemin × date × flux) ; la récursion du collatéral
est séquentielle en temps mais indépendante par chemin (un thread par
chemin). Le benchmark suit la règle du dépôt : **temps pour atteindre une
erreur donnée** sur le CVA, CPU 1 cœur / 56 cœurs / 1 V100 / 2 V100, et
égalité bit à bit.

### 13.7 Ce que l'outil doit être (décisions du mainteneur, 2026-10-01)

Le mainteneur doute que l'outil soit utilisable « en vrai » (ni portefeuilles
ni CSA réels) et en fixe donc l'objet :

1. **Un outil pédagogique auto-explicatif.** Il sert à apprendre et à
   expliquer (entretien, démonstration), mais la page doit se suffire : chaque
   étape porte son explication, sa formule (celle de Gregory) et sa source, à
   côté du chiffre calculé. Texte du site en anglais, sources savantes, tout
   chiffre défendable avec son erreur Monte-Carlo (règles du dépôt).
2. **Plusieurs portefeuilles, du simple au complexe**, avec les seuls
   produits existants :

   | # | Portefeuille | Ce qu'il enseigne |
   |---|---|---|
   | P1 | un swap payeur 10 ans, sans CSA | profil en cloche, EE / PFE / EPE, CVA ≈ spread × EPE |
   | P2 | une swaption achetée, puis vendue | $EE^* = V_0$, CVA d'une option, exposition nulle du vendeur |
   | P3 | un book directionnel (plusieurs payeurs) | le netting ne rapporte presque rien |
   | P4 | un book équilibré (payeurs et receveurs) | facteur de netting, allocation d'Euler, CVA incrémental |
   | P5 | P4 sous CSA, puis avec marge initiale | seuil, MTA, MPoR, pics de coupons, le CVA devient du MVA |
   | P6 | swaps + swaptions + bermudans, wrong-way | AMC (X4), WWR (X6), capital et KVA |

3. **Un pricing aussi proche du réel que possible.** Ce que cela demande,
   entrée par entrée :

   | Entrée | État | Écart au réel |
   |---|---|---|
   | Courbe d'actualisation | réelle : courbe de swaps SOFR 1–30 ans tirée des swaps traités (`rates.dtcc_swap_rates`), en plus de FRED | médiane des transactions du jour, pas des cotations ; rien sous 1 an |
   | Vol de Hull-White | **réelle depuis le 2026-10-01** : transactions de swaptions publiées par la DTCC (`data-ingest`, `rates.dtcc_swaptions`), inversées en vols normales ATM (`api/app/swaption_market.py`) | médianes de quelques transactions par point, pas une surface de dealer ; échéances ≤ 2 ans ; USD SOFR seulement |
   | Crédit des contreparties | réel mais proxy (OAS ICE BofA par notation) | pas de CDS single-name |
   | Termes des CSA | réels : ceux des règles de marge (seuil nul, MTA 500 k€, 10 jours) | — |
   | Transactions | synthétiques, calibrées sur des tailles réalistes | pas de portefeuille de banque public |
   | Chiffres de contrôle | à ingérer : rapports Pilier 3 (EAD, capital CVA) | ordres de grandeur seulement |

   Le verrou était la volatilité ; il est levé (issue #137). Les
   **transactions de swaptions** publiées par le référentiel central de la
   DTCC (*public price dissemination*, un fichier par jour) sont ingérées par
   `~/data-ingest` (sources `dtcc-swaptions` et `dtcc-swap-rates`), et
   `GET /api/rates/market` en tire un jeu de cotations USD SOFR : courbe de
   swaps du jour et vols normales ATM, avec le nombre de transactions derrière
   chaque point. Sur les données du 30 septembre 2026, Hull-White se cale à
   a = 4,6 %, σ = 122 pb, avec 5 pb d'écart quadratique. Les fichiers ne
   restent téléchargeables qu'environ deux ans : l'historique part de fin
   septembre 2024. Reste ouvert : la convention de prime des échéances
   longues (comptant ou à l'échéance), qui limite la grille à 2 ans
   d'échéance. L'estimation historique sur les taux reste nécessaire pour la
   mesure historique (lot X1b).

---

## 14. Les lots

Chaque lot = une branche, une issue, des tests de propriété (§15). L'ordre
suit les dépendances ; X0 ne demande aucune simulation.

| Lot | Contenu | Critère d'acceptation vérifiable |
|---|---|---|
| **X0 — Formules** | `risk/` : métriques normales (§3.2), facteur de netting, SA-CCR complet, grille d'IM + NGR, IRB, BA-CVA, intégrales CVA / DVA / FVA / MVA / KVA sur des profils donnés | exemples chiffrés du BCBS 279 reproduits ; formules normales vs intégration numérique à 1e-12 ; CVA = $-LGD\,V_0\,PD$ sur un profil constant |
| **X1 — Moteur d'exposition, taux USD** | grille (dates de flux, piliers), Hull-White $T^*$-forward, swaps et swaptions européennes en forme fermée, netting, EE / ENE / EFV / PFE / EPE / EEE / EEPE, allocation d'Euler, CPU multi-thread | $EE+ENE = EFV$ ; $EFV^*(t)$ = valeur analytique à 3 σ MC ; option achetée : $EE^* = V_0$ ; profil de swap en cloche, pic vers $T/3$ ; mêmes bits sur 1 et 56 threads ; somme des contributions d'Euler = total |
| **X2 — Collatéral** | CSA (seuil, MTA, arrondi, IA), MPoR classique puis avancée (flux pendant la MPoR), grille d'IM ; IM déterministe | EE collatéralisée ≈ $0{,}4\sigma_V\sqrt{MPoR}$ sur un produit gaussien ; monotonie en seuil et en MTA ; seuil infini = non collatéralisé ; pics de flux visibles |
| **X3 — CVA / DVA** | courbes proxy ICE BofA, CVA unilatéral et bilatéral (premier défaut), CVA incrémental et marginal, API + événement d'audit | symétrie CVA$_I$ = DVA$_C$ ; hazard nul → CVA nul ; approximation $-s\,EPE\,T$ à quelques % ; rejouable par le replay |
| **X4 — AMC** (fait : §14.6 et §14.8) | exposition par régression sur toute la grille, bermudans avec état d'exercice, scripts quelconques | sur swaps et swaptions européennes, régression = forme fermée à l'erreur MC près (EE et PFE 99 %) ; bermudan : exposition cohérente avec le prix du réseau (WP 21) à $t=0$ |
| **X5 — FVA, ColVA, MVA, KVA** (fait : §14.7) | FCA / FBA (symétrique et asymétrique), ColVA, DIM par régression (Anfuso et al.) calée sur une SIMM d'aujourd'hui, MVA ; capital projeté SA-CCR + BA-CVA → KVA | FVA symétrique = $-FS\sum EFV^*\Delta t$ ; DIM backtestée sur un historique simulé ; KVA d'un netting set vide = 0 |
| **X5b — SIMM par chemin** (fait : §14.10 ; sensibilités en forme fermée, CPU) | sensibilités CRIF par AAD sur chemins et dates, SIMM par chemin, sur GPU | SIMM à $t = 0$ = SIMM calculée directement ; DIM par chemin vs régression |
| **X6 — Wrong-way risk** (fait : §14.9 ; sans le saut FX au défaut) | hazard de Hull & White 2012, $\lambda = e^{a(t) + bV}$ ; saut FX au défaut (si le multi-devise existe) | $b = 0$ redonne X3 au bit près ; survie de marché retrouvée pour tout $b$ ; CVA croissant en $b$ sur un portefeuille payeur |
| **X7 — GPU** (fait : §14.11 ; transactions en forme fermée, bit à bit entre cartes, à 10⁻¹³ près contre le CPU) | moteur d'exposition et collatéral sur les deux V100 | égalité bit à bit CPU / 1 GPU / 2 GPU ; tableau temps pour une erreur donnée |
| **X8 — CVA par AAD** *(le capstone)* | sensibilités du CVA à tous les piliers (courbes, vols HW, hazards, financement), à travers la régression et la calibration ; sur GPU ensuite ; SA-CVA à partir de ces sensibilités | AAD = différences finies (CRN) à l'erreur MC ; coût AAD / pricing publié ; SA-CVA reproduit à la main sur un cas jouet |
| **X9 — Page `/xva`** | WP 15 §4 : profils EE / PFE avec enveloppe, surface chemin × temps × exposition en 3D, décomposition par ajustement et par transaction, effet des mitigants superposé, sensibilités | chaque chiffre affiché avec son erreur MC ; convention de signe cash-flow |
| **X10 — Multi-devise** *(optionnel)* | FX lognormal couplé à deux Hull-White, cross-currency swaps et FX forwards existants (`instruments/fx/forward.hpp`) ; rejoint l'issue #86 (quanto) | parité forward FX retrouvée ; profil de CCS dominé par le notionnel final |

### 14.1 Lot X0 : ce qui est fait, et les choix d'implémentation

Le lot X0 est livré dans `include/quantModeling/risk/` (`exposure_metrics.hpp`,
`xva.hpp`, `regulatory/sa_ccr.hpp`, `irb.hpp`, `ba_cva.hpp`, `im_schedule.hpp`)
avec ses tests (`tests/testExposureMetrics.cpp`, `testXvaIntegrals.cpp`,
`testSaCcr.cpp`, `testRegulatoryCapital.cpp`). Les chiffres réglementaires ont
été revérifiés contre les textes primaires ; le paragraphe est cité dans le
code. Choix à connaître pour les lots suivants :

- **Grille** : un profil est donné aux dates $t_1 < \dots < t_n$, $t_0 = 0$
  implicite, et toute intégrale en temps est la somme « à droite »
  $\sum_i f(t_i)\,(t_i - t_{i-1})$ : c'est la formule discrète de Gregory et la
  définition bâloise de l'EEPE (CRE53.13). Erreur d'ordre 1 en pas de temps
  (mesurée dans les tests) ; la variante au point milieu du §7.1 n'est pas
  codée.
- **Profils actualisés** : `risk/xva.hpp` reçoit $EE^*$, $ENE^*$,
  $\mathbb{E}[D\,IM]$, $\mathbb{E}[D\,K]$ déjà actualisés (c'est le moteur qui
  prend l'espérance de la quantité actualisée) ; aucune courbe de taux n'y
  entre.
- **Spreads de financement et coût du capital constants** : un scalaire par
  appel. Une structure par terme attendra le lot X5.
- **Agrégat non imposé** : CVA, DVA, FCA, FBA, MVA, KVA sont des fonctions
  séparées ; le recouvrement DVA / FBA (§9.3) reste le choix de l'appelant.
- **SA-CCR** : non couverts, faute de produit dans la librairie — tranches de
  CDO (CRE52.41), options digitales (CRE52.42), plusieurs accords de marge sur
  un netting set (CRE52.74–76). L'EAD margée est plafonnée à l'EAD non margée
  (CRE52.2).
- **IRB** : la formule de CRE31.5 est publiée en image ; les coefficients sont
  validés contre les poids de risque illustratifs de CRE99 (tableau 1). Le
  plancher réglementaire de PD est laissé à l'appelant.
- **Grille d'IM** : $NGR = 0/0$ (aucune transaction de valeur positive) est
  pris égal à 1, le choix prudent ; les bornes 2 ans et 5 ans tombent dans la
  tranche inférieure. Le texte ne tranche ni l'un ni l'autre.

### 14.2 Lot X1 : ce qui est fait, et les choix d'implémentation

Livré : `engines/xva/future_value.hpp` (l'interface « valeur future »),
`engines/xva/hull_white_future_value.hpp` (swap et swaption européenne, exacts
sous Hull-White), `engines/xva/exposure_engine.hpp` (grille + simulation),
`risk/exposure_paths.hpp` (le cube et les métriques par netting set), tests
dans `tests/testExposureEngine.cpp`.

- **Aucun branchement sur le produit** : le moteur ne voit que `FutureValue`
  (`value(i, état)`, `cashflow(i, état)`, `event_times()`), construit par un
  visiteur d'instrument, comme les engines de pricing. Une transaction vendue
  est une quantité négative.
- **Valeur après paiement** : $V(t_i)$ est la valeur des flux strictement
  postérieurs à $t_i$ ; le flux de la date est rendu à part par `cashflow`.
  Le coupon flottant en cours est repricé avec son fixing lu sur le chemin
  (l'exactitude en dépend : la propriété « valeur + flux payés = martingale »
  est testée).
- **Mesure** : état $x$ de Hull-White simulé par ses transitions gaussiennes
  exactes sous la mesure $T^*$-forward, $T^*$ = dernière date de la grille ;
  le cube porte le poids d'actualisation $w = P(0,T^*)/P(t,T^*)$, de moyenne
  $P(0,t)$ (testé).
- **Deux familles de profils** : actualisés ($EE^*$, pour les xVA) et non
  actualisés, définis comme $EE(t) = EE^*(t)/P(0,t)$, c'est-à-dire
  l'espérance sous la mesure $t$-forward. La PFE est le quantile sous cette
  même mesure (chemins pondérés par $w$). EPE et EEPE sont calculées sur le
  profil non actualisé.
- **Le cube garde les transactions séparées** (`ExposurePaths`), le netting
  est un post-traitement (`exposure_statistics(paths, transactions)`) : un
  seul jeu de chemins répond pour n'importe quel netting set, pour
  l'incrémental et pour l'allocation d'Euler. Mémoire : transactions × chemins
  × dates × 8 octets, avec une limite explicite (4 Gio par défaut) au-delà de
  laquelle la simulation refuse de partir.
- **Reproductibilité** : Philox par (graine, chemin, date) ; chaque chemin
  écrit sa ligne ; les réductions se font en ordre de chemin sur un thread.
  Mêmes bits sur 1 ou 8 threads (testé).
- **Grille** : hebdomadaire le premier mois, mensuelle jusqu'à 2 ans,
  trimestrielle ensuite, plus toutes les dates d'événement.
- **Swaption** : à règlement physique ; après l'expiry c'est le swap sur les
  chemins exercés (donc une exposition négative possible). Le prix fermé a
  été généralisé à toute date future (`HullWhiteExerciseRegion` : la région
  d'exercice est calculée une fois).
- **Non couverts** : swap déjà commencé (il faudrait son fixing historique),
  bermudans et scripts (lot X4), réduction de variance, EE non actualisée sous
  la mesure risque-neutre proprement dite (elle demanderait de simuler le
  compte bancaire) et calibration historique pour la PFE (§3.1).
- **Constat utile** : la valeur d'un swap est presque gaussienne (EE à 2 %
  de la formule normale) mais la PFE à 95 % est quelques pourcents sous le
  quantile normal — la convexité des obligations borne la valeur d'un swap
  payeur.

### 14.3 Lot X2 : ce qui est fait, et les choix d'implémentation

Livré : `market/csa.hpp` (les termes du CSA, en données), `risk/collateral.hpp`
(la mécanique), tests dans `tests/testCollateral.cpp`.

- **Post-traitement du cube** (ADR-X2) : `collateralise(cube, csa)` rend un
  cube à une seule « transaction », la valeur collatéralisée
  $V(t) - C(t - MPoR)$, que `exposure_statistics` traite comme les autres.
  Avec et sans CSA se comparent donc sur les mêmes chemins ; des seuils
  infinis redonnent le cube d'origine au bit près (testé).
- **Dates décalées sur la grille** : `ExposureGridSettings::margin_period_of_risk`
  ajoute $t - MPoR$ pour chaque date $t$. Les dates de reporting sont celles
  dont la date décalée existe ; avant $MPoR$, le collatéral est celui appelé
  sur la valeur d'aujourd'hui.
- **Récursion de la MTA** : le solde part de l'appel d'aujourd'hui et n'est
  mis à jour qu'aux dates décalées, dans l'ordre du temps (les appels
  quotidiens intermédiaires ne sont pas simulés). Arrondi au multiple le plus
  proche. Le montant indépendant s'ajoute au-dessus de la VM.
- **Flux pendant la MPoR** (Andersen, Pykhtin & Sokol) : trois traitements —
  `Paid` (Classical−, défaut : pics aux dates où la banque paie),
  `Withheld` (Classical+ : aucun flux échangé, pas de pic), `OnlyBankPays`
  (le cas défavorable). Le modèle complet, avec une date d'arrêt par partie,
  n'est pas codé.
- **Marge initiale** : profils déterministes, reçue et versée ; la reçue ne
  réduit que l'exposition de la banque, la versée (ségréguée) que celle de la
  contrepartie. L'EFV du cube avec IM n'est plus un besoin de financement :
  le FVA se calcule sur le cube sans IM.
- **Allocation d'Euler** non reportée sous CSA (l'homogénéité est perdue).
- **Mesuré** : l'exposition résiduelle à seuil nul suit la formule normale du
  lot X0 à 3 % près et se multiplie par $\sqrt 2$ quand la MPoR double ; sur
  un swap payeur 10 ans, le pic d'une date de coupon vaut le coupon net payé.

### 14.4 Lot X1b : mesure historique

Livré : `market/historical_rate_dynamics.hpp` (la dynamique et son
estimation), `ExposureSimulationSettings::historical` dans le moteur, le
drapeau `ExposureMeasure` sur le cube ; tests dans
`tests/testHistoricalMeasure.cpp`.

- **Simuler sous P, pricer sous Q.** Le taux court suit
  $dr = a(\theta - r)\,dt + \sigma\,dW$ sous la mesure historique, depuis le
  taux court d'aujourd'hui ; l'état du modèle est $x(t) = r(t) - f(0,t)$ et
  chaque transaction est toujours valorisée, sur chaque chemin, par le
  Hull-White risque-neutre. Seule la probabilité des scénarios change.
- **Rien n'est actualisé** : poids et facteurs d'actualisation valent 1, les
  profils sont des espérances et des quantiles simples. `profile()` refuse de
  donner un cube historique aux intégrales xVA (le CVA est un prix).
- **Estimation** par maximum de vraisemblance exact (le processus
  échantillonné est un AR(1)), avec les erreurs standard.
- **Ce que disent les vraies données** (bons du Trésor à 3 mois, FRED) :

  | Fenêtre | $a$ | $\theta$ | $\sigma$ |
  |---|---|---|---|
  | depuis 1990 | 0,11 ± 0,05 | 1,95 % ± 1,06 | 73 pb ± 1 |
  | depuis 2010 | pas de retour à la moyenne mesurable | — | 40 pb |
  | depuis oct. 2016 | 0,06 ± 0,08 | 9,4 % ± 2,7 | 49 pb ± 1 |
  | depuis oct. 2021 | 0,63 ± 0,16 | 5,3 % ± 0,4 | 58 pb ± 1 |

  **La volatilité s'estime, le niveau de long terme et la vitesse de rappel
  non** : ils changent du tout au tout selon la fenêtre. Or c'est $\theta$
  qui fait le résultat : sur un swap payeur 10 ans au pair (courbe SOFR du
  30 septembre 2026), le pic de PFE à 95 % passe de 18,5 (risque-neutre,
  vol implicite 122 pb) à 5,3 (historique depuis 1990 : les taux sont
  supposés redescendre vers 2 %). À l'affichage, $\theta$ et $a$ doivent être
  des choix visibles de l'utilisateur, pas un chiffre présenté comme mesuré.
- **Garde-fou** : une courbe dont le facteur d'actualisation saute (tenue
  plate avant son premier pilier) n'a pas de taux court de départ ; le moteur
  le refuse au lieu de simuler depuis des milliers de pourcents.

### 14.5 Lot X3 : CVA / DVA de bout en bout

Livré : `risk/xva_report.hpp` (le rapport d'un netting set), les bindings
`qm.xva_netting_set` et `qm.estimate_historical_rate_dynamics`,
`api/app/xva.py` avec `GET /api/xva/portfolios` et
`POST /api/xva/netting-set`, l'événement d'audit `xva.valuation` ; tests dans
`tests/testXvaReport.cpp` et `api/tests/test_xva.py`.

- **Rapport** : exposition (après collatéral s'il y a un CSA), CVA et DVA au
  premier défaut **avec leur erreur Monte-Carlo** (dispersion de la perte
  chemin par chemin, corrélations entre dates comprises), CVA unilatéral, FCA
  / FBA, la règle de pouce $-s \times EPE \times T$ en regard, et par
  transaction : CVA seule, **incrémental** (avec et sans la transaction, sous
  le même CSA) et **marginal** (Euler ; indéfini sous CSA). Il est rangé dans
  `risk/` et non dans `pricers/adapters/` comme le prévoyait le §13.2 : il ne
  dépend d'aucun produit.
- **Entrées, toutes lues en base, sans repli** : courbe SOFR et Hull-White
  calé sur les transactions DTCC (§13.7) ; crédit de la contrepartie et de la
  banque par notation (OAS ICE BofA, hazard constant qui reprice un CDS 5 ans,
  comme la page crédit) ; dynamique historique estimée sur le taux à 3 mois
  depuis 1990, dont chaque paramètre peut être remplacé par l'utilisateur.
- **Portefeuilles** P1 à P4 du §13.7 (`single_swap`, `bought_swaption`,
  `sold_swaption`, `directional`, `balanced`), transactions au pair sur la
  courbe du jour, notionnel de référence 10 M$ ; P5 est n'importe lequel avec
  un CSA ; P6 attend les lots X4 et X6.
- **Sur les données du 30 septembre 2026** (contrepartie BBB à 102 pb, banque
  A, 20 000 chemins) : swap payeur 10 ans, CVA −24 100 $ ± 200 (règle de
  pouce −27 200), DVA +15 600 ; book équilibré, CVA −15 700 pour −67 500 de
  CVA des transactions prises une à une ; le même sous CSA à seuil nul et MTA de 250 k$,
  CVA −3 900.
- **Constat** : avec un fixe annuel et un flottant trimestriel, un swap
  receveur porte une exposition systématique entre deux dates de coupon ; les
  portefeuilles de l'API utilisent des jambes annuelles des deux côtés
  (convention SOFR) pour ne pas mêler cet effet aux leçons.
- **Reste** : le rejeu par `POST /api/admin/replay/{id}` n'est pas branché
  (l'événement porte ce qu'il faut : requête, graine, modèle, empreintes des
  entrées) ; la page `/xva` (lot X9).

### 14.6 Lot X4a : exposition par régression (AMC), bermudans

Livré : `engines/xva/regression_future_value.hpp` (la régression et le
« jumeau par régression » d'une transaction),
`src/engines/xva/hull_white_bermudan_value.cpp` (le bermudan), la passe pilote
dans `HullWhiteExposureEngine::simulate`, le type `bermudan` dans
`qm.xva_netting_set`, deux portefeuilles dans l'API (`bermudan`,
`cancellable`) ; tests dans `tests/testExposureRegression.cpp` et
`api/tests/test_xva.py`. Les scripts quelconques sont le lot X4b (§14.8).

- **Ce qu'une transaction dit d'elle-même.** `FutureValue` gagne trois
  crochets : `needs_pilot()` / `fit(pilot)`, et la description de ce dont la
  valeur dépend — un **régime** discret (option, swap après exercice, rien) et
  des **régresseurs** continus (l'état du jour, plus ce qui a été fixé et
  reste à payer : le coupon flottant en cours). La régression se fait régime
  par régime : un polynôme ne suit pas « exercé ou non ». Le moteur ne sait
  toujours pas ce qu'est un swap.
- **Passe pilote séparée** (§13.4) : chemins indépendants (complément de la
  graine), toujours sous la mesure de pricing, simulés et ajustés sur un seul
  thread — les mêmes bits quel que soit le nombre de threads (testé). Par
  défaut **quatre fois** les chemins principaux, entre 20 000 et 200 000.
- **La cible** : $V(t_i) = \mathbb{E}[\sum_{j>i} \text{flux}_j\,N(t_i)/N(t_j) \mid \mathcal F_i]$,
  donc on régresse la somme des flux futurs réalisés, déflatés. Seuls les
  rapports de déflateurs d'un même chemin comptent.
- **Régression par tranches.** Les chemins pilotes sont triés selon l'état
  et coupés en tranches de même effectif (4 par défaut), un polynôme de degré
  2 par tranche. Un polynôme global de degré 4 ne suit pas le coude d'une
  option près de son échéance : sur la swaption européenne il plafonne à 2,5 %
  d'écart sur l'EE et 3,2 % sur la PFE à 99 %, quel que soit le nombre de
  chemins.
- **C'est le bruit qui limite, pas la base.** On régresse dix ans de flux
  sur un nombre : l'écart à la forme fermée décroît comme $1/\sqrt{N_{pilote}}$.

  | Chemins pilotes (degré 2, 4 tranches) | EE swap | PFE 99 % swap | EE swaption | PFE 99 % swaption |
  |---|---|---|---|---|
  | 20 000 | 2,1 % | 3,6 % | 2,1 % | 3,3 % |
  | 80 000 (défaut pour 20 000 chemins) | 0,9 % | 1,8 % | 0,7 % | 2,0 % |
  | 200 000 | 0,5 % | 1,0 % | 0,5 % | 1,3 % |

  (écart maximal sur le profil, en % du pic ; l'erreur Monte-Carlo de l'EE
  exacte elle-même est d'environ 1 % du pic à 20 000 chemins.) D'où le défaut
  à quatre fois les chemins principaux : un chemin pilote ne fait qu'évaluer
  des flux. Une cible moins bruitée (régresser la valeur estimée à la date
  suivante plutôt que tous les flux futurs) est l'amélioration à essayer.
- **Bermudan** : état d'exercice chemin par chemin. Tant qu'il n'a pas
  exercé, valeur de continuation par régression sur l'état à **chaque date
  de la grille** ; à chaque date d'exercice le détenteur compare avec la
  valeur du swap entré, **exacte** sous Hull-White (ADR-X1 : la forme fermée
  dès qu'elle existe), et exerce si elle est positive et supérieure (Longstaff
  & Schwartz). Ensuite la transaction **est** ce swap : valeur exacte, flux
  compris, exposition négative possible. Vendu = quantité négative, la règle
  d'exercice reste celle du détenteur.
- **Vérifications** : un bermudan à une seule date d'exercice redonne la
  swaption européenne (mêmes valeurs au bit près après l'échéance) ; valeur
  simulée 4,942 pour 4,949 au réseau (la règle estimée est sous-optimale, la
  borne est du bon côté) ; valeur actualisée + coupons payés = martingale sur
  toute la grille, à 1,5 erreur standard ; EE* du bermudan ≥ celle de
  l'européenne. `value_today()` est le prix du réseau.
- **Mesure historique.** Une régression ne dit rien hors de la plage où
  elle a été ajustée, et les scénarios historiques n'y sont pas forcément.
  Le pilote part alors d'un **état initial dispersé**, $x(0) \sim N(0, s_0^2)$,
  avec le plus petit $s_0$ tel qu'à chaque date trois écarts-types du pilote
  couvrent quatre écarts-types des scénarios historiques autour de leur
  moyenne. La loi conditionnelle du futur sachant $x(t_i)$ n'est pas touchée,
  et c'est tout ce qu'une régression estime. Sur un swap, taux ramenés à zéro
  en un an : erreur chemin par chemin 0,56 % avec, 0,99 % sans.
- **API** : portefeuille `bermudan` (un bermudan acheté) et `cancellable`
  (swap payeur 10 ans + droit d'annuler chaque année = bermudan receveur au
  taux du swap). La réponse porte `pilot_paths`.
- **Sur les données du 1ᵉʳ octobre 2026** (contrepartie BBB à 103 pb, banque
  A, 20 000 chemins, 80 000 chemins pilotes, notionnel 10 M$) :

  | Portefeuille | Valeur | CVA | DVA |
  |---|---|---|---|
  | swap payeur 10 ans | 0 | −24 900 ± 200 | +15 500 |
  | swaption 1 an → 10 ans achetée | 302 700 | −22 600 | +4 200 |
  | bermudan payeur, annuel de 1 à 9 ans | 523 100 | −31 900 ± 200 | +900 |
  | swap annulable (swap + bermudan receveur) | 469 500 | −29 300 | +60 |

  Le droit d'annuler supprime presque tout le DVA (la banque annule quand
  le swap lui est défavorable) et **augmente** le CVA : l'option est un actif.
  Temps : 4 à 5 s pour les deux simulations (prix et mesure historique) sur
  16 threads, contre 1 s sans régression.
- **Non couverts** : scripts (X4b) ; bermudan à règlement en espèces ;
  régresseurs autres que l'état pour la continuation ; biais de la règle
  d'exercice non borné par le haut (pas de dual).

### 14.7 Lot X5 : marge initiale dynamique, MVA, ColVA, capital projeté et KVA

Livré : `utils/bucketed_regression.hpp` (la régression du lot X4, sortie de
`engines/xva/` pour servir aussi au risque), `risk/initial_margin.hpp` (DIM et
son backtest), `risk/capital.hpp` (capital projeté), ColVA dans `risk/xva.hpp`,
le collatéral attendu et la marge initiale **par chemin** dans
`risk/collateral.hpp`, le tout dans `risk/xva_report.hpp`, `qm.xva_netting_set`
et `POST /api/xva/netting-set` ; tests dans `tests/testInitialMargin.cpp`,
`tests/testCapitalProjection.cpp`, `tests/testXvaReport.cpp` et
`api/tests/test_xva.py`. FCA et FBA (symétrique ou non) dataient du lot X3.

- **DIM par régression** (Anfuso, Aziz, Giltinan & Loukopoulos 2017). Pour
  chaque date, le carré du mouvement de valeur sur la MPoR (flux de la période
  compris quand le cube les garde) est régressé sur la valeur du netting set :
  c'est sa variance conditionnelle, et $IM = \Phi^{-1}(0{,}99)\,\hat\sigma$.
  Un seul polynôme de degré 2 : une variance n'a pas de coude à suivre. La
  même marge pour les deux parties (règles de marge des dérivés non compensés).
  C'est un post-traitement du cube (ADR-X2), sans produit ni modèle.
- **Pas de SIMM.** Le lot prévoyait de caler la DIM sur une SIMM d'aujourd'hui ;
  il n'y a pas de SIMM dans le dépôt, et ses paramètres ne se citent pas de
  mémoire. La DIM donne donc **son propre** montant d'aujourd'hui (écart-type
  du mouvement sur la plus longue période partant d'aujourd'hui, ramené à la
  MPoR en racine du temps), et `im_today` permet de **recaler tout le profil**
  sur un montant calculé ailleurs. *Décision à prendre par le mainteneur :
  coder la SIMM taux (lot X5b) ou s'en tenir à ce modèle.*
- **Backtest** (le critère du lot) : ajustée sur un jeu de chemins, appliquée
  à un autre, la marge est dépassée sur 0,99 % des chemins (cible 1 %), 0,91 %
  à la hausse et 1,07 % à la baisse. Deux limites visibles, et écrites dans
  l'en-tête : la dérive de la valeur sur dix jours est ignorée (d'où
  l'asymétrie), et une fois le dernier coupon fixé la valeur ne bouge plus que
  par portage — un montant infime que le modèle gaussien ne décrit pas.
- **Exposition sous marge initiale** : `CollateralSettings` accepte une marge
  par chemin. Il reste de l'exposition sur environ un chemin sur cent (testé),
  et le CVA disparaît presque. Le FVA reste celui de la marge de variation :
  la marge ségréguée ne finance rien.
- **ColVA** : $-\sum \mathbb{E}[D\,C](t_i)\,S_C S_I\,(r_c - r)\,\Delta t_i$ sur
  le collatéral détenu ($C > 0$) ou versé, lu sur la même récursion que
  l'exposition collatéralisée.
- **Capital projeté** (Green, Kenyon & Dennis 2014). EAD SA-CCR chemin par
  chemin : l'add-on est calculé une fois par date sur les transactions
  **vieillies** (maturités raccourcies), le coût de remplacement et le
  multiplicateur viennent de la valeur du chemin, la marge initiale détenue
  compte comme collatéral et comme NICA. Capital de défaut = formule IRB sur
  cette EAD, capital CVA = BA-CVA réduit ; les deux sont linéaires en EAD.
  Paragraphes vérifiés sur le texte de Bâle en ligne le 2 octobre 2026 :
  plancher de PD 0,05 % (CRE32.4), LGD fondation 45 % / 40 % (CRE32.6),
  maturité effective plancher 1 an et plafond 5 ans (CRE32.46), moyenne
  pondérée par les notionnels (CRE32.49), pas de plafond pour BA-CVA
  (MAR50.15), bermudan $S$ = première date d'exercice et $T$ = dernière
  (CRE52.32 tableau 1, CRE52.42).
- **Approximations du capital**, toutes côté réglementaire : le delta
  prudentiel d'une option garde la monnaie d'aujourd'hui ; passée sa dernière
  date d'exercice elle est portée comme son swap sous-jacent ; ni capital de
  marché des couvertures ni ratio de levier.
- **La PD.** Faute de taux de défaut historiques en base, l'API prend par
  défaut la PD **implicite du spread** de la notation (1,7 % pour BBB), bien
  au-dessus de ce qu'estimerait le système de notation d'une banque : le
  capital est surestimé, la réponse le dit (`pd_is_market_implied`, un
  avertissement) et accepte une PD saisie. *Décision à prendre : ingérer des
  taux de défaut par notation dans `~/data-ingest`.*
- **Sur les données du 1ᵉʳ octobre 2026** (swap payeur 10 ans de 10 M$,
  contrepartie BBB, banque A, financement à 50 pb, coût du capital 10 %) :

  | | CVA | FCA | MVA | KVA | IM aujourd'hui | EAD aujourd'hui |
  |---|---|---|---|---|---|---|
  | sans CSA | −24 900 | −12 100 | 0 | −110 000 | — | 550 900 |
  | CSA (seuil nul) | −2 600 | −1 300 | 0 | −21 300 | — | 165 300 |
  | CSA + marge initiale | −20 | −1 300 | −7 400 | −4 700 | 322 200 | 45 600 |

  Le KVA est le plus gros ajustement d'un swap non collatéralisé (avec la PD
  de marché ; −83 900 avec une PD de 0,2 %). Sous marge initiale le CVA
  disparaît et le MVA qui le remplace coûte près de trois fois le CVA retiré.
- **Coût** : la DIM est réajustée pour chaque sous-ensemble (CVA incrémental
  de chaque transaction) ; le book équilibré sous CSA et marge initiale prend
  14 s à 20 000 chemins.
- **Non couverts** : DIM sous la mesure historique dans le rapport (le modèle
  s'y applique, `DynamicInitialMargin::margin`) ; régression de la dérive ;
  option du collatéral le moins cher ; FVA asymétrique récursif.

### 14.8 Lot X4b : les scripts dans le moteur d'exposition

Livré : `engines/xva/script_future_value.hpp` (`make_script_future_value`),
`ScriptedProduct::replay` (le script événement par événement),
`FutureValue::evaluate_path` et `make_workspace` (une transaction qui valorise
un chemin d'un trait, avec son état par thread), le type `script` dans
`qm.xva_netting_set` ; tests dans `tests/testExposureScript.cpp`.

- **Le script tourne sur les chemins du moteur.** À chaque événement, l'état
  $x(t)$ donne tous les facteurs d'actualisation que le script demande,
  `df(T)` $= P(t, T \mid x)$. Rejoué avec un numéraire égal à 1, ses `pays`
  **sont** les flux du chemin, date par date. Vérifié : un swap écrit en
  script paie, chemin par chemin, ce que paie le swap natif.
- **Valeur par régression** des flux à venir (la cible du lot X4a) sur l'état
  **et sur les variables du script**, qui portent ce que le chemin a déjà
  décidé. Une variable qui prend beaucoup de valeurs est un **régresseur**
  (le coupon fixé, pas encore payé) ; une variable qui en prend peu (quatre au
  plus sur les chemins pilotes) est un **régime**, avec sa propre régression
  (un drapeau `done = 1` posé à l'exercice) ; une variable identique sur tous
  les chemins est ignorée. Swap en script contre forme fermée : EE à 2 % du
  pic, PFE à 99 % à 4 %.
- **Exercice** : la règle vient du LSMC existant (`fit_exercise_policy`), sur
  le modèle hybride des scripts dont l'action n'est lue par personne. **Le
  script doit consigner l'exercice dans une variable** : vu de l'extérieur, on
  ne sait pas qu'un chemin a exercé. C'est écrit dans l'en-tête et testé (un
  bermudan réglé en espèces vaut exactement zéro après exercice).
- **Valeur d'aujourd'hui** : le prix Monte-Carlo du script (50 000 chemins par
  défaut), pas une forme fermée ; le moteur la lit désormais après la passe
  pilote.
- **Un état par thread.** L'évaluateur d'un script garde l'état du chemin : le
  moteur demande à chaque transaction un espace de travail par tâche
  (`make_workspace`) et lui passe le chemin entier (`evaluate_path`). Pour les
  transactions en forme fermée rien ne change, bit pour bit.
- **Taux seulement** : un script qui lit `spot()` ou `forward()` est refusé à
  la construction. Les scripts actions demandent le modèle joint (lot X10).
- **Capital** : SA-CCR ne décrit pas un script quelconque ; la liaison Python
  refuse de projeter le capital d'un netting set qui en contient un plutôt que
  de l'inventer.

### 14.9 Lot X6 : wrong-way risk (Hull & White 2012)

Livré : `risk/wrong_way_risk.hpp` (`wrong_way_survival`), `wrong_way_b` dans
`XvaInputs`, CVA et DVA à survie par chemin dans `risk/xva_report.hpp`, le
paramètre dans `qm.xva_netting_set` et `POST /api/xva/netting-set` ; tests dans
`tests/testWrongWayRisk.cpp` et `api/tests/test_xva.py`.

- **Le modèle** : $\lambda(t) = \exp(a(t) + b\,V(t))$, $V$ la valeur du netting
  set **avant collatéral**. Sur la grille, $\lambda$ est constant sur
  $(t_{i-1}, t_i]$ à $\exp(a_i + b\,V(t_i))$ et la survie du chemin est
  $q_i = q_{i-1}\,e^{-\lambda_i \Delta t_i}$. Post-traitement du cube (ADR-X2).
- **$a(t)$ comme dans l'article** : à chaque date, la valeur pour laquelle la
  survie **moyenne sur les chemins** est celle du marché (bissection, date
  après date ; l'exposant est décalé de son maximum pour ne jamais déborder).
  $b$ déplace la probabilité de défaut entre scénarios sans en créer. *Écart
  consigné* : caler sur l'obligation risquée $\mathbb{E}[D\,q] = P(0,t)\,S(t)$
  serait plus exact (la moyenne est prise sous la mesure de simulation), mais
  n'a pas toujours de solution sur une grille fine — la covariance entre la
  survie et l'actualisation dépasse le hazard du pas.
- **CVA et DVA par chemin** :
  $-LGD_C \sum D\,V^+ S_I(t_{i-1})\,(q_{i-1} - q_i)$ et
  $-LGD_I \sum D\,V^- q_{i-1}\,(S_I(t_{i-1}) - S_I(t_i))$. Avec $b = 0$ le
  rapport prend le chemin du lot X3 : **mêmes bits** (testé), et le CVA
  indépendant est toujours rendu à côté (`cva_independent`).
- **Constats** (tests) : la survie moyenne est celle du marché pour tout $b$ ;
  le défaut est plus fréquent là où la contrepartie doit ; le CVA croît avec
  $b$, **le DVA aussi** (là où la banque doit, la contrepartie survit plus
  longtemps, donc la banque fait plus souvent défaut la première) ; vu de
  l'autre côté, $b$ change de signe et chaque chemin a la même survie.
- **Le ratio à publier** (§8.2), sur les données du 1ᵉʳ octobre 2026, $\beta =
  b \times 10$ M$ (le paramètre de l'API) :

  | $\beta$ | −10 | −5 | 0 | +5 | +10 | +20 |
  |---|---|---|---|---|---|---|
  | swap payeur 10 ans | 0,27 | 0,54 | 1 | 1,63 | 2,34 | 3,61 |
  | book équilibré | 0,62 | 0,79 | 1 | 1,28 | 1,64 | 2,53 |
  | swaption achetée | 0,38 | 0,61 | 1 | 1,64 | 2,54 | 4,37 |

  $\beta = 10$ multiplie le hazard par $e^{0,5} \approx 1{,}65$ quand le netting
  set vaut 5 % du notionnel de plus.
- **Sous CSA l'effet disparaît presque** (swap, $\beta = 10$ : CVA −2 900 pour
  −3 500 sans dépendance). L'exposition collatéralisée est le mouvement de la
  valeur sur dix jours, pas son niveau, et il n'est pas plus grand là où la
  valeur est haute. C'est une limite du modèle, pas une bonne nouvelle : le
  wrong-way risk d'une position collatéralisée est un **saut au défaut**.
- **Ce que $b$ ne touche pas** : FCA, FBA, MVA, KVA gardent la courbe de
  survie de marché ; pas d'allocation d'Euler sous wrong-way risk.

### 14.10 Lot X5b : la SIMM, aujourd'hui et sur chaque chemin

Livré : `risk/simm.hpp` (la SIMM de la classe de risque taux, une devise),
`FutureValue::sensitivities` (ce à quoi la valeur d'une transaction est
sensible, dans un scénario), le calcul de la SIMM par chemin dans
`HullWhiteExposureEngine` (`ExposureSimulationSettings::simm`),
`simm_initial_margin` et `XvaInputs::margin_model` dans le rapport, le choix du
modèle dans `qm.xva_netting_set` et l'API ; tests dans `tests/testSimm.cpp`,
`tests/testSimmPaths.cpp`, `tests/testXvaReport.cpp`, `api/tests/test_xva.py`.
**La décision D1 du §20 est prise par ce lot** : la SIMM taux est codée.

- **Paramètres lus sur le document de l'ISDA**, version **2.8+2512** (calée à
  décembre 2025, en vigueur depuis le 11 juillet 2026), extraits par script du
  PDF public : poids de risque par vertex des trois groupes de devises (§33),
  matrice de corrélation 12 × 12 (§36), VRW = 0,20 (§35), HVR = 0,74 (§34),
  seuils de concentration (§74, §81). Chaque constante porte son paragraphe ;
  un test vérifie la symétrie de la matrice et des valeurs relevées dans le
  texte. L'ISDA recalibre chaque année : une nouvelle version est un
  changement de tables.
- **Ce qui est couvert** : une devise, une sous-courbe (la courbe OIS) —
  marges delta (§7), vega (§10) et courbure (§11, avec le facteur $HVR^{-2}$
  propre aux taux). Pas plusieurs devises ni sous-courbes, pas l'inflation ni
  la base de change, pas les autres classes de risque.
- **Les sensibilités sortent des formes fermées, pas de l'AAD** (écart au
  libellé du lot, dans l'esprit de l'ADR-X1). La valeur d'un swap est une
  somme de flux × zéro-coupons : chaque terme $c\,P(t, T)$ perd
  $(T - t)\,c\,P \times 1$ pb quand son taux zéro monte d'un point de base,
  réparti sur les deux vertex qui l'encadrent. Pour une swaption européenne,
  la formule de Hull-White donne la valeur **obligation par obligation**,
  $c_k P(t, T_k)\,Q^{T_k}(\text{exercice})$, et chaque terme est aussi la
  sensibilité à $P(t, T_k)$ : la frontière d'exercice est là où le swap vaut
  zéro, la déplacer ne change rien au premier ordre (Jamshidian). Vérifié
  contre un choc de courbe suivi d'une revalorisation complète : 1 % d'écart
  au plus, dans et hors de la monnaie.
- **Vega** : la SIMM demande une sensibilité à une vol implicite ATM, qu'un
  modèle de taux court n'a pas comme paramètre. On prend la vol normale que
  le prix implique et le vega de Bachelier à cette vol, $\sigma_N\,\partial V
  / \partial\sigma_N$. Vérifié : c'est, à 2 % près, la sensibilité du prix à
  1 % de $\sigma$ de Hull-White.
- **Critère du lot** : la SIMM d'aujourd'hui = celle calculée directement à
  partir de sensibilités par choc (1 %) ; sur un chemin, à une date de coupon,
  la SIMM du moteur = celle du même swap revalorisé de zéro sur la courbe de
  ce scénario (0,5 %).
- **SIMM par chemin contre régression.** Même profil, à un facteur stable
  d'environ 1,5 : la régression lit la volatilité du modèle, la SIMM les poids
  de l'ISDA, calés sur une période de stress. Sur les données du 1ᵉʳ octobre
  2026 (CSA à seuil nul, financement à 50 pb) :

  | Portefeuille | IM aujourd'hui, SIMM | IM, régression | MVA, SIMM | MVA, régression |
  |---|---|---|---|---|
  | swap payeur 10 ans | 487 000 | 322 200 | −9 600 | −7 400 |
  | swaption achetée | 356 800 (delta 225 900, vega 60 500, courbure 70 300) | 164 900 | −5 900 | −5 400 |
  | book équilibré | 144 900 | 47 700 | −3 800 | −2 800 |

  Le book équilibré est la leçon : **trois fois** plus de marge en SIMM. Des
  swaps de maturités différentes ne se compensent qu'à la corrélation entre
  vertex près, alors qu'un modèle à un facteur les voit parfaitement couverts.
- **Trois modèles de marge dans le rapport** : la régression seule, la
  régression **recalée sur la SIMM d'aujourd'hui** (ce que prévoyait le lot
  X5), la SIMM par chemin. L'API choisit et l'annonce : SIMM quand toutes les
  transactions savent donner leurs sensibilités, régression sinon, avec la
  raison ; le choix reste possible à la main.
- **Non couverts** : sensibilités d'un bermudan ou d'un script (valorisés par
  régression : leur valeur est une fonction de l'état, pas de chaque point de
  la courbe) — le moteur refuse la SIMM pour un netting set qui en contient ;
  sensibilités aux **taux de marché** (la SIMM le demande, §22 ; on prend les
  taux zéro) ; parts de chaque transaction sous SIMM (régression recalée) ;
  GPU (lot X7).

### 14.11 Lot X7 : le moteur d'exposition et le collatéral sur GPU

Livré : `engines/xva/exposure_program.hpp` (les transactions en tableaux
plats), `FutureValue::compile`, `risk/collateral_path.hpp` (le collatéral d'un
chemin, écrit une fois pour l'hôte et la carte), `gpu/exposure.hpp` et
`src/gpu/exposure.cu`, `ExposureSimulationSettings::device`,
`HullWhiteExposureEngine::simulate_netting_set`, le paramètre `device` de
`qm.xva_netting_set` et de l'API, `benchmarks/gpu_exposure.cpp` ; tests dans
`tests/testExposureProgram.cpp` (lancés par la CI, sans carte) et
`tests/gpu/testGpuExposure.cpp` (sur le serveur, `ctest -L gpu`).

- **Le problème, et le programme plat.** Le moteur demande sa valeur à chaque
  transaction par une interface virtuelle (`FutureValue`), qu'une carte ne
  peut pas appeler. Une transaction à forme fermée sous Hull-White n'est
  pourtant faite que de trois sortes de termes, dont aucune ne nomme un
  produit : des **exponentielles de l'état du jour**, $c\,e^{-G x(t_i)}$ (un
  flux fois un zéro-coupon) ; des **montants fixés plus tôt et payés plus
  tard** (un coupon flottant en cours) ; une **intégrale gaussienne**
  d'exponentielles sur des intervalles de l'état à une date future, avec une
  **porte** lue à cette date (une option avant son échéance, et ce qu'elle
  est devenue après). `FutureValue::compile` écrit la transaction dans ces
  termes ; le kernel les évalue. Ni le moteur ni le kernel ne savent ce
  qu'est un swap : le découpage du dépôt tient.
- **Une seule écriture.** Les fonctions d'évaluation du programme et la
  récurrence du collatéral sont marquées `QM_HOST_DEVICE` : le même code
  tourne sur l'hôte et sur la carte. `collateralise()` appelle désormais ces
  fonctions — il n'y a qu'un énoncé de ce qu'est un appel de marge — et la
  CI, qui n'a pas de carte, vérifie sur l'hôte que le programme redonne les
  valeurs et les flux de chaque transaction.
- **Deux entrées, deux usages.** `simulate()` avec `device = Gpu` rapatrie le
  **cube** : tout le post-traitement de `risk/` (collatéral, marge initiale,
  wrong-way risk, capital, rapport) tourne dessus sans changement.
  `simulate_netting_set()` fait le netting, applique le CSA et réduit les
  profils **sur la carte** : seuls EE\*, ENE\*, EFV\*, leurs erreurs et deux
  sommes pondérées par chemin reviennent (avec les probabilités de défaut
  pour poids : le CVA et le DVA, et leur erreur Monte-Carlo corrélations
  entre dates comprises). Le cube n'existe jamais : le nombre de chemins
  n'est plus borné par la mémoire de l'hôte ni le temps par la copie.
- **Reproductibilité — écart au critère du lot.** Le critère demandait
  l'égalité bit à bit CPU / 1 GPU / 2 GPU. Elle est tenue **entre les
  cartes** : chemins découpés en blocs logiques de 4 096, chaque bloc réduit
  par le même arbre, partiels repliés dans l'ordre — une carte, deux cartes,
  des lancements de n'importe quelle taille donnent les mêmes bits (testé).
  Contre le CPU elle ne peut pas l'être, pour la raison déjà établie au WP 19
  §7 : l'exponentielle et la fonction d'erreur de la carte ne sont pas celles
  de la libm, et le compilateur de la carte fusionne multiplications et
  additions. Les scénarios sont les mêmes (mêmes tirages Philox) ; les
  valeurs s'accordent à **treize ou quatorze chiffres** : écart maximal
  mesuré de 3,6 × 10⁻⁸ unité de devise sur des transactions de 10⁷ de
  notionnel, et un CVA identique au quinzième chiffre.
- **Mémoire.** Les cartes sont partagées avec le LLM de l'assistant : un
  lancement prend au plus la moitié de la mémoire libre à cet instant, et au
  plus 256 blocs. Une carte rejoint le calcul dès son premier bloc (un chemin
  vaut ici des milliers d'exponentielles ; le seuil de 128 blocs du WP 19
  est fait pour des chemins à une exponentielle).
- **`Auto`.** L'API demande `auto` : la carte quand il y en a une et que
  toutes les transactions ont une forme fermée, le CPU sinon, **avec la
  raison dans la réponse** (`device`, `gpus`, `device_reason`). Une carte
  qui échoue en cours de calcul (mémoire prise par un autre processus)
  renvoie sur le CPU au lieu de faire échouer la requête. `gpu` refuse le
  repli.

**Temps pour une erreur donnée** (`build-cuda/qm_gpu_exposure_bench 5e-4 16
4000000`) : netting set de 10 transactions (8 swaps de 2 à 20 ans, 2
swaptions), CSA à seuil nul, MTA 100 000, MPoR de 10 jours, 198 dates. Erreur
relative du CVA de 0,05 % : 126 439 chemins, CVA = −67 629 ± 34 sur chaque
ligne.

| | Temps | Rapport |
|---|---|---|
| CPU, 1 thread | 71,5 s | 1 |
| CPU, 16 threads | 8,2 s | 9 |
| CPU, 48 threads | 5,8 s | 12 |
| 1 V100, cube rapatrié puis post-traité sur l'hôte | 2,4 s (dont 1,7 s pour le cube) | 30 |
| 1 V100, réduit sur la carte | 0,206 s | 348 |
| 2 V100, réduit sur les cartes | 0,141 s | 507 |

À 4 millions de chemins (un cube de 65 Gio, qu'aucun hôte ne tient ici) :
4,8 s sur une carte, 2,4 s sur deux — 1,66 million de chemins par seconde,
et un CVA à 0,009 % près. Trois lectures :

1. **Le CPU sature bien avant 48 cœurs** (× 12) : la simulation est
   parallèle, le collatéral et les statistiques ne le sont pas.
2. **Rapatrier le cube coûte bien plus que le calculer** : 1,7 s pour
   2 Gio, quand tout le calcul réduit sur la carte prend 0,2 s. La carte ne
   paie vraiment que si la réduction s'y fait.
3. **Les lignes CPU calculent aussi la PFE** (un tri par date), que la
   réduction sur carte ne rend pas : la comparaison porte sur le CVA et son
   erreur.

**Ce que le lot change pour l'API, et ce qu'il ne change pas.** Mesuré sur le
portefeuille directionnel, 50 000 chemins, CSA : 23,0 s avant le lot, 7,6 s
après (8,2 s sans carte). La carte n'y est donc que pour 0,6 s. Le reste vient de deux corrections
de l'hôte faites en chemin : le rapport appelait `exposure_statistics` une
vingtaine de fois (CVA autonome et incrémental de chaque transaction), et
chaque appel triait tous les chemins à chaque date pour une PFE dont ces
appels n'ont pas besoin — `exposure_profile()` ne calcule que les profils,
aux mêmes bits ; et le tri de la PFE porte maintenant sur les valeurs avec
leurs poids plutôt que sur des indices. **Le temps d'une requête est
désormais celui du post-traitement sur l'hôte** (environ 7 s sur 7,6) : c'est
la suite logique, notée G1 et G2 au §20.

**Non couverts** : les transactions valorisées par régression (bermudans,
scripts) et la SIMM par chemin restent sur le CPU ; la réduction sur carte ne
donne ni PFE ni allocation d'Euler, ne prend la marge initiale que comme un
profil (pas par chemin) et ignore le wrong-way risk — pour tout cela, le
cube.

**Ne pas ajouter de produits** (règle de la roadmap) : le portefeuille de
démonstration n'utilise que ce qui existe — swaps, swaptions, bermudans, FX
forwards, options actions, CDS (WP 20), scripts.

---

## 15. Tests de propriété

La règle du dépôt (tester des propriétés plutôt que des égalités) s'applique
particulièrement bien ici :

| Propriété | Pourquoi elle est vraie |
|---|---|
| $EE(t) + ENE(t) = EFV(t)$ | identité chemin par chemin |
| $EFV^*(t) = V_0 - $ flux actualisés payés avant $t$ | la valeur actualisée d'un portefeuille est une martingale |
| $EE_{\text{netté}} \le \sum EE_k$ | sous-additivité de $x^+$ |
| $\sum_k$ contributions d'Euler $= EE_{NS}$ | homogénéité de degré 1 |
| option achetée : $CVA = -LGD\,V_0\,PD(0,T)$ | $EE^*$ constante |
| option vendue (prime payée) : $CVA = 0$ | exposition toujours négative |
| hazard nul → CVA nul ; CVA linéaire en $LGD$ à hazard fixé | définition |
| à spread fixé, CVA quasi insensible à $R$ | le hazard compense (triangle du crédit) |
| $CVA_I = DVA_C$ (mêmes données vues des deux côtés) | symétrie du premier défaut |
| EE collatéralisée $\le$ EE non collatéralisée ; croissante en seuil, en MTA, en MPoR | chaque mitigant retire de l'exposition |
| seuil infini = pas de CSA | cas limite |
| $EE_{\text{collat}} \approx 0{,}4\,\sigma\sqrt{MPoR}$ (produit gaussien, seuil nul) | §4.4 |
| IM à 99 % sur la MPoR → $\mathbb{P}(E>0) \le 1\%$ | construction de l'IM |
| exposition par régression = forme fermée (swaps) | ADR-X1 : l'oracle de l'AMC |
| $b = 0$ (WWR) → CVA indépendant au bit près | cas particulier |
| mêmes bits sur 1 / 56 threads, CPU / 1 / 2 GPU | ordre fixe des réductions |
| AAD = différences finies sur nombres aléatoires communs | WP 17 |
| SA-CCR : exemples du BCBS 279 reproduits | texte officiel |
| convergence : erreur MC du CVA en $1/\sqrt N$, pente mesurée en log-log | règle du dépôt |

---

## 16. Données et limites assumées

| Besoin | Ce qu'on a | Limite, à dire dans la doc et l'interface |
|---|---|---|
| Courbes de taux | FRED (Treasury, SOFR) dans la base `data-ingest` ; multi-courbe (WP 21) | pas de cotations OIS / swaps réelles gratuites |
| Vols de swaptions (calibration HW) | transactions de swaptions SOFR publiées par la DTCC, en base depuis le 2026-10-01 (§13.7) | médianes de transactions, échéances ≤ 2 ans, USD seulement ; pas une surface de dealer |
| Crédit des contreparties | OAS ICE BofA par notation et maturité (WP 20) | courbes proxy ; base CDS-obligation et prime de liquidité non corrigées ; pas de CDS single-name |
| Notre propre spread (DVA, FVA) | idem, par la notation qu'on se donne | hypothèse utilisateur |
| Portefeuilles, CSA, contreparties | le portefeuille de l'utilisateur (`portfolio_ledger`) | CSA et contreparties saisis par l'utilisateur ; démonstration sur portefeuilles synthétiques |
| Paramètres SIMM | méthodologie ISDA publique | licence pour tout usage commercial ; pas de tests officiels |
| Paramètres réglementaires | textes BCBS | à revérifier contre le texte primaire à l'implémentation (§ début) |

Règle des données du dépôt : tout passe par la base Postgres de
`data-ingest`, **aucun repli Yahoo Finance** ; une courbe absente est une
erreur explicite.

---

## 17. Aide-mémoire : les chiffres et ratios à connaître

**Formules d'ordre de grandeur (Gregory).**

| Quantité | Valeur |
|---|---|
| EE d'une exposition normale centrée | $\varphi(0)\,\sigma \approx 0{,}40\,\sigma$ |
| PFE 95 % / 99 % normale centrée | $1{,}645\,\sigma$ / $2{,}33\,\sigma$ |
| EE d'un forward | $\approx 0{,}4\,\sigma\sqrt t$ |
| EPE d'un forward | $\approx 0{,}27\,\sigma\sqrt T$ |
| Pic de l'EE d'un swap | vers $T/3$ |
| Facteur de netting | $\sqrt{n + n(n-1)\bar\rho}\,/\,n$ ; $1/\sqrt n$ si indépendance |
| Exposition résiduelle collatéralisée | $\approx 0{,}4\,\sigma_V\sqrt{MPoR}$ ; 10 j → $\approx 8\%$ de $\sigma_V$ |
| Triangle du crédit | $\lambda \approx s / (1 - R)$ |
| CVA en spread | $\approx EPE \times s$ |
| CVA d'une option achetée | $-LGD \times V_0 \times PD(0, T)$ |

**Chiffres réglementaires.**

| Paramètre | Valeur |
|---|---|
| Capital minimal | 8 % des RWA ; CET1 4,5 % ; coussin de conservation 2,5 % |
| $RWA$ | $12{,}5 \times K$ |
| α (IMM et SA-CCR) | 1,4 (IMM : plancher 1,2 si estimé) |
| Horizon de l'EEPE | 1 an |
| Quantile IRB | 99,9 % |
| Corrélation d'actifs IRB | 12 % à 24 %, × 1,25 pour les grandes financières |
| MPoR | 10 j (bilatéral), 5 j (client compensé), 20 j (> 5 000 transactions, collatéral illiquide), doublée après litiges |
| SA-CCR plancher du multiplicateur | 5 % |
| SA-CCR $MF_{\text{margé}}$ | $1{,}5\sqrt{MPoR / 1\text{ an}}$ |
| SA-CCR corrélations des buckets de taux | 1,4 (adjacents), 0,6 (1 et 3) |
| SA-CCR taux d'actualisation de la durée | 5 % |
| BA-CVA | $\rho = 50\%$, $DS = 0{,}65$, $\beta = 0{,}25$ |
| SA-CVA | $m_{CVA} = 1{,}25$, $R = 0{,}01$ |
| Raccourci capital CVA | notionnel non compensé ≤ 100 Md€ |
| CCP qualifiée | poids de risque 2 % sur les expositions de transaction |
| IM des dérivés non compensés | 99 %, 10 jours, période de stress incluse |
| Seuil d'IM / MTA | 50 M€ / 500 k€ |
| Dernière phase UMR | AANA > 8 Md€ (septembre 2022) |
| Grille d'IM | taux 1 / 2 / 4 %, crédit 2 / 5 / 10 %, change 6 %, actions et matières premières 15 % |
| Netting de la grille | $0{,}4 + 0{,}6 \times NGR$ |
| IM de CCP | VaR / ES 99 % à 99,7 %, 5 jours |
| Default fund des CCP | Cover 2 |
| Ratio de levier | 3 % |
| Recouvrement CDS standard | 40 % senior, 25 % subordonné |
| Coût du capital (KVA) | 10 à 15 % |
| Pertes CCR 2007-2009 | ~ 2/3 par variation du CVA, ~ 1/3 par défauts |

---

## 18. Bibliographie

**Ouvrages.**

| Référence | Pour quoi |
|---|---|
| ★ Gregory, *The xVA Challenge*, 4ᵉ éd., Wiley 2020 | tout ce document |
| ★ Savine & Andreasen, *Modern Computational Finance: Scripting for Derivatives and xVA*, Wiley 2021 | l'architecture (§13) |
| Savine, *Modern Computational Finance: AAD and Parallel Simulations*, Wiley 2018 | lot X8 |
| Green, *XVA: Credit, Funding and Capital Valuation Adjustments*, Wiley 2015 | FVA, KVA, EDP |
| Brigo, Morini & Pallavicini, *Counterparty Credit Risk, Collateral and Funding*, Wiley 2013 | cadre théorique, WWR |
| Andersen & Piterbarg, *Interest Rate Modeling*, 2010 | Hull-White, AMC |
| Glasserman, *Monte Carlo Methods in Financial Engineering*, 2003 | simulation, régression |

**Articles.**

| Référence | Lot |
|---|---|
| Pykhtin & Zhu, « A guide to modelling counterparty credit risk », *GARP Risk Review*, 2007 | X1 |
| Canabarro & Duffie, « Measuring and marking counterparty risk », 2003 | X1, α |
| Longstaff & Schwartz, « Valuing American options by simulation », *RFS*, 2001 | X4 |
| Andersen, Pykhtin & Sokol, « Rethinking the margin period of risk », *Journal of Credit Risk*, 2017 | X2 |
| Hull & White, « CVA and wrong-way risk », *Financial Analysts Journal*, 2012 | X6 |
| Brigo & Pallavicini, « Counterparty risk under correlation between default and interest rates », 2007 | X6 |
| Piterbarg, « Funding beyond discounting », *Risk*, 2010 | X5 |
| Burgard & Kjaer, « Partial differential equation representations of derivatives with bilateral counterparty risk and funding costs », *Journal of Credit Risk*, 2011 | X5 |
| Hull & White, « The FVA debate », *Risk*, 2012 | X5 |
| Green & Kenyon, « MVA by replication and regression », *Risk*, 2015 | X5 |
| Anfuso, Aziz, Giltinan & Loukopoulos, « A sound modelling and backtesting framework for forecasting initial margin requirements », *Risk*, 2017 | X5 |
| Green, Kenyon & Dennis, « KVA: capital valuation adjustment by replication », *Risk*, 2014 | X5 |
| Chourdakis, Epperlein, Jeannin & McEwen, « A cross-section across CVA », 2013 | X3 (proxies) |
| Capriotti & Giles, « Fast correlation Greeks by adjoint algorithmic differentiation », *Risk*, 2010 | X8 |

**Textes réglementaires** (à lire dans le cadre consolidé de Bâle,
bis.org/basel_framework) :

| Texte | Contenu |
|---|---|
| CRE52 (BCBS 279, 2014) | SA-CCR, avec exemples chiffrés |
| CRE53 | IMM (EEPE, α, calibration stressée) |
| CRE31 | formule IRB |
| CRE54 | expositions envers les CCP |
| MAR50 (BCBS d507, 2020) | capital CVA : BA-CVA, SA-CVA |
| BCBS-IOSCO, *Margin requirements for non-centrally cleared derivatives* (2015, révisé 2020) | UMR, grille d'IM |
| ISDA SIMM Methodology (version annuelle) et spécification CRIF | §5.3 |
| IFRS 13 | juste valeur, DVA |

---

## 19. Questions d'entretien

> Pourquoi l'EE d'un swap est-elle en cloche et celle d'un cross-currency
> swap croissante ? Pourquoi le netting rapporte-t-il peu sur un portefeuille
> directionnel ? Pourquoi une exposition collatéralisée à seuil nul n'est-elle
> pas nulle, et d'où viennent les pics autour des dates de flux ? Pourquoi
> le DVA est-il retiré du capital ? Comment le FBA recoupe-t-il le DVA ?
> Pourquoi le CVA est-il presque insensible au recouvrement à spread donné ?
> Qu'est-ce qu'α et pourquoi 1,4 ? Comment projeter l'IM future, et comment
> savoir si la projection est bonne ? Comment dériver un $\max(V, 0)$ par
> AAD, et que devient la dérivée à travers une régression ? Pourquoi le
> wrong-way risk casse-t-il la formule $\sum EE \times PD$ ?

---

## 20. Améliorations notées

Ce que les lots X4 à X6 ont laissé de côté ou révélé, pour ne pas le perdre.
Chaque point dit **ce qui manque** et **pourquoi cela compterait**. La même
liste est suivie dans l'issue GitHub #154 ; les lots
restants (X8 à X10) sont dans le tableau du §14.

### Décisions qui attendent le mainteneur

| # | Question | Ce qui en dépend |
|---|---|---|
| D1 | ~~Coder la SIMM taux ou garder la DIM par régression~~ — **tranché par le lot X5b** : la SIMM taux est codée (§14.10) | — |
| D2 | Ingérer des **taux de défaut historiques par notation** dans `~/data-ingest` | la PD du capital : aujourd'hui implicite du spread, donc un KVA surestimé (−110 000 contre −83 900 avec 0,2 %) |
| D3 | Comment **présenter $b$** (wrong-way risk) : un scénario saisi, ou un paramètre estimé sur l'historique spread / valeur comme le proposent Hull & White | la page `/xva` (X9) |
| D4 | Exposer des **transactions en script** dans les portefeuilles de l'API | demande une description SA-CCR du script, ou un capital désactivable |

### Exposition par régression (X4a, X4b)

| # | Amélioration | Pourquoi |
|---|---|---|
| A1 | Cible de régression moins bruitée : régresser la valeur estimée à la date suivante plutôt que tous les flux futurs, ou une variable de contrôle en forme fermée | c'est le bruit qui limite : 2 % d'écart sur l'EE à 20 000 chemins pilotes, d'où un pilote quatre fois plus gros que la simulation |
| A2 | Borne supérieure duale de la règle d'exercice (Andersen-Broadie) | le biais du LSMC n'est borné que par le bas (4,942 pour 4,949 au réseau) |
| A3 | État d'exercice du bermudan calculé une fois par chemin (`evaluate_path`) au lieu d'être recalculé à chaque date | 9 s pour 40 000 chemins sur un thread |
| A4 | Bermudan réglé en espèces ; régresseurs autres que l'état (taux swap résiduel) | couverture des produits ; qualité de la continuation |
| A5 | Scripts : rendre l'état « exercé » de l'évaluateur visible comme régime automatique | aujourd'hui le script doit poser lui-même un drapeau, sinon son exposition après exercice est fausse |
| A6 | Scripts : erreur Monte-Carlo de la valeur d'aujourd'hui ; `forward()` ; scripts actions (modèle joint, lot X10) | la valeur d'aujourd'hui d'un script est un prix simulé, sans son erreur |
| A7 | Scripts : mémoire de la passe pilote (chemins × événements × variables) hors de la limite du moteur | un script long à sept variables et 200 000 chemins pilotes prend plusieurs centaines de Mo que le moteur ne compte pas |
| A8 | Pilote sous la mesure historique : la règle de couverture (3 σ du pilote pour 4 σ des scénarios) est une heuristique | à valider sur des dynamiques extrêmes |
| A9 | Courbe de forwards en dents de scie sur les données DTCC (lissage, ou ajustement de la courbe aux transactions) | visible dans Market et dans tout profil d'exposition |

### Marge, financement et capital (X5)

| # | Amélioration | Pourquoi |
|---|---|---|
| B1 | DIM : régresser aussi la **dérive** du mouvement | marge dépassée 0,91 % à la hausse et 1,07 % à la baisse au lieu de 1 % des deux côtés |
| B2 | DIM : cas dégénéré après le dernier fixing (mouvement tout en portage) | fréquences de dépassement de 13 à 40 % sur une marge infime |
| B3 | DIM dans la mesure historique du rapport (PFE sous marge initiale) | le modèle s'y applique déjà (`DynamicInitialMargin::margin`), le rapport ne le fait pas |
| B4 | DIM réajustée pour chaque sous-ensemble (CVA incrémental) : mise en cache ou allocation | 14 s pour le book équilibré sous CSA et marge initiale |
| B5 | Capital : delta prudentiel à la monnaie **future**, état d'exercice par chemin | aujourd'hui figé à la monnaie d'aujourd'hui, et l'option échue est portée comme son swap |
| B6 | Capital : risque de marché des couvertures, ratio de levier, SA-CVA sur sensibilités AAD (lot X8) | le KVA ne couvre que le défaut (IRB) et le CVA (BA-CVA réduit) |
| B7 | FVA sur le collatéral non décalé ; FVA asymétrique récursif (Brigo et al.) ; option du collatéral le moins cher | le financement utilise l'exposition collatéralisée à la MPoR |
| B8 | Exposition sous marge initiale avec le traitement « Paid » des flux : pics de coupons | CVA −529 contre −20 en « Withheld » ; à montrer comme tel sur la page |

### SIMM (X5b)

| # | Amélioration | Pourquoi |
|---|---|---|
| S1 | Sensibilités d'un **bermudan** et d'un **script** par chemin (AAD à travers la régression, ou régression des sensibilités) | la SIMM par chemin est refusée dès qu'un netting set en contient un ; l'API retombe sur la régression |
| S2 | Sensibilités aux **taux de marché** (jacobienne taux zéro → taux de swap) | la méthodologie le demande (§22) ; l'écart est faible sur une courbe lisse mais non nul |
| S3 | Plusieurs devises et sous-courbes, inflation, base de change ; les autres classes de risque | la SIMM codée est celle d'un netting set de taux dans une devise |
| S4 | Parts de chaque transaction sous SIMM par chemin | aujourd'hui calculées avec la régression recalée sur la SIMM |
| S5 | Vega : interpolation dans le cube de vols plutôt que la vol propre de chaque swaption | suffisant tant que le modèle n'a qu'une volatilité |
| S6 | Suivre les versions de l'ISDA (recalibrage annuel) : paramètres en données plutôt qu'en constantes | la version 2.8+2512 est codée en dur |
| S7 | SIMM par chemin sur GPU | 12 × 12 produits par chemin et par date : parallèle par nature ; non fait au lot X7 (voir G5) |

### GPU (X7)

| # | Amélioration | Pourquoi |
|---|---|---|
| G1 | **Le rapport xVA réduit sur la carte** : le CVA autonome et incrémental de chaque transaction comme autant de netting sets du même lancement | le temps d'une requête est celui du post-traitement sur l'hôte : environ 7 s sur 7,6 (50 000 chemins, CSA) ; la carte fait un netting set de 126 000 chemins en 0,2 s |
| G2 | Paralléliser `xva_report` sur l'hôte : les `2K + 1` appels à `collateralise` sont indépendants | l'alternative à G1 sans carte, et ce qui sert les bermudans |
| G3 | PFE sur la carte (histogramme, ou tri sur la carte) | la réduction ne rend pas de quantile |
| G4 | Transactions par régression sur la carte : évaluer les polynômes par bucket dans le kernel ; puis le pilote lui-même | bermudans et scripts renvoient tout le netting set sur le CPU |
| G5 | SIMM par chemin sur la carte (S7) | la marge SIMM renvoie aussi sur le CPU |
| G6 | Mémoire épinglée et pas de remise à zéro du cube rapatrié | 1,7 s pour 2 Gio, contre 0,2 s pour tout le calcul réduit sur la carte |
| G7 | Fusionner les obligations de même échéance dans `bind` (les coupons flottants se télescopent) | environ 2,5 fois moins d'exponentielles, sur CPU comme sur carte |
| G8 | Marge initiale par chemin et wrong-way risk dans la réduction | aujourd'hui : profil de marge seulement, pas de $b$ |
| G9 | Flux de la période de marge par sommes cumulées | la boucle relit toute la période à chaque date de reporting |

### Wrong-way risk (X6)

| # | Amélioration | Pourquoi |
|---|---|---|
| C1 | **Saut au défaut** (devise, sous-jacent) pour les positions collatéralisées | sous CSA le modèle de Hull & White ne donne presque rien : l'exposition est un mouvement, pas un niveau |
| C2 | Hazard fonction d'un **facteur de marché** (le niveau des taux) plutôt que de $V$ | « la contrepartie souffre quand les taux montent » serait wrong-way pour un payeur et right-way pour un receveur ; avec $V$, $b > 0$ est wrong-way pour tout le monde |
| C3 | Caler $a(t)$ sur l'obligation risquée plutôt que sur la survie moyenne | la moyenne est prise sous la mesure de simulation ; écart du second ordre |
| C4 | Erreur Monte-Carlo du calage de $a(t)$ dans l'erreur du CVA ; allocation d'Euler sous wrong-way risk | l'erreur affichée ignore le bruit du calage |
| C5 | Wrong-way risk dans FCA, FBA, MVA, KVA (survie par chemin) et sur le défaut propre (DVA) | seuls le CVA et le DVA voient $b$ |
| C6 | Estimer $b$ sur l'historique | voir D3 |

### Transverse

| # | Amélioration | Pourquoi |
|---|---|---|
| T1 | Rejeu d'un événement `xva.valuation` (issue #141) | prévu au lot X3, pas branché |
| T2 | Convention de prime des swaptions au-delà de deux ans (issue #142) | limite la grille de vols à 2 ans d'échéance |
| T3 | Calage Heston / rough Bergomi dégénéré sur SPY (issue #146) | la page Scripting valorise sous un modèle qui ne redonne pas un forward |
| T4 | Grecques de la swaption dans le workbench de pricing | le bloc `greeks` est vide |
| T5 | Temps de réponse de l'API xVA avec régression et marge initiale (5 à 15 s) ; sans régression, 7,6 s à 50 000 chemins après le lot X7 | à surveiller avant la page `/xva` ; voir G1 et G2 |
