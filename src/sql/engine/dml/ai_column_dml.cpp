/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"

namespace oceanbase {
using namespace common;
using namespace share::schema;
namespace sql {

int ai_column_append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if ('`' == name.ptr()[i]) {
      ret = sql.append("``");
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int ai_column_append_literal(ObSqlString &sql, const ObString &value)
{
  // Hex is independent of NO_BACKSLASH_ESCAPES and safely carries SQL source,
  // embedded NULs, quotes and non-ASCII identifiers through a nested statement.
  static const char hex[] = "0123456789abcdef";
  int ret = sql.append("CONVERT(X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    char pair[2] = {hex[c >> 4], hex[c & 15]};
    ret = sql.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("' USING utf8mb4)"); }
  return ret;
}

static int ai_qualified_name(ObSqlString &sql, const ObString &db, const ObString &table)
{
  int ret = ai_column_append_identifier(sql, db);
  if (OB_SUCC(ret)) { ret = sql.append("."); }
  if (OB_SUCC(ret)) { ret = ai_column_append_identifier(sql, table); }
  return ret;
}

int ai_column_build_task_sql(const ObTableSchema &table, const ObString &database_name,
                            const ObAiColumnInfo &info, const ObString &predicate,
                            const ObString &request_id, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *state = table.get_column_schema(info.state_column_id_);
  const ObColumnSchemaV2 *request = table.get_column_schema(info.request_column_id_);
  const ObColumnSchemaV2 *column = table.get_column_schema(info.column_id_);
  ObSqlString execute;
  if (OB_ISNULL(state) || OB_ISNULL(request) || OB_ISNULL(column)) {
    ret = OB_SCHEMA_ERROR;
  } else if (OB_FAIL(execute.append("SELECT "))) {
  } else if (OB_FAIL(execute.append(info.normalized_expr_))) {
  } else if (OB_FAIL(execute.append(" FROM "))) {
  } else if (OB_FAIL(ai_qualified_name(execute, database_name, table.get_table_name_str()))) {
  } else if (OB_FAIL(execute.append(" WHERE "))) {
  } else if (OB_FAIL(execute.append(predicate))) {
  } else if (OB_FAIL(execute.append(" AND "))) {
  } else if (OB_FAIL(ai_column_append_identifier(execute, request->get_column_name_str()))) {
  } else if (OB_FAIL(execute.append(" = "))) {
  } else if (OB_FAIL(ai_column_append_literal(execute, request_id))) {
  } else if (OB_FAIL(sql.append("INSERT INTO oceanbase.__all_ai_column_task("
      "request_id,column_id,table_id,rowkey_predicate,state,retry_count,eligible_ts,definition_version,"
      "database_name,table_name,column_name,state_column_name,request_column_name,execute_sql,max_retries) VALUES("))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, request_id))) {
  } else if (OB_FAIL(sql.append_fmt(",%lu,%lu,", info.column_id_, table.get_table_id()))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, predicate))) {
  } else if (OB_FAIL(sql.append_fmt(",0,0,NOW(6),%ld,", info.definition_version_))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, database_name))) {
  } else if (OB_FAIL(sql.append(","))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, table.get_table_name_str()))) {
  } else if (OB_FAIL(sql.append(","))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, column->get_column_name_str()))) {
  } else if (OB_FAIL(sql.append(","))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, state->get_column_name_str()))) {
  } else if (OB_FAIL(sql.append(","))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, request->get_column_name_str()))) {
  } else if (OB_FAIL(sql.append(","))) {
  } else if (OB_FAIL(ai_column_append_literal(sql, ObString(execute.length(), execute.ptr())))) {
  } else if (OB_FAIL(sql.append_fmt(",%ld)", info.max_retries_))) {
  }
  return ret;
}

static int ai_row_obj(const ExprFixedArray &row, int64_t index, ObEvalCtx &eval, ObObj &obj)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;
  if (index < 0 || index >= row.count() || OB_ISNULL(row.at(index))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(row.at(index)->eval(eval, datum))) {
  } else if (OB_ISNULL(datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = datum->to_obj(obj, row.at(index)->obj_meta_);
  }
  return ret;
}

