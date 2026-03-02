cmake_minimum_required(VERSION 3.20)

project(quantModeling
    VERSION 0.1
    LANGUAGES CXX
)

# ---- Build options ----
option(QM_BUILD_CLI "Build CLI executable" OFF)
option(QM_BUILD_TESTS "Build unit tests (GoogleTest)" OFF)
option(QM_BUILD_PYTHON "Build Python bindings (pybind11)" OFF)

# ---- Global Options  ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Main library ----
add_library(quantModeling)

set_target_properties(quantModeling PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_sources(quantModeling
    PRIVATE
        src/utils/stats.cpp
        src/utils/greeks.cpp
        src/engines/base.cpp
        src/engines/analytic/black_scholes.cpp
        src/engines/analytic/asian.cpp
        src/engines/analytic/future.cpp
        src/engines/analytic/bonds.cpp
        src/engines/mc/black_scholes.cpp
        src/engines/mc/asian.cpp
        src/engines/mc/barrier.cpp
        src/engines/mc/basket.cpp
        src/engines/mc/lookback.cpp
        src/engines/mc/local_vol.cpp
        src/engines/tree/binomial.cpp
        src/engines/tree/trinomial.cpp
        src/engines/pde/european_vanilla.cpp
        src/market/discount_curve.cpp
        src/instruments/equity/vanilla.cpp
        src/instruments/equity/asian.cpp
        src/instruments/equity/barrier.cpp
        src/instruments/equity/basket.cpp
        src/instruments/equity/lookback.cpp
        src/instruments/equity/future.cpp
        src/instruments/equity/autocall.cpp
        src/instruments/equity/mountain.cpp
        src/instruments/equity/variance_swap.cpp
        src/instruments/equity/dispersion.cpp
        src/instruments/equity/rainbow.cpp
        src/instruments/fx/forward.cpp
        src/instruments/fx/option.cpp
        src/instruments/commodity/forward.cpp
        src/instruments/commodity/option.cpp
        src/instruments/rates/zero_coupon_bond.cpp
        src/instruments/rates/fixed_rate_bond.cpp
        src/models/rates/flat_rate.cpp
        src/models/rates/vasicek.cpp
        src/models/rates/cir.cpp
        src/models/rates/hull_white.cpp
        src/instruments/rates/bond_option.cpp
        src/instruments/rates/capfloor.cpp
        src/instruments/rates/caplet.cpp
        src/engines/analytic/short_rate.cpp
        src/engines/mc/short_rate.cpp
        src/engines/mc/autocall.cpp
        src/engines/mc/mountain.cpp
        src/engines/mc/variance_swap.cpp
        src/engines/mc/dispersion.cpp
        src/engines/mc/rainbow.cpp
        src/engines/analytic/variance_swap.cpp
        src/engines/analytic/fx.cpp
        src/engines/analytic/commodity.cpp
        src/pricers/registry.cpp
        src/pricers/adapters/equity_vanilla.cpp
        src/pricers/adapters/equity_vanilla_american.cpp
        src/pricers/adapters/equity_asian.cpp
        src/pricers/adapters/equity_asian_lv.cpp
        src/pricers/adapters/equity_barrier.cpp
        src/pricers/adapters/equity_barrier_lv.cpp
        src/pricers/adapters/equity_basket.cpp
        src/pricers/adapters/equity_lookback.cpp
        src/pricers/adapters/equity_lookback_lv.cpp
        src/pricers/adapters/equity_future.cpp
        src/pricers/adapters/bonds.cpp
        src/pricers/adapters/rates_short_rate.cpp
        src/pricers/adapters/equity_autocall.cpp
        src/pricers/adapters/equity_mountain.cpp
        src/pricers/adapters/equity_vol_swap.cpp
        src/pricers/adapters/equity_dispersion.cpp
        src/pricers/adapters/equity_rainbow.cpp
        src/pricers/adapters/fx.cpp
        src/pricers/adapters/commodity.cpp
        src/engines/analytic/digital.cpp
        src/instruments/equity/digital.cpp
        src/pricers/adapters/equity_digital.cpp
)

target_include_directories(quantModeling
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Eigen3 CONFIG REQUIRED)
target_link_libraries(quantModeling PUBLIC Eigen3::Eigen)

target_compile_options(quantModeling PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)

# ---- CLI ----
if(QM_BUILD_CLI)
  add_executable(quantModeling_cli main.cpp)
  target_link_libraries(quantModeling_cli PRIVATE quantModeling)
  target_compile_options(quantModeling_cli PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  )
endif()

# ---- Tests (GoogleTest) ----
if(QM_BUILD_TESTS)
  include(CTest)
  enable_testing()

  find_package(GTest CONFIG REQUIRED)

  add_executable(quantModeling_tests
    tests/testBSAna.cpp
    tests/testBSMC.cpp
    tests/testAsianOption.cpp
    tests/testLookback.cpp
    tests/testBasket.cpp
    tests/testFuture.cpp
    tests/testBonds.cpp
    tests/testDigital.cpp
    tests/testBarrier.cpp
    tests/testBinomial.cpp
    tests/testTrinomial.cpp
    tests/testPDE.cpp
    tests/testDiscountCurve.cpp
    tests/testLocalVol.cpp
    tests/testUtils.cpp
    tests/testRegistry.cpp
    tests/testModels.cpp
    tests/testShortRateModels.cpp
    tests/testStructuredProducts.cpp
    tests/testNewProducts.cpp
    tests/testRainbow.cpp
  )

  target_link_libraries(quantModeling_tests
    PRIVATE
      quantModeling
      GTest::gtest_main
  )

  target_compile_options(quantModeling_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  )

  include(GoogleTest)
  gtest_discover_tests(quantModeling_tests)
endif()

# ---- Python binding (pybind11) ----
if(QM_BUILD_PYTHON)
  add_subdirectory(bindings/python)
endif()
