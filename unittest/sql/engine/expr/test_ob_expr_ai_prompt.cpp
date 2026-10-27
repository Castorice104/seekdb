/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.h"
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
class AIPromptTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  void expect_format(const char *json, const std::string &expected)
  {
    ObString input = ObString::make_string(json);
    ObJsonObject *prompt = nullptr;
    ObString output;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, prompt));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator_, prompt, output));
    EXPECT_EQ(expected, std::string(output.ptr(), output.length()));
  }
};

TEST_F(AIPromptTest, accepts_lob_template_and_arguments_without_varchar_cast)
{
  ObExprAIPrompt op(allocator_);
  ObExprTypeCtx ctx;
  const ObObjType text_types[] = {ObTinyTextType, ObTextType, ObMediumTextType, ObLongTextType};
  for (const auto text_type : text_types) {
    ObExprResType args[3];
    args[0].set_type(text_type);
    args[1].set_type(text_type);
    args[2].set_json();
    ObExprResType result;
    ASSERT_EQ(OB_SUCCESS, op.calc_result_typeN(result, args, 3, ctx));
    EXPECT_EQ(ObJsonType, result.get_type());
    EXPECT_EQ(text_type, args[0].get_calc_type());
    EXPECT_EQ(text_type, args[1].get_calc_type());
    EXPECT_EQ(ObJsonType, args[2].get_calc_type());
  }
}

TEST_F(AIPromptTest, rejects_sql_null_and_non_text_arguments)
{
  ObExprAIPrompt op(allocator_);
  ObExprTypeCtx ctx;
  ObExprResType result;
  ObExprResType args[2];
  args[0].set_varchar();
  args[1].set_type(ObNullType);
  EXPECT_EQ(OB_ERR_INVALID_TYPE_FOR_OP, op.calc_result_typeN(result, args, 2, ctx));
  args[1].set_int();
  EXPECT_EQ(OB_ERR_INVALID_TYPE_FOR_OP, op.calc_result_typeN(result, args, 2, ctx));
}

TEST_F(AIPromptTest, completion_accepts_json_or_null_config_and_longtext_prompt)
{
  ObExprAIComplete op(allocator_);
  ObExprTypeCtx ctx;
  for (const auto config_type : {ObNullType, ObJsonType, ObLongTextType}) {
    ObExprResType args[3];
    args[0].set_varchar();
    args[1].set_type(ObLongTextType);
    args[1].set_calc_type(ObLongTextType);
    args[2].set_type(config_type);
    args[2].set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ObExprResType result;
    ASSERT_EQ(OB_SUCCESS, op.calc_result_typeN(result, args, 3, ctx));
    EXPECT_EQ(ObLongTextType, args[1].get_calc_type());
    EXPECT_EQ(ObLongTextType, result.get_type());
  }
}

TEST_F(AIPromptTest, repeated_placeholders_do_not_reinterpret_inserted_arguments)
{
  expect_format(R"({"template":"{0}/{0}/{1}","args":["{1}abcdefghij","end"]})",
                "{1}abcdefghij/{1}abcdefghij/end");
}

TEST_F(AIPromptTest, json_arguments_and_nested_prompt)
{
  // Compare serialized JSON with the same canonical serializer used by other AI functions.
  ObString json = ObString::make_string(R"({"template":"{0}/{1}/{2}","args":[[1,true],{"k":2},{"template":"nested {0}","args":["world"]}]})");
  ObJsonObject *prompt = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, json, prompt));
  auto *args = static_cast<ObJsonArray *>(prompt->get_value("args"));
  ObString first;
  ObString second;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, args->get_value(0), first));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, args->get_value(1), second));
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  EXPECT_EQ(std::string(first.ptr(), first.length()) + "/" + std::string(second.ptr(), second.length()) + "/nested world",
            std::string(output.ptr(), output.length()));
}

TEST_F(AIPromptTest, unicode_escaping_and_literal_braces)
{
  expect_format(R"({"template":"你好 {0} {name} {}","args":["世界\n\"\\"]})",
                "你好 世界\n\"\\ {name} {}");
}

TEST_F(AIPromptTest, invalid_numeric_indices_fail_without_integer_overflow)
{
  for (const char *text : {
      R"({"template":"{1}","args":["x"]})",
      R"({"template":"{-1}","args":["x"]})",
      R"({"template":"{999999999999999999999999999999}","args":["x"]})"}) {
    ObString json = ObString::make_string(text);
    ObJsonObject *prompt = nullptr;
    ObString output;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, json, prompt));
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator_, prompt, output));
  }
}

TEST_F(AIPromptTest, construction_preserves_unbound_placeholders_until_formatting)
{
  ObJsonArray *args = nullptr;
  ObJsonString *arg = nullptr;
  ObJsonObject *prompt = nullptr;
  ObString tmpl = ObString::make_string("{0}+{1}={2}");
  ObString value = ObString::make_string("1");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, value, arg));
  ASSERT_EQ(OB_SUCCESS, args->append(arg));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
      allocator_, tmpl, args, prompt));
  ASSERT_NE(nullptr, prompt);
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ObString output;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, prompt, output));
}

TEST_F(AIPromptTest, long_repeated_arguments_are_not_limited_to_varchar_size)
{
  std::string long_arg(100000, 'x');
  ObString arg(long_arg.size(), long_arg.data());
  ObString tmpl = ObString::make_string("{0}/{0}/{0}");
  ObJsonArray *args = nullptr;
  ObJsonString *value = nullptr;
  ObJsonObject *prompt = nullptr;
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, arg, value));
  ASSERT_EQ(OB_SUCCESS, args->append(value));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, tmpl, args, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  EXPECT_EQ(long_arg + "/" + long_arg + "/" + long_arg, std::string(output.ptr(), output.length()));
}
} // namespace