static int ai_predicate(const ObTableSchema &table, const ObDMLBaseCtDef &ctdef,
                        const ExprFixedArray &row, ObEvalCtx &eval, ObIAllocator &alloc,
                        const ObObjPrintParams &params, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  bool first = true;
  for (int64_t i = 0; OB_SUCC(ret) && i < ctdef.column_ids_.count(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(ctdef.column_ids_.at(i));
    if (nullptr == column || !column->is_rowkey_column()) { continue; }
    ObObj value;
    char *buffer = nullptr;
    int64_t length = 0, pos = 0;
    if (OB_FAIL(ai_row_obj(row, i, eval, value))) {
    } else if (OB_FAIL(sql.append(first ? "" : " AND "))) {
    } else if (OB_FAIL(ai_column_append_identifier(sql, column->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (value.is_string_type()) {
      ret = ai_column_append_literal(sql, value.get_string());
    } else if (OB_FAIL(value.print_sql_literal(buffer, length, pos, alloc, params))) {
    } else {
      ret = sql.append(buffer, pos);
    }
    first = false;
  }
  if (OB_SUCC(ret) && first) { ret = OB_ERR_UNEXPECTED; }
  return ret;
}

static bool ai_has_dependency(const ObString &deps, uint64_t column_id)
{
  int64_t pos = 0;
  while (pos < deps.length()) {
    uint64_t id = 0;
    bool found = false;
    while (pos < deps.length() && deps.ptr()[pos] >= '0' && deps.ptr()[pos] <= '9') {
      found = true;
      id = id * 10 + deps.ptr()[pos++] - '0';
    }
    if (found && id == column_id) { return true; }
    ++pos;
  }
  return false;
}

int ObTableModifyOp::queue_ai_column_sql(const ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObString copy;
  if (OB_FAIL(ob_write_string(ctx_.get_allocator(), ObString(sql.length(), sql.ptr()), copy))) {
  } else {
    ret = ai_column_sql_.push_back(copy);
  }
  return ret;
}

int ObTableModifyOp::queue_ai_column_change(const ObDMLBaseCtDef &ctdef, uint64_t table_id,
                                          const ExprFixedArray *old_row, const ExprFixedArray *new_row)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSchemaGetterGuard guard;
  ObSEArray<ObAiColumnInfo, 4> affected;
  bool has_ai = false;
  if (!ctdef.is_primary_index_ || OB_ISNULL(session) || !session->is_user_session()
      || session->is_ai_column_internal()) {
    return OB_SUCCESS;
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
  } else if (OB_ISNULL(table) || table->is_sys_table()) {
    return OB_SUCCESS;
  } else {
    for (int64_t i = 0; !has_ai && i < table->get_column_count(); ++i) {
      const ObColumnSchemaV2 *col = table->get_column_schema_by_idx(i);
      has_ai = nullptr != col && col->is_ai_column();
    }
  }
  if (OB_FAIL(ret) || !has_ai) { return ret; }
  if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
  } else if (OB_ISNULL(database)) {
    ret = OB_SCHEMA_ERROR;
  }
  ObSqlString old_key, new_key;
  if (OB_SUCC(ret) && nullptr != old_row) {
    ret = ai_predicate(*table, ctdef, *old_row, eval_ctx_, ctx_.get_allocator(), obj_print_params_, old_key);
  }
  if (OB_SUCC(ret) && nullptr != new_row) {
    ret = ai_predicate(*table, ctdef, *new_row, eval_ctx_, ctx_.get_allocator(), obj_print_params_, new_key);
  }
  const bool pk_changed = nullptr != old_row && nullptr != new_row && old_key.string() != new_key.string();
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (nullptr == column || !column->is_ai_column()) { continue; }
    ObAiColumnInfo info;
    bool changed = nullptr == old_row || nullptr == new_row || pk_changed;
    if (OB_FAIL(get_ai_column_info(*column, ctx_.get_allocator(), info))) { break; }
    for (int64_t k = 0; OB_SUCC(ret) && !changed && k < ctdef.column_ids_.count(); ++k) {
      if (ai_has_dependency(info.depends_columns_, ctdef.column_ids_.at(k))) {
        ObObj before, after;
        int cmp = 0;
        if (OB_FAIL(ai_row_obj(*old_row, k, eval_ctx_, before))) {
        } else if (OB_FAIL(ai_row_obj(*new_row, k, eval_ctx_, after))) {
        } else if (OB_FAIL(before.compare(after, CS_TYPE_BINARY, cmp))) {
        } else { changed = 0 != cmp; }
      }
    }
    if (OB_SUCC(ret) && changed) { ret = affected.push_back(info); }
  }
  if (OB_FAIL(ret) || affected.empty()) { return ret; }

  // Delete precisely the superseded columns. Other members of an old group
  // retain their tasks and may complete after a target-only RETRY/update.
  ObSqlString remove;
  const ObSqlString &key = nullptr != old_row ? old_key : new_key;
  if (OB_FAIL(remove.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=", table_id))) {
  } else if (OB_FAIL(ai_column_append_literal(remove, key.string()))) {
  } else if (OB_FAIL(remove.append(" AND column_id IN ("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
    ret = remove.append_fmt("%s%lu", i ? "," : "", affected.at(i).column_id_);
  }
  if (OB_SUCC(ret) && OB_FAIL(remove.append(")"))) {
  } else if (OB_SUCC(ret)) { ret = queue_ai_column_sql(remove); }
  if (OB_FAIL(ret) || nullptr == new_row) { return ret; }

  char uuid[37] = {};
  if (OB_FAIL(ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID))) { return ret; }
  const ObString request_id = ObString::make_string(uuid);
  ObSqlString invalidate;
  if (OB_FAIL(invalidate.append("UPDATE "))) {
  } else if (OB_FAIL(ai_qualified_name(invalidate, database->get_database_name_str(), table->get_table_name_str()))) {
  } else if (OB_FAIL(invalidate.append(" SET "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
    const ObAiColumnInfo &info = affected.at(i);
    const ObColumnSchemaV2 *state = table->get_column_schema(info.state_column_id_);
    const ObColumnSchemaV2 *request = table->get_column_schema(info.request_column_id_);
    if (OB_ISNULL(state) || OB_ISNULL(request)) { ret = OB_SCHEMA_ERROR; }
    else if (OB_FAIL(invalidate.append(i ? "," : ""))) {
    } else if (OB_FAIL(ai_column_append_identifier(invalidate, info.column_name_))) {
    } else if (OB_FAIL(invalidate.append("=NULL,"))) {
    } else if (OB_FAIL(ai_column_append_identifier(invalidate, state->get_column_name_str()))) {
    } else if (OB_FAIL(invalidate.append("=0,"))) {
    } else if (OB_FAIL(ai_column_append_identifier(invalidate, request->get_column_name_str()))) {
    } else if (OB_FAIL(invalidate.append("="))) {
    } else { ret = ai_column_append_literal(invalidate, request_id); }
  }
  if (OB_SUCC(ret) && OB_FAIL(invalidate.append(" WHERE "))) {
  } else if (OB_SUCC(ret) && OB_FAIL(invalidate.append(new_key.string()))) {
  } else if (OB_SUCC(ret)) { ret = queue_ai_column_sql(invalidate); }
  for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
    ObSqlString task;
    if (OB_FAIL(ai_column_build_task_sql(*table, database->get_database_name_str(), affected.at(i),
                                       new_key.string(), request_id, task))) {
    } else { ret = queue_ai_column_sql(task); }
  }
  return ret;
}

int ObTableModifyOp::flush_ai_column_changes()
{
  int ret = OB_SUCCESS;
  if (ai_column_sql_.empty()) { return ret; }
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) { return OB_ERR_UNEXPECTED; }
  const bool previous = session->is_ai_column_internal();
  session->set_ai_column_internal(true);
  bool begun = false;
  if (OB_FAIL(open_inner_conn())) {
  } else if (OB_FAIL(begin_nested_session(true))) {
  } else {
    begun = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_column_sql_.count(); ++i) {
      int64_t affected = 0;
      ret = inner_conn_->execute_write(ai_column_sql_.at(i), affected, false);
      if (OB_FAIL(ret)) { LOG_WARN("AI column transactional maintenance failed", K(ret), K(i)); }
    }
  }
  if (begun) {
    int end_ret = end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  session->set_ai_column_internal(previous);
  ai_column_sql_.reuse();
  return ret;
}

// Only an unquoted STATUS token has retry pseudo-column semantics. Preserve
// strings, backtick identifiers and comments verbatim before native SQL binding.
static int ai_retry_predicate(const ObString &source, const ObString &state_name, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  for (int64_t p = 0; OB_SUCC(ret) && p < source.length();) {
    const char c = source.ptr()[p];
    const int64_t start = p;
    if ('\'' == c || '"' == c || '`' == c) {
      ++p;
      while (p < source.length()) {
        if ('\\' == source.ptr()[p] && p + 1 < source.length()) { p += 2; }
        else if (c == source.ptr()[p++]) {
          if (p < source.length() && c == source.ptr()[p]) { ++p; }
          else { break; }
        }
      }
      ret = out.append(source.ptr() + start, p - start);
    } else if ('#' == c || ('-' == c && p + 2 < source.length()
                            && '-' == source.ptr()[p + 1] && isspace(source.ptr()[p + 2]))) {
      while (p < source.length() && '\n' != source.ptr()[p]) { ++p; }
      ret = out.append(source.ptr() + start, p - start);
    } else if ('/' == c && p + 1 < source.length() && '*' == source.ptr()[p + 1]) {
      p += 2;
      while (p + 1 < source.length() && !('*' == source.ptr()[p] && '/' == source.ptr()[p + 1])) { ++p; }
      p = std::min(p + 2, static_cast<int64_t>(source.length()));
      ret = out.append(source.ptr() + start, p - start);
    } else if (isalnum(static_cast<unsigned char>(c)) || '_' == c || '$' == c || static_cast<unsigned char>(c) >= 128) {
      while (p < source.length()) {
        const unsigned char ch = static_cast<unsigned char>(source.ptr()[p]);
        if (!(isalnum(ch) || '_' == ch || '$' == ch || ch >= 128)) { break; }
        ++p;
      }
      const ObString token(p - start, source.ptr() + start);
      if (0 == token.case_compare("status")) {
        if (OB_FAIL(out.append("(CASE "))) {
        } else if (OB_FAIL(ai_column_append_identifier(out, state_name))) {
        } else { ret = out.append(" WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' ELSE 'PENDING' END)"); }
      } else { ret = out.append(token); }
    } else {
      ret = out.append(source.ptr() + p++, 1);
    }
  }
  return ret;
}

int ai_column_regenerate_column(ObExecContext &ctx, uint64_t table_id,
                               uint64_t column_id, const ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  const ObColumnSchemaV2 *column = nullptr, *state = nullptr, *request = nullptr;
  ObAiColumnInfo info;
  ObArenaAllocator allocator("AiColumnRetry");
  ObSEArray<const ObColumnSchemaV2 *, 4> rowkey;
  ObSEArray<ObString, 16> keys;
  ObMySQLTransaction transaction;
  ObSqlString qualified, where, select;
  if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table))) {
  } else if (OB_ISNULL(table) || OB_ISNULL(column = table->get_column_schema(column_id)) || !column->is_ai_column()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  } else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
  } else if (OB_ISNULL(database)) { ret = OB_SCHEMA_ERROR;
  } else if (OB_FAIL(get_ai_column_info(*column, allocator, info))) {
  } else if (OB_ISNULL(state = table->get_column_schema(info.state_column_id_))
          || OB_ISNULL(request = table->get_column_schema(info.request_column_id_))) { ret = OB_SCHEMA_ERROR;
  } else if (OB_FAIL(ai_qualified_name(qualified, database->get_database_name_str(), table->get_table_name_str()))) {
  } else if (!predicate.empty() && OB_FAIL(ai_retry_predicate(predicate, state->get_column_name_str(), where))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *key = table->get_column_schema_by_idx(i);
    if (nullptr != key && key->is_rowkey_column()) { ret = rowkey.push_back(key); }
  }
  if (OB_SUCC(ret) && rowkey.empty()) { ret = OB_SCHEMA_ERROR; }
  if (OB_SUCC(ret)) { ret = select.append("SELECT "); }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.count(); ++i) {
    if (OB_FAIL(select.append(i ? "," : ""))) {
    } else { ret = ai_column_append_identifier(select, rowkey.at(i)->get_column_name_str()); }
  }
  if (OB_SUCC(ret) && OB_FAIL(select.append(" FROM "))) {
  } else if (OB_SUCC(ret) && OB_FAIL(select.append(qualified.string()))) {
  } else if (OB_SUCC(ret) && !where.empty()) {
    if (OB_FAIL(select.append(" WHERE ("))) {
    } else if (OB_FAIL(select.append(where.string()))) {
    } else { ret = select.append(")"); }
  }
  if (OB_SUCC(ret) && OB_FAIL(select.append(" FOR UPDATE"))) {
  }
  // RETRY predicates are restricted by the resolver to current-row scalar
  // expressions; no subqueries, routines, variables or external reads can enter.
  if (OB_SUCC(ret)) { ret = transaction.start(ctx.get_sql_proxy()); }
  if (OB_SUCC(ret)) {
    ObISQLClient::ReadResult result;
    sqlclient::ObMySQLResult *rows = nullptr;
    if (OB_FAIL(transaction.read(result, select.ptr()))) {
    } else if (OB_ISNULL(rows = result.get_result())) { ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && OB_SUCC(ret = rows->next())) {
        ObSqlString key;
        for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.count(); ++i) {
          ObObj value;
          char *buffer = nullptr;
          int64_t length = 0, pos = 0;
          if (OB_FAIL(rows->get_obj(i, value, nullptr, &allocator))) {
          } else if (OB_FAIL(key.append(i ? " AND " : ""))) {
          } else if (OB_FAIL(ai_column_append_identifier(key, rowkey.at(i)->get_column_name_str()))) {
          } else if (OB_FAIL(key.append(" = "))) {
          } else if (value.is_string_type()) { ret = ai_column_append_literal(key, value.get_string());
          } else if (OB_FAIL(value.print_sql_literal(buffer, length, pos, allocator))) {
          } else { ret = key.append(buffer, pos); }
        }
        ObString copy;
        if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, key.string(), copy))) {
        } else if (OB_SUCC(ret)) { ret = keys.push_back(copy); }
      }
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
    char uuid[37] = {};
    ObSqlString remove, invalidate, task;
    int64_t affected = 0;
    if (OB_FAIL(ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID))) {
    } else if (OB_FAIL(remove.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND column_id=%lu AND rowkey_predicate=", table_id, column_id))) {
    } else if (OB_FAIL(ai_column_append_literal(remove, keys.at(i)))) {
    } else if (OB_FAIL(transaction.write(remove.ptr(), affected))) {
    } else if (OB_FAIL(invalidate.append("UPDATE "))) {
    } else if (OB_FAIL(invalidate.append(qualified.string()))) {
    } else if (OB_FAIL(invalidate.append(" SET "))) {
    } else if (OB_FAIL(ai_column_append_identifier(invalidate, column->get_column_name_str()))) {
    } else if (OB_FAIL(invalidate.append("=NULL,"))) {
    } else if (OB_FAIL(ai_column_append_identifier(invalidate, state->get_column_name_str()))) {
    } else if (OB_FAIL(invalidate.append("=0,"))) {
    } else if (OB_FAIL(ai_column_append_identifier(invalidate, request->get_column_name_str()))) {
    } else if (OB_FAIL(invalidate.append("="))) {
    } else if (OB_FAIL(ai_column_append_literal(invalidate, ObString(uuid)))) {
    } else if (OB_FAIL(invalidate.append(" WHERE "))) {
    } else if (OB_FAIL(invalidate.append(keys.at(i)))) {
    } else if (OB_FAIL(transaction.write(invalidate.ptr(), affected))) {
    } else if (OB_FAIL(ai_column_build_task_sql(*table, database->get_database_name_str(), info,
                                               keys.at(i), ObString(uuid), task))) {
    } else { ret = transaction.write(task.ptr(), affected); }
  }
  if (transaction.is_started()) {
    const int end_ret = transaction.end(OB_SUCCESS == ret);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
