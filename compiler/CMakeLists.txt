cmake_minimum_required(VERSION 3.24)

add_library(spark_compiler
  ${CMAKE_SOURCE_DIR}/src/core/common/ast.cpp
  ${CMAKE_SOURCE_DIR}/src/core/common/cpu_features.cpp
  ${CMAKE_SOURCE_DIR}/src/core/common/compiler_common.cpp
  ${CMAKE_SOURCE_DIR}/src/core/behavior/compiler_behavior.cpp
  ${CMAKE_SOURCE_DIR}/src/core/behavior/platform_support_behavior.cpp
  ${CMAKE_SOURCE_DIR}/src/core/mapper/compiler_mapper.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/parser.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/lexer.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/compiler_driver.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/platform_support_driver.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/dto_driver_registry.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/runtime/arena_allocator.cpp
  ${CMAKE_SOURCE_DIR}/src/core/driver/runtime/gc_runtime.cpp
  ${CMAKE_SOURCE_DIR}/src/core/mode/compiler_mode.cpp
  ${CMAKE_SOURCE_DIR}/src/core/mode/compiler_entry.cpp
  ${CMAKE_SOURCE_DIR}/src/core/ui/compiler_ui.cpp
  ${CMAKE_SOURCE_DIR}/src/core/ui/platform_support_ui.cpp
  ${CMAKE_SOURCE_DIR}/src/core/servis/codegen.cpp
  ${CMAKE_SOURCE_DIR}/src/core/servis/compiler_servis.cpp
  ${CMAKE_SOURCE_DIR}/src/core/servis/platform_support_servis.cpp
  ${CMAKE_SOURCE_DIR}/src/core/logic/runtime/math.cpp
  ${CMAKE_SOURCE_DIR}/src/core/logic/runtime/collection.cpp
  ${CMAKE_SOURCE_DIR}/src/core/logic/evaluator.cpp
  ${CMAKE_SOURCE_DIR}/src/core/logic/compiler_logic.cpp
  ${CMAKE_SOURCE_DIR}/src/core/logic/platform_support_logic.cpp
  ${CMAKE_SOURCE_DIR}/src/core/manager/semantic.cpp
  ${CMAKE_SOURCE_DIR}/src/core/manager/compiler_manager.cpp
  ${CMAKE_SOURCE_DIR}/src/core/manager/platform_support_manager.cpp
  ${CMAKE_SOURCE_DIR}/src/port/input/filesystem_source_input_port.cpp
  ${CMAKE_SOURCE_DIR}/src/port/output/stdout_diagnostic_output_port.cpp
  ${CMAKE_SOURCE_DIR}/src/application/wiring/compiler_wiring.cpp
)

