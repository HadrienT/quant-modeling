## 📁 Project Structure — `pricing`

Le module `pricing` implémente une architecture de pricing générique et extensible,
séparant clairement **produits**, **modèles**, **méthodes numériques** et **orchestration**.

---

### 🔧 Build & Tooling

- `CMakeLists.txt`  
  Point d’entrée CMake du module `pricing`.

- `cmake/`  
  Outils CMake partagés.
  - `toolchains/` — toolchains optionnels (clang, gcc, vcpkg, …)
  - `modules/` — helpers et modules `Find*.cmake`

---

### 📦 Public API — `include/pricing/`

En-têtes publics exposant l’API du moteur de pricing.

---

#### 🧱 Core

Types fondamentaux et concepts transverses.

- `core/types.hpp` — `Money`, `Currency`, `Date`, etc.
- `core/timegrid.hpp` — discrétisation temporelle
- `core/results.hpp` — `PriceResult`, Greeks, diagnostics
- `core/exceptions.hpp` — exceptions métier

---

#### 📈 Market

Abstractions de données de marché et conventions financières.

- `market/marketdata.hpp` — interfaces (`MarketDataProvider`)
- `market/curves.hpp` — `DiscountCurve`, `ForwardCurve`
- `market/surfaces.hpp` — `VolSurface`, `VolSmile`
- `market/quotes.hpp` — `Quote`, conventions de cotation
- `market/conventions.hpp` — day count, business day rules
- `market/calendars.hpp` — calendriers

---

#### 🧾 Instruments

Description des produits financiers (payoffs, exercices).

- `instruments/base.hpp` — `Instrument`, `Payoff`, `Exercise`

- `instruments/equity/`
  - `vanilla.hpp` — options européennes / américaines
  - `barrier.hpp`
  - `asian.hpp`
  - `cliquet.hpp`

- `instruments/rates/`
  - `swaption.hpp`
  - `capfloor.hpp`

- `instruments/credit/`
  - `cds.hpp`

---

#### 🧮 Models

Modèles stochastiques ou déterministes sous-jacents.

- `models/base.hpp` — interface modèle (`simulate`, `pde_coeffs`, …)

- `models/equity/`
  - `black_scholes.hpp`
  - `heston.hpp`
  - `localvol.hpp`

- `models/rates/`
  - `hull_white.hpp`

---

#### ⚙️ Engines

Méthodes numériques de pricing.

- `engines/base.hpp` — interface `PricingEngine`

- `engines/mc/` — Monte Carlo
  - `engine.hpp`
  - `rng.hpp`
  - `paths.hpp`
  - `estimators.hpp` — control variates, antithetic, etc.

- `engines/pde/` — méthodes aux dérivées partielles
  - `engine.hpp`
  - `grid.hpp`
  - `schemes.hpp` — CN, implicite, explicite
  - `boundary.hpp`

- `engines/tree/` — arbres et lattices
  - `engine.hpp`
  - `lattice.hpp`

---

#### 🧩 Pricers

Orchestration **Instrument + Model + Engine**.

- `pricers/pricer.hpp` — orchestrateur principal
- `pricers/registry.hpp` — mapping instrument / modèle → engine

- `pricers/adapters/`
  - `equity_vanilla.hpp` — configuration MC/PDE pour ce cas
  - `equity_barrier.hpp`

---

#### 📊 Risk

Outils de risque et analyses de sensibilité.

- `risk/greeks.hpp` — bump-and-reprice, pathwise, adjoint (à venir)
- `risk/scenarios.hpp` — scénarios de stress

---

#### 📥 IO

Chargement et sérialisation des données.

- `io/loaders.hpp` — parsers CSV / JSON
- `io/serializers.hpp`

---

#### 🛠 Utils

Fonctions utilitaires transverses.

- `utils/math.hpp`
- `utils/stats.hpp`
- `utils/logging.hpp`

---

### 🏗 Implementation — `src/`

Implémentations correspondantes aux headers publics, miroir de `include/pricing/`.

*(structure identique : `core/`, `market/`, `instruments/`, `models/`, `engines/`, etc.)*

---

### 🖥 Applications

- `apps/cli/`
  - `main.cpp` — binaire CLI (pricing, calibration, …)

- `apps/examples/`
  - `european_bs_mc.cpp` — exemples rapides / sanity checks
  - `european_bs_pde.cpp`

---

### 🧪 Tests

- `tests/CMakeLists.txt`
- `tests/unit/` — tests unitaires
- `tests/integration/` — tests end-to-end

---

### 📦 Third-party

- `third_party/`  
  Dépendances vendorisées (optionnel, sinon via vcpkg / conan).
