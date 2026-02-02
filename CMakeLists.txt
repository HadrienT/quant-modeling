cmake_minimum_required(VERSION 3.16)

project(quantModeling
    VERSION 0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(quantModeling)

target_sources(quantModeling
    PRIVATE
        src/utils/stats.cpp
        src/engines/base.cpp
)

target_include_directories(quantModeling
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Eigen3 CONFIG REQUIRED)
target_link_libraries(quantModeling PUBLIC Eigen3::Eigen)


add_executable(quantModeling_cli
    main.cpp
)

target_link_libraries(quantModeling_cli
    PRIVATE
        quantModeling
)
