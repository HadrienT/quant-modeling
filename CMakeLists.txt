cmake_minimum_required(VERSION 3.20)

project(quantModeling
    VERSION 0.1
    LANGUAGES CXX
)

# ---- Build options ----
option(QM_BUILD_CLI "Build CLI executable" OFF)
option(QM_BUILD_TESTS "Build unit tests (GoogleTest)" OFF)
option(QM_BUILD_PYTHON "Build Python bindings (pybind11)" OFF)
option(QM_BUILD_BENCHMARKS "Build benchmarks (google-benchmark)" OFF)
option(QM_NATIVE_ARCH "Optimize for the host CPU (-march=native)" OFF)
option(QM_ENABLE_CUDA "Build the CUDA Monte-Carlo backend (blueprint/wp/19-gpu.md)" OFF)

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
        src/aad/tape.cpp
        src/aad/number.cpp
        src/utils/stats.cpp
        src/utils/greeks.cpp
        src/utils/sobol_directions.cpp
        src/core/date.cpp
        src/core/period.cpp
        src/core/timegrid.cpp
        src/scripting/token.cpp
        src/scripting/script_error.cpp
        src/scripting/lexer.cpp
        src/scripting/parser.cpp
        src/scripting/domain.cpp
        src/market/conventions.cpp
        src/market/calendars.cpp
        src/market/schedule.cpp
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
        src/market/curve_bootstrap.cpp
        src/market/raw_vol_surface.cpp
        src/market/svi.cpp
        src/market/svi_calibration.cpp
        src/market/svi_surface.cpp
        src/market/dupire_from_svi.cpp
        src/market/vol_surface_pipeline.cpp
        src/market/sabr_calibration.cpp
        src/market/slv_calibration.cpp
        src/market/heston_calibration.cpp
        src/market/superbucket.cpp
        src/market/calibration/levenberg_marquardt.cpp
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
        src/models/equity/sabr.cpp
        src/models/equity/sabr_pde.cpp
        src/models/equity/heston.cpp
        src/engines/analytic/heston_cos.cpp
        src/engines/mc/heston_qe.cpp
        src/engines/mc/rough_bergomi_hybrid.cpp
        src/engines/mc/path_simulation.cpp
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
        src/pricers/adapters/equity_quanto.cpp
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
        src/utils/thread_pool.cpp
)

# ---- CUDA backend (blueprint/wp/19-gpu.md §8) ----
# Off by default: the CI and the Docker image have no GPU and build exactly as
# before, with src/gpu/cpu_only.cpp answering "no device". On the server, use
# the `cuda` preset -- it sets the V100 architecture (sm_70) and g++-13 as
# nvcc's host compiler (CUDA 12.4 rejects gcc 14; CUDA 12.x is the last branch
# that supports the V100, so this pin stays).
if(QM_ENABLE_CUDA)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 70)
  endif()
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  # nvcc's host compiler (g++-13) is not the one that links (the project's
  # g++): drop g++-13's library directory, or its older libstdc++ is linked
  # against objects built by the newer one.
  list(FILTER CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES EXCLUDE REGEX "/gcc/")
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  target_sources(quantModeling
      PRIVATE
          src/gpu/device.cu
          src/gpu/vanilla_bs.cu
          src/gpu/rng.cu
          src/gpu/paths.cu
  )
  target_link_libraries(quantModeling PUBLIC CUDA::cudart_static)
  target_compile_options(quantModeling PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr -Xcompiler=-Wall>
  )
else()
  target_sources(quantModeling PRIVATE src/gpu/cpu_only.cpp)
endif()

target_include_directories(quantModeling
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Eigen3 CONFIG REQUIRED)
target_link_libraries(quantModeling PUBLIC Eigen3::Eigen)

find_package(Threads REQUIRED)
target_link_libraries(quantModeling PUBLIC Threads::Threads)

target_compile_options(quantModeling PRIVATE
  $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
)

if(QM_NATIVE_ARCH)
  target_compile_options(quantModeling PUBLIC
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-march=native>
  )
endif()

# ---- CLI ----
if(QM_BUILD_CLI)
  add_executable(quantModeling_cli main.cpp)
  target_link_libraries(quantModeling_cli PRIVATE quantModeling)
  target_compile_options(quantModeling_cli PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  )

  add_executable(qm_price_script examples/scripting/price_script.cpp)
  target_link_libraries(qm_price_script PRIVATE quantModeling)
  target_compile_options(qm_price_script PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  )
endif()

