cmake_minimum_required(VERSION 3.24)

add_library(spark_stdlib
  src/math.cpp
  src/collection.cpp
)

target_include_directories(spark_stdlib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

