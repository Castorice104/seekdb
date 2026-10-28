#pragma once

#include "sql/resolver/ddl/ob_ai_column_info.h"
#include "sql/parser/parse_node.h"
#include "share/schema/ob_table_schema.h"
#include "lib/string/ob_sql_string.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase { namespace sql {

inline const char *ai_type_prompt(const common::ObObjType type)
{
  const char *prompt = NULL;
  if (common::ObJsonType == type) {
    prompt = "Return exactly one valid JSON value";
  } else if (common::ob_is_integer_type(type)) {
    prompt = "Return a base-10 integer";
  } else if (common::ObDoubleType == type || common::ObFloatType == type
             || common::ObNumberType == type || common::ObUNumberType == type
             || common::ObONumberFloatType == type || common::ObDecimalIntType == type) {
    prompt = "Return a number";
  } else if (common::ObDateType == type) {
    prompt = "Return a date in YYYY-MM-DD format";
  } else if (common::ObDateTimeType == type || common::ObTimestampType == type
             || common::ObMySQLDateTimeType == type) {
    prompt = "Return a datetime in YYYY-MM-DD HH:MM:SS format";
  } else if (common::ob_is_string_type(type) || common::ObTextType == type
             || common::ObLongTextType == type) {
    prompt = "Return plain text only";
  }
  return prompt;
}

inline int ai_build_normalized_expr(common::ObIAllocator &alloc,
                                    const common::ObString &expression, const char *prompt,
                                    common::ObString &normalized)
{
  int ret = common::OB_SUCCESS;
  if (NULL == prompt) {
    normalized = expression;
  } else {
    // The AST is already validated. Split the three outer arguments while
    // preserving each expression verbatim, including SQL strings and quoting.
    int64_t start = 0, depth = 0;
    char quote = 0;
    common::ObSEArray<common::ObString, 3> args;
    for (; start < expression.length() && expression.ptr()[start] != '('; ++start) {}
    int64_t arg_start = start + 1;
    for (int64_t i = arg_start; OB_SUCC(ret) && i < expression.length(); ++i) {
      const char c = expression.ptr()[i];
      if (quote) {
        if ('\\' == c) { ++i; }
        else if (quote == c) { quote = 0; }
      } else if ('\'' == c || '"' == c || '\x60' == c) { quote = c; }
      else if ('(' == c) { ++depth; }
      else if (')' == c && depth > 0) { --depth; }
      else if ((',' == c || ')' == c) && 0 == depth) {
        ret = args.push_back(common::ObString(i - arg_start, expression.ptr() + arg_start).trim());
        arg_start = i + 1;
        if (')' == c) { break; }
      }
    }
    common::ObSqlString out;
    if (OB_SUCC(ret) && (args.count() < 2 || args.count() > 3)) {
      ret = common::OB_ERR_PARSE_SQL;
    } else if (OB_SUCC(ret)) {
      if (OB_FAIL(out.append_fmt("AI_COMPLETE(%.*s,%.*s,", args.at(0).length(), args.at(0).ptr(),
                                args.at(1).length(), args.at(1).ptr()))) {
      } else if (2 == args.count()) {
        ret = out.append_fmt("'{\"system_prompt\":\"%s\"}')", prompt);
      } else {
        const common::ObString &config = args.at(2);
        ret = out.append_fmt(
            "JSON_SET(COALESCE(%.*s,JSON_OBJECT()),'$.system_prompt',"
            "CONCAT(COALESCE(JSON_UNQUOTE(JSON_EXTRACT(%.*s,'$.system_prompt')),''),' %s')))",
            config.length(), config.ptr(), config.length(), config.ptr(), prompt);
      }
      if (OB_SUCC(ret)) { ret = common::ob_write_string(alloc, out.string(), normalized); }
    }
  }
  return ret;
}