target_include_directories(spark_compiler
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core/driver/parsing
    ${CMAKE_SOURCE_DIR}/src/core/manager/typecheck
    ${CMAKE_SOURCE_DIR}/src/core/servis/codegen
    ${CMAKE_SOURCE_DIR}/src/core/logic/runtime
    ${CMAKE_SOURCE_DIR}/src/core/logic/pipeline
    ${CMAKE_SOURCE_DIR}/src/core/logic/schedule
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

add_executable(sparkc ${CMAKE_SOURCE_DIR}/src/application/main.cpp)
target_include_directories(sparkc
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/frontend
)
target_link_libraries(sparkc PRIVATE spark_compiler)

if(SPARK_BUILD_TESTS)
  set(SPARK_TEST_SOURCE_DIR ${CMAKE_SOURCE_DIR}/test)
  find_package(Python3 COMPONENTS Interpreter)
  function(spark_enable_test_assertions TARGET_NAME)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${TARGET_NAME} PRIVATE -UNDEBUG)
    endif()
  endfunction()

  add_executable(sparkc_smoke_test ${SPARK_TEST_SOURCE_DIR}/core/pipeline/smoke/smoke_test.cpp)
  target_link_libraries(sparkc_smoke_test PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_smoke_test)
  add_test(NAME sparkc_smoke_test COMMAND sparkc_smoke_test)

  add_executable(sparkc_parser_tests ${SPARK_TEST_SOURCE_DIR}/core/pipeline/parsing/parser_tests.cpp)
  target_link_libraries(sparkc_parser_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_parser_tests)
  add_test(NAME sparkc_parser_tests COMMAND sparkc_parser_tests)

  add_executable(sparkc_eval_tests ${SPARK_TEST_SOURCE_DIR}/core/pipeline/semantic_runtime/eval_tests.cpp)
  target_link_libraries(sparkc_eval_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_eval_tests)
  add_test(NAME sparkc_eval_tests COMMAND sparkc_eval_tests)

  add_executable(sparkc_typecheck_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/semantic_runtime/typecheck_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/semantic_runtime/typecheck_support.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/semantic_runtime/typecheck_core_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/semantic_runtime/typecheck_tier_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/semantic_runtime/typecheck_inference_shape_tests.cpp
  )
  target_include_directories(sparkc_typecheck_tests PRIVATE ${SPARK_TEST_SOURCE_DIR})
  target_link_libraries(sparkc_typecheck_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_typecheck_tests)
  add_test(NAME sparkc_typecheck_tests COMMAND sparkc_typecheck_tests)

  add_executable(sparkc_codegen_tests ${SPARK_TEST_SOURCE_DIR}/core/pipeline/lowering/codegen_tests.cpp)
  target_link_libraries(sparkc_codegen_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_codegen_tests)
  add_test(NAME sparkc_codegen_tests COMMAND sparkc_codegen_tests)

  add_executable(sparkc_containers_primitives_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/list/list_container_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/list/list_container_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/matrix/matrix_container_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/matrix/matrix_container_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/primitives/primitive_numeric_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/primitives/primitive_numeric_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/containers_primitives_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/containers_primitives_support.cpp
  )
  target_include_directories(sparkc_containers_primitives_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives)
  target_link_libraries(sparkc_containers_primitives_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_containers_primitives_tests)
  add_test(NAME sparkc_containers_primitives_tests COMMAND sparkc_containers_primitives_tests)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME sparkc_containers_crosslang_primitives
      COMMAND ${Python3_EXECUTABLE} ${SPARK_TEST_SOURCE_DIR}/core/pipeline/containers_primitives/primitives/crosslang_native_primitives_tests.py --randomize-seed
    )
    set_tests_properties(sparkc_containers_crosslang_primitives PROPERTIES TIMEOUT 1800)
  endif()

  add_executable(sparkc_hetero_runtime_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime/list/hetero_list_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime/list/hetero_list_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime/matrix/hetero_matrix_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime/matrix/hetero_matrix_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime/hetero_runtime_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime/hetero_runtime_support.cpp
  )
  target_include_directories(sparkc_hetero_runtime_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/core/pipeline/hetero_runtime)
  target_link_libraries(sparkc_hetero_runtime_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_hetero_runtime_tests)
  add_test(NAME sparkc_hetero_runtime_tests COMMAND sparkc_hetero_runtime_tests)

  add_executable(sparkc_stream_pipeline_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/list/pipeline_list_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/list/pipeline_list_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/matrix/pipeline_matrix_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/matrix/pipeline_matrix_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/analyze/pipeline_analyze_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/stream_pipeline_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline/stream_pipeline_support.cpp
  )
  target_include_directories(sparkc_stream_pipeline_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/core/pipeline/stream_pipeline)
  target_link_libraries(sparkc_stream_pipeline_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_stream_pipeline_tests)
  add_test(NAME sparkc_stream_pipeline_tests COMMAND sparkc_stream_pipeline_tests)

  add_executable(sparkc_matmul_schedule_gpu_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/matmul_schedule_gpu/matmul/matmul_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/matmul_schedule_gpu/matmul/matmul_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/matmul_schedule_gpu/analyze/matmul_analyze_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/matmul_schedule_gpu/matmul_schedule_gpu_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/matmul_schedule_gpu/matmul_schedule_gpu_support.cpp
  )
  target_include_directories(sparkc_matmul_schedule_gpu_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/core/pipeline/matmul_schedule_gpu)
  target_link_libraries(sparkc_matmul_schedule_gpu_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_matmul_schedule_gpu_tests)
  add_test(NAME sparkc_matmul_schedule_gpu_tests COMMAND sparkc_matmul_schedule_gpu_tests)

  add_executable(sparkc_async_runtime_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/runtime/async_task_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/runtime/async_task_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/runtime/channel_stream_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/runtime/channel_stream_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/runtime/parallel_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/runtime/parallel_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/analyze/safety_diagnostics_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/async_runtime_main.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime/async_runtime_support.cpp
  )
  target_include_directories(sparkc_async_runtime_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                   ${SPARK_TEST_SOURCE_DIR}/core/pipeline/async_runtime)
  target_link_libraries(sparkc_async_runtime_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_async_runtime_tests)
  add_test(NAME sparkc_async_runtime_tests COMMAND sparkc_async_runtime_tests)

  add_executable(sparkc_platform_support_tests
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/platform_support/dispatch/cpu_dispatch_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/platform_support/dispatch/cpu_dispatch_extreme_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/platform_support/architecture/core_architecture_tests.cpp
    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/platform_support/platform_support_main.cpp
  )
  target_include_directories(sparkc_platform_support_tests PRIVATE ${SPARK_TEST_SOURCE_DIR}
                                                    ${SPARK_TEST_SOURCE_DIR}/core/pipeline/platform_support)
  target_link_libraries(sparkc_platform_support_tests PRIVATE spark_compiler)
  spark_enable_test_assertions(sparkc_platform_support_tests)
  add_test(NAME sparkc_platform_support_tests COMMAND sparkc_platform_support_tests)
endif()