# ---- Tests (GoogleTest) ----
if(QM_BUILD_TESTS)
  include(CTest)
  enable_testing()

  find_package(GTest CONFIG REQUIRED)

  add_executable(quantModeling_tests
    tests/testAADNumber.cpp
    tests/testAADTape.cpp
    tests/testAADSimulation.cpp
    tests/testAADMulti.cpp
    tests/testAADExpression.cpp
    tests/testMultiAssetBSSimModel.cpp
    tests/testLocalVolSimModel.cpp
    tests/testMertonJumpDiffusionSimModel.cpp
    tests/testKouJumpDiffusionSimModel.cpp
    tests/testBatesSimModel.cpp
    tests/testSABRSimModel.cpp
    tests/testVasicekSimModel.cpp
    tests/testSLVSimModel.cpp
    tests/testRoughBergomiSimModel.cpp
    tests/testSchwartzSmithSimModel.cpp
    tests/testThreadPool.cpp
    tests/testRNGSkipTo.cpp
    tests/testParallelAAD.cpp
    tests/testScriptAnalysis.cpp
    tests/testScriptModels.cpp
    tests/testDate.cpp
    tests/testConventions.cpp
    tests/testCalendars.cpp
    tests/testSchedule.cpp
    tests/testTimeline.cpp
    tests/testScriptParser.cpp
    tests/testScriptEval.cpp
    tests/testScriptDomain.cpp
    tests/testScriptFuzzy.cpp
    tests/testScriptParity.cpp
    tests/testSimulationEngine.cpp
    tests/testBSAna.cpp
    tests/testQuanto.cpp
    tests/testBSMC.cpp
    tests/testAsianOption.cpp
    tests/testAsianVR.cpp
    tests/testVanillaVR.cpp
    tests/testSmoothing.cpp
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
    tests/testCurveBootstrap.cpp
    tests/testCurveAwareBSSimModel.cpp
    tests/testRawVolSurface.cpp
    tests/testSVI.cpp
    tests/testSVICalibration.cpp
    tests/testSVISurface.cpp
    tests/testDupireFromSVI.cpp
    tests/testVolSurfacePipeline.cpp
    tests/testSABR.cpp
    tests/testSABRPDE.cpp
    tests/testSABRCalibration.cpp
    tests/testHestonCalibration.cpp
    tests/testSuperbucket.cpp
    tests/testModelRecommendation.cpp
    tests/testHeston.cpp
    tests/testHestonCOS.cpp
    tests/testHestonQE.cpp
    tests/testRoughBergomi.cpp
    tests/testRoughBergomiHybrid.cpp
    tests/testPathSimulation.cpp
    tests/testLocalVol.cpp
    tests/testUtils.cpp
    tests/testRegistry.cpp
    tests/testModels.cpp
    tests/testShortRateModels.cpp
    tests/testStructuredProducts.cpp
    tests/testNewProducts.cpp
    tests/testRainbow.cpp
    tests/testInverseNormal.cpp
    tests/testSobol.cpp
    tests/testLevenbergMarquardt.cpp
    tests/testPhilox.cpp
    tests/testLogicalBlocks.cpp
    tests/testScriptBytecode.cpp
  )

  target_link_libraries(quantModeling_tests
    PRIVATE
      quantModeling
      GTest::gtest_main
  )

  # The script library (blueprint WP 19 lot G2 prices every product of it).
  target_compile_definitions(quantModeling_tests PRIVATE
    QM_PRODUCT_LIBRARY_DIR="${PROJECT_SOURCE_DIR}/api/app/product_library"
  )

  target_compile_options(quantModeling_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
  )

  include(GoogleTest)
  gtest_discover_tests(quantModeling_tests)

  # GPU tests: only with the CUDA backend, labelled so that they can be run
  # (or skipped) as a group -- `ctest -L gpu`. The GitHub CI has no GPU and
  # never builds them.
  if(QM_ENABLE_CUDA)
    add_executable(quantModeling_gpu_tests
      tests/gpu/testGpuVanilla.cpp
      tests/gpu/testGpuPaths.cpp
    )
    target_link_libraries(quantModeling_gpu_tests PRIVATE quantModeling GTest::gtest_main)
    target_compile_options(quantModeling_gpu_tests PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wconversion -Wshadow>
    )
    gtest_discover_tests(quantModeling_gpu_tests PROPERTIES LABELS gpu)
  endif()
endif()

# ---- Benchmarks (google-benchmark) ----
if(QM_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)

  add_executable(quantModeling_bench
    benchmarks/bench_vanilla_mc.cpp
    benchmarks/bench_aad_basket.cpp
  )
  target_link_libraries(quantModeling_bench
    PRIVATE
      quantModeling
      benchmark::benchmark_main
  )
  target_compile_options(quantModeling_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
  )
endif()

# ---- GPU benchmark (blueprint/wp/19-gpu.md §9) ----
# Not google-benchmark: the metric is the wall time to reach a target standard
# error, not paths per second. A command-line tool like the CLI: the wheel
# build has neither.
if(QM_ENABLE_CUDA AND QM_BUILD_CLI)
  add_executable(qm_gpu_bench benchmarks/gpu_time_to_error.cpp)
  target_link_libraries(qm_gpu_bench PRIVATE quantModeling)
endif()

# ---- Python binding (pybind11) ----
if(QM_BUILD_PYTHON)
  # Build stamp read by quantmodeling.build_sha() (blueprint WP 18a §5.1): an
  # audit event records exactly which native build produced a valuation, which
  # is what lot 18e's replay endpoint compares against.
  #
  # It is a digest of the native sources' CONTENT (include/, src/, the CMake
  # files), not the git commit: the Docker build context has no .git (so a
  # commit SHA came out "unknown" in production), and a commit SHA would have
  # to enter the C++ build stage, rebuilding the library on every deploy even
  # when only the front changed. The same sources give the same stamp, a dirty
  # tree a different one. Computed at configure time: an incremental local
  # build refreshes it on the next reconfigure.
  file(GLOB_RECURSE _qm_stamp_files CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cu
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cuh
    ${CMAKE_CURRENT_SOURCE_DIR}/bindings/*.txt
  )
  list(APPEND _qm_stamp_files ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
  list(SORT _qm_stamp_files)
  set(_qm_stamp_manifest "")
  foreach(_qm_file IN LISTS _qm_stamp_files)
    file(SHA256 ${_qm_file} _qm_file_hash)
    file(RELATIVE_PATH _qm_rel ${CMAKE_CURRENT_SOURCE_DIR} ${_qm_file})
    string(APPEND _qm_stamp_manifest "${_qm_rel} ${_qm_file_hash}\n")
  endforeach()
  string(SHA256 QM_BUILD_SHA "${_qm_stamp_manifest}")
  string(SUBSTRING ${QM_BUILD_SHA} 0 12 QM_BUILD_SHA)
  message(STATUS "quantmodeling build stamp: ${QM_BUILD_SHA}")

  add_subdirectory(bindings/python)
endif()
