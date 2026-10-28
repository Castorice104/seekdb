#include <gtest/gtest.h>
#include "sql/resolver/ddl/ob_ai_column_info.h"
#include "sql/resolver/ddl/ob_ai_column_resolver_utils.h"
#include "sql/parser/ob_parser.h"
#include "lib/allocator/page_arena.h"
using namespace oceanbase;
TEST(AiColumnSchema, preserves_definition_and_user_comment)
{
  common::ObArenaAllocator alloc;
  share::schema::ObColumnSchemaV2 column, restored;
  sql::ObAiColumnInfo info, decoded;
  ASSERT_EQ(common::OB_SUCCESS, column.set_column_name("answer"));
  column.set_column_id(19);
  ASSERT_EQ(common::OB_SUCCESS, column.set_comment("business comment"));
  info.column_id_ = 19;
  info.column_name_ = common::ObString::make_string("answer");
  info.ai_type_ = sql::ObAiColumnInfo::AI_COLUMN_COMPLETE;
  info.result_type_ = common::ObVarcharType;
  info.expr_text_ = common::ObString::make_string("AI_COMPLETE('model', content)");
  info.normalized_expr_ = info.expr_text_;
  info.depends_columns_ = common::ObString::make_string("18");
  info.state_column_id_ = 20;
  info.request_column_id_ = 21;
  info.result_spec_column_id_ = 22;
  info.definition_version_ = 7;
  ASSERT_EQ(common::OB_SUCCESS, sql::set_ai_column_info(column, alloc, info));
  ASSERT_TRUE(column.is_ai_column());
  ASSERT_EQ(common::OB_SUCCESS, restored.assign(column));
  ASSERT_EQ(common::OB_SUCCESS, sql::get_ai_column_info(restored, alloc, decoded));
  EXPECT_EQ(7, decoded.definition_version_);
  EXPECT_EQ(20, decoded.state_column_id_);
  EXPECT_EQ(info.depends_columns_, decoded.depends_columns_);
  EXPECT_EQ(common::ObString::make_string("business comment"), restored.get_comment_str());
  char buf[8192]; int64_t pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, column.serialize(buf, sizeof(buf), pos));
  restored.reset(); int64_t read_pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, restored.deserialize(buf, pos, read_pos));
  ASSERT_EQ(common::OB_SUCCESS, sql::get_ai_column_info(restored, alloc, decoded));
  EXPECT_EQ(7, decoded.definition_version_);
}

