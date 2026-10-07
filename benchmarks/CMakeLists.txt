add_executable(qtzpl_benchmarks
  main.cpp
  algorithm_cases.cpp
  pipeline_cases.cpp
  component_cases.cpp
)
target_link_libraries(qtzpl_benchmarks PRIVATE QtZpl::QtZpl)
target_include_directories(qtzpl_benchmarks PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(qtzpl_benchmarks PRIVATE
  QTZPL_BENCH_CORPUS_DIR="${PROJECT_SOURCE_DIR}/tests/corpus/go-zpl-demo"
  QTZPL_BENCH_FONT_FILE="${PROJECT_SOURCE_DIR}/resources/fonts/font0.ttf"
  QTZPL_BENCH_BUILD_TYPE="$<CONFIG>"
  QTZPL_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  QTZPL_BENCH_SHARED=$<BOOL:${QTZPL_BUILD_SHARED}>
)
# Timing is deliberately opt-in, never a pass/fail CTest performance threshold.
