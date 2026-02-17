cmake_minimum_required(VERSION 3.24)

add_library(spark_runtime
  src/arena_allocator.cpp
  src/gc_runtime.cpp
)

target_include_directories(spark_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

