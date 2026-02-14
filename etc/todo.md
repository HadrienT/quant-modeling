# Checklist Desk Grade — Version orientée C++

---

## 1. Produits / Instruments

### Abstraction des instruments
- Classe de base `Instrument`
  - `virtual payoff(...)`
  - `virtual exercise(...)`
  - `virtual cashflows(...)`
- Aucun accès aux données de marché dans `Instrument`
- Objets **immutables** après construction

### Instruments supportés
- Vanilla (Call / Put)
- Barrier (KI / KO / Double)
- Digital
- Asian (arithmétique / géométrique)
- Lookback
- Basket
- Cliquet
- Autocall
- Swaps / Swaptions
- Caps / Floors
- CMS

### Conception des payoffs
- Payoff implémenté comme **Strategy** (`Payoff` interface)
- Payoffs dépendants du chemin séparés de `Instrument`
- Aucun branchement sur le type de produit dans les engines

### Exercices
- Classe de base `Exercise`
  - Européen
  - Américain
  - Bermudéen
- Dates d’exercice pré-validées (triées, ≥ date de valuation)

---

## 2. Cashflows / Legs

### Hiérarchie de cashflows
- `Cashflow` (abstraite)
- `FixedCashflow`
- `FloatingCashflow`
- `OISCashflow`
- `InflationCashflow`

### Construction des legs
- `vector<shared_ptr<Cashflow>>`
- Convention de day count explicite
- Capitalisation explicite
- Ajustement de date de paiement via `Calendar`

### Logique d’accrual et de fixing
- Lag de fixing explicite
- Fixings historiques injectés de manière externe

---

## 3. Marché / Data

### Conteneur de données de marché
- `MarketData` comme snapshot immuable
- Aucun singleton global
- Accès thread-safe en lecture seule

### Courbes
- Interface `YieldCurve`
  - `discount(t)`
  - `forward(t1, t2)`
- Support multi-courbes
- Validation de la monotonie de l’interpolation

### Volatilité
- Interface `VolSurface`
  - `vol(T, K)`
- Smiles Equity / FX
- Cube de volatilité taux

### Corrélation
- `CorrelationMatrix` / `CorrelationSurface`
- Vérification PSD (ou régularisation en fallback)

### Calendriers
- Interface `Calendar`
- `BusinessDayConvention` explicite

---

## 4. Modèles

### Abstraction des modèles
- Classe de base `Model`
  - Définition des variables d’état
  - `evolve(dt)`
- Le modèle possède ses paramètres, **pas** les données de marché

### Modèles Equity / FX
- Black–Scholes
- Local Volatility
- Volatilité stochastique
  - Heston
  - SABR
- Jump Diffusion

### Modèles de taux
- Hull–White
- Black / Kirk
- LMM (discrétisé)

### Modèles de crédit
- Hazard Rate / Intensity
- Courbe CDS bootstrapée en externe

---

## 5. Calibration

### Framework de calibration
- Interface `ObjectiveFunction`
- Contraintes explicites
- Optimiseur interchangeable

### Stabilité numérique
- Bornes de paramètres imposées
- Taille de bump finie contrôlée
- Calibration reproductible (seed fixe)

---

## 6. Engines de Pricing

### Conception des engines
- Interface `PricingEngine`
  - `price()`
  - `greeks()`
- Aucune logique spécifique produit dans l’engine

### Formules fermées
- Black–Scholes
- Black 76
- Bachelier
- Heston (Fourier)

### Lattices
- Binomial / Trinomial
- Arbres de taux courts
- Arbre LMM

### PDE / Différences finies
- Schémas explicite / implicite / Crank–Nicolson
- Schémas ADI
- Conditions aux limites explicites
- Tests de convergence de grille

### Monte Carlo
- Abstraction RNG
- Générateur de chemins séparé du payoff
- Réduction de variance
  - Antithétique
  - Variables de contrôle
- Régression (LSMC)

---

## 7. Greeks

### Calcul des sensibilités
- Analytique lorsque possible
- Différences finies (bump-and-reprice)
- Pathwise / Likelihood Ratio
- AAD (optionnel / avancé)

### Règles de bump
- Différence centrale vs forward explicite
- Bump relatif vs absolu explicite

---

## 8. Transverse C++ (Desk Grade)

### Validation des entrées
- Dates strictement croissantes
- Maturités > 0
- Volatilités ≥ 0
- Courbes monotones si requis

### Déterminisme
- RNG seedé explicitement
- Aucune randomisation cachée

### Performance
- Pas d’allocation dynamique dans les boucles internes
- Move semantics activées
- `const` correctness partout
- Layouts compatibles vectorisation

### Thread safety
- `MarketData` et `Model` immuables
- Un RNG par thread
- Aucun état statique mutable

### Gestion des erreurs
- `Result<T>` / `StatusOr<T>` privilégiés
- Diagnostics attachés au résultat
- Exceptions réservées aux erreurs fatales

### Tests
- Tests unitaires par classe
- Pricing vs références (QuantLib / Excel)
- Tests de convergence et de régression

### Configuration
- Paramètres engine / modèle sérialisables
- JSON / YAML
- Schémas versionnés

### API & Architecture
- Séparation claire headers publics / privés
- ABI stable si bibliothèque
- PIMPL si nécessaire
- Dépendances d’includes minimales
