cmake_minimum_required(VERSION 3.24)

project(codex_spark VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPARK_BUILD_TESTS "Build test targets" OFF)
option(SPARK_BUILD_BENCHMARKS "Build benchmark targets" OFF)

if(SPARK_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(compiler)
add_subdirectory(runtime)
add_subdirectory(stdlib)
add_subdirectory(tests)
add_subdirectory(bench)