// Validate the parsed option list rather than silently ignoring misspelled keys.
inline int resolve_ai_column_options(const ParseNode *options, ObAiColumnInfo &info)
{
  int ret = common::OB_SUCCESS;
  bool seen_retry = false, seen_mode = false;
  info.max_retries_ = 0;
  info.sync_mode_ = 1;
  for (int64_t i = 0; OB_SUCC(ret) && NULL != options && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    if (NULL == option || 2 != option->num_child_ || NULL == option->children_[0]
        || NULL == option->children_[1]) {
      ret = common::OB_ERR_PARSE_SQL;
    } else {
      const ParseNode *key = option->children_[0], *value = option->children_[1];
      common::ObString name(key->str_len_, key->str_value_);
      if (0 == name.case_compare("max_retries")) {
        if (seen_retry || T_INT != value->type_ || value->value_ < 0) {
          ret = common::OB_ERR_PARSE_SQL;
        } else { seen_retry = true; info.max_retries_ = value->value_; }
      } else if (0 == name.case_compare("sync_mode")) {
        common::ObString mode(value->str_len_, value->str_value_);
        if (seen_mode || (T_VARCHAR != value->type_ && T_CHAR != value->type_ && T_IDENT != value->type_)) {
          ret = common::OB_ERR_PARSE_SQL;
        } else if (0 != mode.case_compare("async")) {
          ret = common::OB_NOT_SUPPORTED;
        } else { seen_mode = true; }
      } else {
        ret = common::OB_ERR_PARSE_SQL;
      }
    }
  }
  return ret;
}

inline bool ai_ast_function(const ParseNode *node, const char *name)
{
  return NULL != node && T_FUN_SYS == node->type_ && 2 == node->num_child_
      && NULL != node->children_[0]
      && 0 == common::ObString(node->children_[0]->str_len_, node->children_[0]->str_value_).case_compare(name);
}

inline int validate_ai_column_ast(const ParseNode *node, bool root = true, int depth = 0)
{
  int ret = common::OB_SUCCESS;
  if (NULL == node || depth > 128) {
    ret = common::OB_ERR_PARSE_SQL;
  } else if (root) {
    if (!ai_ast_function(node, "ai_complete") && !ai_ast_function(node, "ai_embed")) {
      ret = common::OB_NOT_SUPPORTED;
    } else if (NULL == node->children_[1]
               || node->children_[1]->num_child_ < 2
               || node->children_[1]->num_child_ > 3) {
      ret = common::OB_ERR_PARSE_SQL;
    }
  } else if (T_SELECT == node->type_ || T_REF_QUERY == node->type_ || T_OBJ_ACCESS_REF == node->type_
             || T_FUN_UDF == node->type_ || T_USER_VARIABLE_IDENTIFIER == node->type_
             || T_SYSTEM_VARIABLE == node->type_ || T_QUESTIONMARK == node->type_
             || ai_ast_function(node, "ai_complete")
             || ai_ast_function(node, "ai_embed")) {
    ret = common::OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && !root && T_FUN_SYS == node->type_) {
    // Background expressions must not gain stored-routine/FILE privileges or
    // change their inputs between retry attempts. Permit pure scalar builders.
    const char *pure_functions[] = {"concat", "concat_ws", "lower", "upper", "length",
      "char_length", "coalesce", "ifnull", "nullif", "abs", "round", "floor", "ceil",
      "substring", "substr", "left", "right", "trim", "ltrim", "rtrim", "replace",
      "json_object", "json_array", "json_extract", "json_unquote", "json_set",
      "json_contains", "json_length", "ai_prompt", "date", "date_format", "year", "month", "day"};
    bool pure = false;
    for (const char *name : pure_functions) { pure = pure || ai_ast_function(node, name); }
    if (!pure) { ret = common::OB_NOT_SUPPORTED; }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
    if (NULL != node->children_[i]) {
      ret = validate_ai_column_ast(node->children_[i], false, depth + 1);
    }
  }
  return ret;
}

