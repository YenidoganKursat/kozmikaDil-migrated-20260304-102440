cmake_minimum_required(VERSION 3.24)

add_library(spark_compiler
  src/common/ast.cpp
  src/common/cpu_features.cpp
  src/phase2/parser.cpp
  src/phase2/lexer.cpp
  src/phase4/codegen.cpp
  src/phase3/evaluator.cpp
  src/phase3/semantic.cpp
)

target_include_directories(spark_compiler
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Optional high-precision numeric backend (f128/f256/f512) via MPFR/GMP.
find_path(SPARK_MPFR_INCLUDE_DIR mpfr.h)
find_library(SPARK_MPFR_LIBRARY mpfr)
find_library(SPARK_GMP_LIBRARY gmp)
if(SPARK_MPFR_INCLUDE_DIR AND SPARK_MPFR_LIBRARY AND SPARK_GMP_LIBRARY)
  target_compile_definitions(spark_compiler PRIVATE SPARK_HAS_MPFR=1)
  target_include_directories(spark_compiler PRIVATE ${SPARK_MPFR_INCLUDE_DIR})
  target_link_libraries(spark_compiler PRIVATE ${SPARK_MPFR_LIBRARY} ${SPARK_GMP_LIBRARY})
endif()

add_executable(sparkc src/phase1/sparkc_main.cpp)
target_include_directories(sparkc
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
)
target_link_libraries(sparkc PRIVATE spark_compiler spark_runtime spark_stdlib)

if(SPARK_BUILD_TESTS)
  set(SPARK_TEST_SOURCE_DIR ${CMAKE_SOURCE_DIR}/tests)
  function(spark_enable_test_assertions TARGET_NAME)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${TARGET_NAME} PRIVATE -UNDEBUG)
    endif()
  endfunction()

  add_executable(sparkc_smoke_test ${SPARK_TEST_SOURCE_DIR}/phase1/smoke_test.cpp)
  target_link_libraries(sparkc_smoke_test PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_smoke_test)
  add_test(NAME sparkc_smoke_test COMMAND sparkc_smoke_test)

  add_executable(sparkc_parser_tests ${SPARK_TEST_SOURCE_DIR}/phase2/parser_tests.cpp)
  target_link_libraries(sparkc_parser_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_parser_tests)
  add_test(NAME sparkc_parser_tests COMMAND sparkc_parser_tests)

  add_executable(sparkc_eval_tests ${SPARK_TEST_SOURCE_DIR}/phase3/eval_tests.cpp)
  target_link_libraries(sparkc_eval_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_eval_tests)
  add_test(NAME sparkc_eval_tests COMMAND sparkc_eval_tests)

  add_executable(sparkc_typecheck_tests
    ${SPARK_TEST_SOURCE_DIR}/phase3/typecheck_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase3/typecheck_support.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase3/typecheck_core_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase3/typecheck_tier_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase3/typecheck_inference_shape_tests.cpp
  )
  target_include_directories(sparkc_typecheck_tests PRIVATE ${SPARK_TEST_SOURCE_DIR})
  target_link_libraries(sparkc_typecheck_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_typecheck_tests)
  add_test(NAME sparkc_typecheck_tests COMMAND sparkc_typecheck_tests)

  add_executable(sparkc_codegen_tests ${SPARK_TEST_SOURCE_DIR}/phase4/codegen_tests.cpp)
  target_link_libraries(sparkc_codegen_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_codegen_tests)
  add_test(NAME sparkc_codegen_tests COMMAND sparkc_codegen_tests)

  add_executable(sparkc_phase5_tests
    ${SPARK_TEST_SOURCE_DIR}/phase5/list/list_container_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase5/matrix/matrix_container_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase5/primitives/primitive_numeric_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase5/phase5_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase5/phase5_support.cpp
  )
  target_include_directories(sparkc_phase5_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/phase5)
  target_link_libraries(sparkc_phase5_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_phase5_tests)
  add_test(NAME sparkc_phase5_tests COMMAND sparkc_phase5_tests)

  add_executable(sparkc_phase6_tests
    ${SPARK_TEST_SOURCE_DIR}/phase6/list/phase6_list_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase6/matrix/phase6_matrix_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase6/phase6_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase6/phase6_support.cpp
  )
  target_include_directories(sparkc_phase6_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/phase6)
  target_link_libraries(sparkc_phase6_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_phase6_tests)
  add_test(NAME sparkc_phase6_tests COMMAND sparkc_phase6_tests)

  add_executable(sparkc_phase7_tests
    ${SPARK_TEST_SOURCE_DIR}/phase7/list/pipeline_list_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase7/matrix/pipeline_matrix_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase7/analyze/pipeline_analyze_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase7/phase7_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase7/phase7_support.cpp
  )
  target_include_directories(sparkc_phase7_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/phase7)
  target_link_libraries(sparkc_phase7_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_phase7_tests)
  add_test(NAME sparkc_phase7_tests COMMAND sparkc_phase7_tests)

  add_executable(sparkc_phase8_tests
    ${SPARK_TEST_SOURCE_DIR}/phase8/matmul/matmul_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase8/analyze/matmul_analyze_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase8/phase8_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase8/phase8_support.cpp
  )
  target_include_directories(sparkc_phase8_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/phase8)
  target_link_libraries(sparkc_phase8_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_phase8_tests)
  add_test(NAME sparkc_phase8_tests COMMAND sparkc_phase8_tests)

  add_executable(sparkc_phase9_tests
    ${SPARK_TEST_SOURCE_DIR}/phase9/runtime/async_task_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase9/runtime/channel_stream_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase9/runtime/parallel_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase9/analyze/safety_diagnostics_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase9/phase9_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase9/phase9_support.cpp
  )
  target_include_directories(sparkc_phase9_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/phase9)
  target_link_libraries(sparkc_phase9_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_phase9_tests)
  add_test(NAME sparkc_phase9_tests COMMAND sparkc_phase9_tests)

  add_executable(sparkc_phase10_tests
    ${SPARK_TEST_SOURCE_DIR}/phase10/dispatch/cpu_dispatch_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/phase10/phase10_main.cpp
  )
  target_include_directories(sparkc_phase10_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                    ${SPARK_TEST_SOURCE_DIR}/phase10)
  target_link_libraries(sparkc_phase10_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_phase10_tests)
  add_test(NAME sparkc_phase10_tests COMMAND sparkc_phase10_tests)
endif()
