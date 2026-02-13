cmake_minimum_required(VERSION 3.20)

# pybind11 via vcpkg (CONFIG)
find_package(pybind11 CONFIG REQUIRED)

# Module Python (generates .so / .pyd importable)
pybind11_add_module(quantmodeling ${PROJECT_SOURCE_DIR}/src/module.cpp)

target_include_directories(quantmodeling PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(quantmodeling PRIVATE quantModeling)

# Optional
target_compile_features(quantmodeling PRIVATE cxx_std_20)

install(TARGETS quantmodeling
  LIBRARY DESTINATION .
  RUNTIME DESTINATION .
  ARCHIVE DESTINATION .
)