inline int validate_ai_column_model(common::ObIAllocator &alloc,
                                     share::schema::ObSchemaGetterGuard &guard,
                                     const ParseNode *expr)
{
  int ret = common::OB_SUCCESS;
  if (OB_FAIL(validate_ai_column_ast(expr))) { return ret; }
  const ParseNode *args = expr->children_[1];
  const ParseNode *model = args->children_[0];
  common::ObAIFuncExprInfo *model_info = NULL;
  if (NULL == model || (T_VARCHAR != model->type_ && T_CHAR != model->type_)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    common::ObString name(model->str_len_, model->str_value_);
    if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, name, guard, model_info))) {
    } else if (NULL == model_info || (ai_ast_function(expr, "ai_complete")
               ? !ObAIFuncUtils::is_completion_type(model_info)
               : !ObAIFuncUtils::is_dense_embedding_type(model_info))) {
      ret = common::OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

// Bind only actual AST column-reference nodes. String literals and function names
// never create dependencies, and lookup takes place after every column is known.
inline int bind_ai_column_dependencies(const ParseNode *node,
                                      const share::schema::ObTableSchema &table,
                                      const uint64_t ai_column_id,
                                      common::ObIArray<uint64_t> &ids, int depth = 0)
{
  int ret = common::OB_SUCCESS;
  if (NULL == node) {
  } else if (depth > 128) {
    ret = common::OB_ERR_PARSE_SQL;
  } else if (T_COLUMN_REF == node->type_) {
    if (3 != node->num_child_ || NULL == node->children_[2]
        || NULL != node->children_[0] || NULL != node->children_[1]) {
      ret = common::OB_NOT_SUPPORTED;
    } else {
      const ParseNode *ident = node->children_[2];
      common::ObString name(ident->str_len_, ident->str_value_);
      const share::schema::ObColumnSchemaV2 *column = table.get_column_schema(name);
      if (NULL == column) {
        ret = common::OB_ERR_BAD_FIELD_ERROR;
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.length(), name.ptr(), 9, "AI COLUMN");
      } else if (ai_column_id == column->get_column_id() || column->is_ai_column()) {
        ret = common::OB_NOT_SUPPORTED;
      } else {
        bool found = false;
        for (int64_t i = 0; i < ids.count(); ++i) { found |= ids.at(i) == column->get_column_id(); }
        if (!found) { ret = ids.push_back(column->get_column_id()); }
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      ret = bind_ai_column_dependencies(node->children_[i], table, ai_column_id, ids, depth + 1);
    }
  }
  return ret;
}

inline bool ai_definition_depends_on(const ObAiColumnInfo &info, uint64_t column_id)
{
  const common::ObString &text = info.depends_columns_;
  uint64_t current = 0;
  bool digits = false;
  for (int64_t i = 0; i <= text.length(); ++i) {
    const char c = i == text.length() ? ',' : text.ptr()[i];
    if (',' == c) {
      if (digits && current == column_id) { return true; }
      current = 0; digits = false;
    } else if (c >= '0' && c <= '9') {
      current = current * 10 + c - '0'; digits = true;
    } else { return false; }
  }
  return false;
}

inline int finalize_ai_column_definition(common::ObIAllocator &alloc,
                                         share::schema::ObColumnSchemaV2 &column,
                                         const ParseNode *node,
                                         const share::schema::ObTableSchema &table,
                                         ObAiColumnInfo &info)
{
  int ret = common::OB_SUCCESS;
  common::ObSEArray<uint64_t, 4> ids;
  common::ObSqlString deps;
  if (OB_FAIL(bind_ai_column_dependencies(node, table, column.get_column_id(), ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
      ret = deps.append_fmt("%s%lu", i ? "," : "", ids.at(i));
    }
    if (OB_SUCC(ret)) { ret = common::ob_write_string(alloc, deps.string(), info.depends_columns_); }
    if (OB_SUCC(ret)) { ret = set_ai_column_info(column, alloc, info); }
  }
  return ret;
}

} }
