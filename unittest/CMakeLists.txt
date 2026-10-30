# Focused SQL AI regression tests; excluded from the default production build.
add_executable(ai_function_tests
  sql/engine/expr/test_ob_expr_ai_prompt.cpp
  sql/engine/expr/test_ob_openai_utils.cpp)
target_link_libraries(ai_function_tests PRIVATE
  oceanbase
  "${DEP_DIR}/lib/libgtest_main.a"
  "${DEP_DIR}/lib/libgtest.a"
  pthread)
add_test(NAME ai_function_tests COMMAND ai_function_tests)
set_tests_properties(ai_function_tests PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(ai_column_schema_tests sql/engine/expr/test_ai_column_schema.cpp)
target_link_libraries(ai_column_schema_tests PRIVATE oceanbase
  "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" pthread)
add_test(NAME ai_column_schema_tests COMMAND ai_column_schema_tests)

add_executable(fts_hot_path_tests storage/fts/test_fts_hot_path.cpp)
target_link_libraries(fts_hot_path_tests PRIVATE oceanbase
  "${DEP_DIR}/lib/libgtest_main.a" "${DEP_DIR}/lib/libgtest.a" pthread)
add_test(NAME fts_hot_path_tests COMMAND fts_hot_path_tests)

target_sources(fts_hot_path_tests PRIVATE storage/fts/test_fts_relevance_usage.cpp)

target_sources(fts_hot_path_tests PRIVATE sql/engine/sort/test_sort_string_hot_path.cpp)

add_executable(fts_parser_microbench storage/fts/fts_parser_microbench.cpp)
target_link_libraries(fts_parser_microbench PRIVATE oceanbase pthread)

target_sources(fts_hot_path_tests PRIVATE storage/fts/test_fts_index_row_lifetime.cpp)
