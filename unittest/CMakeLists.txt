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