namespace {
const ParseNode *find_ai_node(const ParseNode *node) {
  if (NULL == node || T_AI_COLUMN_INFO == node->type_) { return node; }
  for (int i = 0; i < node->num_child_; ++i) {
    const ParseNode *found = find_ai_node(node->children_[i]);
    if (NULL != found) { return found; }
  }
  return NULL;
}
}
TEST(AiColumnSchema, validates_real_parser_options_and_expression)
{
  const char *statements[] = {
    "create table t(content varchar(10), a varchar(10) AI COLUMN (AI_COMPLETE('m', content)) WITH (max_retries=0))",
    "create table t(content varchar(10), a varchar(10) AI COLUMN (AI_COMPLETE('m', content)) WITH (max_retries=0,max_retries=1))",
    "create table t(content varchar(10), a varchar(10) AI COLUMN (AI_COMPLETE('m', content)) WITH (typo=1))",
    "create table t(content varchar(10), a varchar(10) AI COLUMN (AI_COMPLETE('m', content)+1))"
  };
  for (int i = 0; i < 4; ++i) {
    common::ObArenaAllocator alloc;
    sql::ObParser parser(alloc, DEFAULT_MYSQL_MODE);
    ParseResult parsed;
    ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString::make_string(statements[i]), parsed));
    const ParseNode *ai = find_ai_node(parsed.result_tree_);
    ASSERT_NE(nullptr, ai);
    sql::ObAiColumnInfo info;
    if (i < 3) {
      EXPECT_EQ(i == 0 ? common::OB_SUCCESS : common::OB_ERR_PARSE_SQL,
                sql::resolve_ai_column_options(ai->children_[1], info));
    }
    EXPECT_EQ(i == 3 ? common::OB_NOT_SUPPORTED : common::OB_SUCCESS,
              sql::validate_ai_column_ast(ai->children_[0]));
    parser.free_result(parsed);
  }
}
TEST(AiColumnSchema, ast_dependencies_ignore_literals_and_bind_forward_columns)
{
  common::ObArenaAllocator alloc;
  sql::ObParser parser(alloc, DEFAULT_MYSQL_MODE);
  ParseResult parsed;
  ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString::make_string(
      "create table t(a varchar(10) AI COLUMN (AI_COMPLETE('m', concat('a', `Later`))), `Later` varchar(10))"), parsed));
  const ParseNode *ai = find_ai_node(parsed.result_tree_);
  ASSERT_NE(nullptr, ai);
  share::schema::ObTableSchema table;
  table.set_table_id(1234);
  table.set_table_type(share::schema::USER_TABLE);
  table.set_charset_type(common::CHARSET_UTF8MB4);
  table.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
  share::schema::ObColumnSchemaV2 a, later;
  a.set_table_id(1234); later.set_table_id(1234);
  a.set_data_type(common::ObVarcharType); later.set_data_type(common::ObVarcharType);
  a.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
  later.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
  common::ObAccuracy text_accuracy;
  text_accuracy.set_length(10);
  a.set_accuracy(text_accuracy); later.set_accuracy(text_accuracy);
  a.set_column_id(16); a.set_column_name("a");
  later.set_column_id(17); later.set_column_name("Later");
  ASSERT_EQ(common::OB_SUCCESS, table.add_column(a));
  ASSERT_EQ(common::OB_SUCCESS, table.add_column(later));
  common::ObSEArray<uint64_t, 4> ids;
  ASSERT_EQ(common::OB_SUCCESS, sql::bind_ai_column_dependencies(ai->children_[0], table, 16, ids));
  ASSERT_EQ(1, ids.count());
  EXPECT_EQ(17, ids.at(0));
  sql::ObAiColumnInfo info;
  ASSERT_EQ(common::OB_SUCCESS, sql::set_ai_column_info(*table.get_column_schema(17), alloc, info));
  ids.reset();
  EXPECT_EQ(common::OB_NOT_SUPPORTED, sql::bind_ai_column_dependencies(ai->children_[0], table, 16, ids));
  parser.free_result(parsed);
}

TEST(AiColumnSchema, preserves_completion_config_and_adds_separate_type_prompt)
{
  common::ObArenaAllocator alloc;
  common::ObString normalized;
  const common::ObString expression = common::ObString::make_string(
      "AI_COMPLETE('m',concat('business, input', content),'{\"temperature\":0.2,\"system_prompt\":\"original\"}')");
  ASSERT_EQ(common::OB_SUCCESS, sql::ai_build_normalized_expr(alloc, expression,
      sql::ai_type_prompt(common::ObJsonType), normalized));
  std::string result(normalized.ptr(), normalized.length());
  EXPECT_NE(std::string::npos, result.find("business, input"));
  EXPECT_NE(std::string::npos, result.find("\"temperature\":0.2"));
  EXPECT_NE(std::string::npos, result.find("Return exactly one valid JSON value"));
  EXPECT_NE(std::string::npos, result.find("JSON_SET"));
}
TEST(AiColumnSchema, default_options_allow_zero_retries)
{
  sql::ObAiColumnInfo info;
  info.max_retries_ = 99;
  ASSERT_EQ(common::OB_SUCCESS, sql::resolve_ai_column_options(nullptr, info));
  EXPECT_EQ(0, info.max_retries_);
  EXPECT_EQ(1, info.sync_mode_);
}

TEST(AiColumnSchema, parses_show_ai_columns_as_native_show_statement)
{
  common::ObArenaAllocator alloc;
  sql::ObParser parser(alloc, DEFAULT_MYSQL_MODE);
  ParseResult parsed;
  ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString::make_string(
      "SHOW AI COLUMNS FROM docs FROM test WHERE max_retries > 0"), parsed));
  ASSERT_NE(nullptr, parsed.result_tree_);
  ASSERT_EQ(1, parsed.result_tree_->num_child_);
  const ParseNode *show = parsed.result_tree_->children_[0];
  ASSERT_NE(nullptr, show);
  EXPECT_EQ(T_SHOW_AI_COLUMNS, show->type_);
  ASSERT_EQ(3, show->num_child_);
  EXPECT_EQ(T_RELATION_FACTOR, show->children_[0]->type_);
  EXPECT_NE(nullptr, show->children_[2]);
  parser.free_result(parsed);
}
