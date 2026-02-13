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
        src/engines/mc/black_scholes.cpp
        src/engines/mc/asian.cpp
        src/instruments/equity/vanilla.cpp
        src/instruments/equity/asian.cpp
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
