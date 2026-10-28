/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_column/ai_column_service.h"

#include <cmath>
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_unify_serialize.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/object/ob_obj_cast.h"
#include "share/json/ob_json_helper.h"
#include "share/ob_lob_access_utils.h"
#include "sql/resolver/ddl/ob_ai_column_info.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace share::schema;
namespace
{
constexpr int64_t LEASE_US = 30L * 1000 * 1000;
constexpr int64_t POLL_US = 100 * 1000;
constexpr int64_t MAX_RETRY_DELAY_US = 30L * 1000 * 1000;
constexpr const char *TASK_TABLE = "oceanbase.__all_ai_column_task";

// Hex literals do not depend on NO_BACKSLASH_ESCAPES and cannot terminate SQL.
int append_string(ObSqlString &sql, const ObString &str)
{
  int ret = sql.append("X'");
  static const char HEX[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(str.ptr()[i]);
    char pair[2] = { HEX[ch >> 4], HEX[ch & 15] };
    ret = sql.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}

int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = name.empty() ? OB_INVALID_ARGUMENT : sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if ('`' == name.ptr()[i]) { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int append_value(ObSqlString &sql, const ObObj &obj, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (obj.is_string_type()) {
    if (OB_FAIL(sql.append("CONVERT("))) {
    } else if (OB_FAIL(append_string(sql, obj.get_string()))) {
    } else { ret = sql.append(" USING utf8mb4)"); }
  } else {
    char *buf = nullptr;
    int64_t length = 0;
    int64_t pos = 0;
    if (OB_FAIL(obj.print_sql_literal(buf, length, pos, allocator))) {
    } else { ret = sql.append(buf, pos); }
  }
  return ret;
}

int write_sql(ObISQLClient &client, const ObSqlString &sql, int64_t &affected)
{
  return client.write(sql.ptr(), affected);
}

int finish_transaction(ObMySQLTransaction &trans, int ret)
{
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCCESS == ret);
    if (OB_SUCCESS == ret) { ret = end_ret; }
  }
  return ret;
}

bool terminal(int64_t state)
{
  return state == static_cast<int64_t>(AiColumnTaskState::SUCCESS_STAGED)
      || state == static_cast<int64_t>(AiColumnTaskState::FAILURE_STAGED);
}
}

AiColumnService::AiColumnService() : inited_(false), worker_id_()
{
}
AiColumnService::~AiColumnService() { destroy(); }

int AiColumnService::server_module_init(AiColumnService *&service, lib::IRunWrapper *wrapper)
{
  return OB_ISNULL(service) ? OB_INVALID_ARGUMENT : service->init(wrapper);
}

int AiColumnService::init(lib::IRunWrapper *wrapper)
{
  int ret = OB_SUCCESS;
  if (inited_) { ret = OB_INIT_TWICE; }
  else {
    // The epoch, rather than this diagnostic owner string, provides fencing.
    snprintf(worker_id_, sizeof(worker_id_), "ai-column-%ld-%p",
             ObTimeUtility::current_time(), this);
    set_run_wrapper(wrapper);
    inited_ = true;
  }
  return ret;
}
int AiColumnService::start()
{
  int ret = OB_SUCCESS;
  if (!inited_) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(set_thread_count(2))) {
  } else { ret = share::ObThreadPool::start(); }
  return ret;
}
void AiColumnService::stop() { share::ObThreadPool::stop(); }
void AiColumnService::wait() { share::ObThreadPool::wait(); }
void AiColumnService::destroy()
{
  if (inited_) {
    stop();
    wait();
    share::ObThreadPool::destroy();
    inited_ = false;
  }
}

int AiColumnService::append_group_condition_(ObSqlString &sql, const Task &task, bool fenced) const
{
  int ret = sql.append(" request_id=");
  if (OB_SUCC(ret)) { ret = append_string(sql, task.request_id_); }
  if (OB_SUCC(ret)) { ret = sql.append_fmt(" AND table_id=%ld", task.table_id_); }
  if (OB_SUCC(ret) && fenced) {
    if (OB_FAIL(sql.append(" AND worker_id="))) {
    } else if (OB_FAIL(append_string(sql, ObString(worker_id_)))) {
    } else { ret = sql.append_fmt(" AND claim_epoch=%ld", task.claim_epoch_); }
  }
  return ret;
}

int AiColumnService::load_group_(ObISQLClient &client, const ObString &request_id,
    int64_t table_id, bool for_update, ObIAllocator &allocator, Tasks &tasks)
{
  int ret = OB_SUCCESS;
  Task key;
  key.request_id_ = request_id;
  key.table_id_ = table_id;
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  sqlclient::ObMySQLResult *rows = nullptr;
  tasks.reuse();
  if (OB_FAIL(sql.append_fmt(
      "SELECT request_id,column_id,table_id,rowkey_predicate,database_name,table_name,"
      "column_name,state_column_name,request_column_name,execute_sql,COALESCE(result_sql,''),"
      "definition_version,state,retry_count,max_retries,lease_until,claim_epoch,"
      "COALESCE(worker_id,''),CAST(UNIX_TIMESTAMP(eligible_ts)*1000000 AS SIGNED) FROM %s WHERE", TASK_TABLE))) {
  } else if (OB_FAIL(append_group_condition_(sql, key, false))) {
  } else if (OB_FAIL(sql.append(" ORDER BY column_id"))) {
  } else if (for_update && OB_FAIL(sql.append(" FOR UPDATE"))) {
  } else if (OB_FAIL(client.read(result, sql.ptr()))) {
  } else if (OB_ISNULL(rows = result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(ret) && OB_SUCC(ret = rows->next())) {
      Task task;
      auto get_string = [&](int64_t index, ObString &target) -> int {
        ObString value;
        int err = rows->get_varchar(index, value);
        if (OB_SUCCESS == err) { err = ob_write_string(allocator, value, target); }
        return err;
      };
      if (OB_FAIL(get_string(0, task.request_id_))
          || OB_FAIL(rows->get_int(1, task.column_id_))
          || OB_FAIL(rows->get_int(2, task.table_id_))
          || OB_FAIL(get_string(3, task.predicate_))
          || OB_FAIL(get_string(4, task.database_))
          || OB_FAIL(get_string(5, task.table_))
          || OB_FAIL(get_string(6, task.column_))
          || OB_FAIL(get_string(7, task.state_column_))
          || OB_FAIL(get_string(8, task.request_column_))
          || OB_FAIL(get_string(9, task.execute_sql_))
          || OB_FAIL(get_string(10, task.result_sql_))
          || OB_FAIL(rows->get_int(11, task.definition_version_))
          || OB_FAIL(rows->get_int(12, task.state_))
          || OB_FAIL(rows->get_int(13, task.retry_count_))
          || OB_FAIL(rows->get_int(14, task.max_retries_))
          || OB_FAIL(rows->get_int(15, task.lease_until_))
          || OB_FAIL(rows->get_int(16, task.claim_epoch_))
          || OB_FAIL(get_string(17, task.worker_))
          || OB_FAIL(rows->get_int(18, task.eligible_us_))) {
      } else {
        ObSchemaGetterGuard guard;
        const ObTableSchema *table = nullptr;
        const ObDatabaseSchema *database = nullptr;
        const ObColumnSchemaV2 *column = nullptr;
        sql::ObAiColumnInfo info;
        if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(guard))) {
        } else if (OB_FAIL(guard.get_table_schema(task.table_id_, table))) {
        } else if (nullptr != table && nullptr != (column = table->get_column_schema(task.column_id_))
                   && column->is_ai_column()) {
          if (OB_FAIL(sql::get_ai_column_info(*column, allocator, info))) {
          } else if (info.definition_version_ == task.definition_version_) {
            const ObColumnSchemaV2 *state = table->get_column_schema(info.state_column_id_);
            const ObColumnSchemaV2 *request = table->get_column_schema(info.request_column_id_);
            if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
            } else if (OB_ISNULL(database) || OB_ISNULL(state) || OB_ISNULL(request)) { ret = OB_SCHEMA_ERROR;
            } else if (OB_FAIL(ob_write_string(allocator, database->get_database_name_str(), task.database_))
                || OB_FAIL(ob_write_string(allocator, table->get_table_name_str(), task.table_))
                || OB_FAIL(ob_write_string(allocator, column->get_column_name_str(), task.column_))
                || OB_FAIL(ob_write_string(allocator, state->get_column_name_str(), task.state_column_))
                || OB_FAIL(ob_write_string(allocator, request->get_column_name_str(), task.request_column_))) {
            } else {
              ObSqlString execution;
              if (OB_FAIL(execution.append("SELECT "))
                  || OB_FAIL(execution.append(info.normalized_expr_))
                  || OB_FAIL(execution.append(" FROM "))
                  || OB_FAIL(append_identifier(execution, task.database_))
                  || OB_FAIL(execution.append("."))
                  || OB_FAIL(append_identifier(execution, task.table_))
                  || OB_FAIL(execution.append_fmt(" WHERE (%.*s) AND ", task.predicate_.length(), task.predicate_.ptr()))
                  || OB_FAIL(append_identifier(execution, task.request_column_))
                  || OB_FAIL(execution.append("="))
                  || OB_FAIL(append_string(execution, task.request_id_))
                  || OB_FAIL(ob_write_string(allocator, execution.string(), task.execute_sql_))) {
              }
            }
          }
        }
        if (OB_SUCC(ret)) { ret = tasks.push_back(task); }
      }
    }
    if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
  }
  return ret;
}

int AiColumnService::claim_group_(ObIAllocator &allocator, Tasks &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObISQLClient::ReadResult result;
  sqlclient::ObMySQLResult *rows = nullptr;
  Task key;
  const int64_t now = ObTimeUtility::current_time();
  // A staged member never causes an unfinished, delayed group to spin.
  if (OB_FAIL(sql.append_fmt(
      "SELECT request_id,table_id FROM %s GROUP BY request_id,table_id "
      "HAVING MAX(lease_until)<=%ld AND MAX(eligible_ts)<=NOW(6) "
      "ORDER BY MIN(gmt_create) LIMIT 1", TASK_TABLE, now))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->read(result, sql.ptr()))) {
  } else if (OB_ISNULL(rows = result.get_result())) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(rows->next())) {
    if (OB_ITER_END == ret) { ret = OB_ENTRY_NOT_EXIST; }
  } else {
    ObString request;
    if (OB_FAIL(rows->get_varchar(0L, request))) {
    } else if (OB_FAIL(ob_write_string(allocator, request, key.request_id_))) {
    } else { ret = rows->get_int(1L, key.table_id_); }
  }
  result.reset();
  if (OB_SUCC(ret)) {
    ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    } else if (OB_FAIL(load_group_(trans, key.request_id_, key.table_id_, true, allocator, tasks))) {
    } else if (tasks.empty()) { ret = OB_ENTRY_NOT_EXIST; }
    else {
      int64_t epoch = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
        if (tasks.at(i).lease_until_ > now
            || tasks.at(i).eligible_us_ > now) {
          ret = OB_EAGAIN;
        }
        epoch = MAX(epoch, tasks.at(i).claim_epoch_);
      }
      if (OB_SUCC(ret)) {
        ++epoch;
        sql.reset();
        int64_t affected = 0;
        if (OB_FAIL(sql.append_fmt("UPDATE %s SET worker_id=", TASK_TABLE))) {
        } else if (OB_FAIL(append_string(sql, ObString(worker_id_)))) {
        } else if (OB_FAIL(sql.append_fmt(
            ",claim_epoch=%ld,lease_until=%ld,state=CASE WHEN state IN(0,1,3) THEN 1 ELSE state END WHERE",
            epoch, now + LEASE_US))) {
        } else if (OB_FAIL(append_group_condition_(sql, key, false))) {
        } else if (OB_FAIL(write_sql(trans, sql, affected))) {
        } else if (affected != tasks.count()) { ret = OB_EAGAIN; }
        for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
          tasks.at(i).claim_epoch_ = epoch;
          if (!terminal(tasks.at(i).state_)) { tasks.at(i).state_ = 1; }
        }
      }
    }
    ret = finish_transaction(trans, ret);
  }
  return ret;
}

int AiColumnService::renew_group_(const Task &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE %s SET lease_until=%ld WHERE",
      TASK_TABLE, ObTimeUtility::current_time() + LEASE_US))) {
  } else if (OB_FAIL(append_group_condition_(sql, task, true))) {
  } else if (OB_FAIL(write_sql(*GCTX.sql_proxy_, sql, affected))) {
  } else if (0 == affected) { ret = OB_EAGAIN; }
  return ret;
}

int AiColumnService::release_group_(const Task &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.append_fmt("UPDATE %s SET lease_until=0,worker_id=NULL,eligible_ts=DATE_ADD(NOW(6), INTERVAL 1 SECOND) WHERE", TASK_TABLE))) {
  } else if (OB_FAIL(append_group_condition_(sql, task, true))) {
  } else { ret = write_sql(*GCTX.sql_proxy_, sql, affected); }
  return ret;
}

int AiColumnService::validate_result_(const Task &task, const ObObj &value,
    ObIAllocator &allocator, ObSqlString &literal)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  sql::ObAiColumnInfo info;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(task.table_id_, table))) {
  } else if (OB_ISNULL(table)
      || OB_ISNULL(column = table->get_column_schema(task.column_id_))) {
    ret = OB_SCHEMA_ERROR;
  } else if (OB_FAIL(sql::get_ai_column_info(*column, allocator, info))) {
  } else if (info.definition_version_ != task.definition_version_) {
    ret = OB_SCHEMA_ERROR;
  } else if (value.is_null()) {
    ret = literal.append("NULL");
  } else if (info.vector_dim_ > 0) {
    // AI_EMBED returns a JSON text vector. Native DML performs its physical
    // vector conversion and index maintenance only after this shape check.
    share::ObJsonReaderHelper reader(allocator);
    ObJsonNode *root = nullptr;
    if (!value.is_string_type()) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(reader.parse(value.get_string().ptr(), value.get_string().length(), root))) {
    } else if (OB_ISNULL(root) || root->json_type() != ObJsonNodeType::J_ARRAY
        || root->element_count() != info.vector_dim_) { ret = OB_INVALID_DATA; }
    else {
      for (int64_t i = 0; OB_SUCC(ret) && i < info.vector_dim_; ++i) {
        ObIJsonBase *element = nullptr;
        float number = 0;
        if (OB_FAIL(reader.get_array_element(root, i, element))) {
        } else if (OB_ISNULL(element) || !share::ObJsonHelper::is_number_type(element)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(reader.get_float_value(element, number))) {
        } else if (!std::isfinite(number)) { ret = OB_INVALID_DATA; }
      }
      if (OB_SUCC(ret)) { ret = append_value(literal, value, allocator); }
    }
  } else {
    ObAccuracy accuracy = column->get_accuracy();
    ObCastCtx ctx(&allocator, nullptr,
        CM_STRICT_MODE | CM_ERROR_ON_SCALE_OVER | CM_STRICT_JSON,
        column->get_collation_type(), &accuracy);
    ObObj casted;
    const ObObj *converted = nullptr;
    // MySQL integer casts normally round fractional strings. Reject fractional
    // inference output explicitly before the target cast.
    if (ob_is_integer_type(column->get_data_type())) {
      ObObj numeric;
      const ObObj *number = nullptr;
      if (OB_FAIL(ObObjCaster::to_type(ObNumberType, ctx, value, numeric, number))) {
      } else if (OB_ISNULL(number) || !number->get_number().is_integer()) {
        ret = OB_INVALID_DATA;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObObjCaster::to_type(column->get_data_type(), ctx, value, casted, converted))) {
    } else if (OB_SUCCESS != ctx.warning_) { ret = ctx.warning_; }
    else if (OB_ISNULL(converted)) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(ob_obj_accuracy_check_only(accuracy, column->get_collation_type(), *converted))) {
    } else if ((converted->is_float() && !std::isfinite(converted->get_float()))
        || (converted->is_double() && !std::isfinite(converted->get_double()))) {
      ret = OB_INVALID_DATA;
    } else {
      // JSON SQL literals must contain JSON text, never the internal binary
      // representation produced by the validation cast.
      ret = append_value(literal, column->get_data_type() == ObJsonType ? value : *converted, allocator);
    }
  }
  return ret;
}

int AiColumnService::stage_result_(Task &task, int inference_ret, const ObSqlString &literal)
{
  int ret = OB_SUCCESS;
  const bool success = OB_SUCCESS == inference_ret;
  const int64_t failures = task.retry_count_ + (success ? 0 : 1);
  const bool retry = !success && inference_ret != OB_ENTRY_NOT_EXIST
      && inference_ret != OB_SCHEMA_ERROR && inference_ret != OB_TABLE_NOT_EXIST
      && failures <= task.max_retries_;
  const int64_t state = success ? 2 : (retry ? 3 : 4);
  const int64_t delay = MIN(MAX_RETRY_DELAY_US, (1L << MIN(failures, 5L)) * 500000L);
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.append_fmt(
      "UPDATE %s SET state=%ld,retry_count=%ld,result_sql=", TASK_TABLE, state, failures))) {
  } else if (OB_FAIL(append_string(sql, success ? literal.string() : ObString("NULL")))) {
  } else if (OB_FAIL(sql.append_fmt(
      ",last_error='%d',eligible_ts=DATE_ADD(NOW(6), INTERVAL %ld MICROSECOND) WHERE",
      inference_ret, retry ? delay : 0))) {
  } else if (OB_FAIL(append_group_condition_(sql, task, true))) {
  } else if (OB_FAIL(sql.append_fmt(" AND column_id=%ld AND definition_version=%ld",
      task.column_id_, task.definition_version_))) {
  } else if (OB_FAIL(write_sql(*GCTX.sql_proxy_, sql, affected))) {
  } else if (0 == affected) {
    // A concurrently superseded member may have disappeared. No unguarded
    // recovery write is permitted; the remaining group is reloaded at publish.
  } else {
    task.state_ = state;
    task.retry_count_ = failures;
  }
  return ret;
}

int AiColumnService::execute_task_(Task &task, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSqlString literal;
  ObISQLClient::ReadResult result;
  sqlclient::ObMySQLResult *rows = nullptr;
  ObSqlString execution;
  ObObj null_value;
  null_value.set_null();
  ObSqlString discarded;
  // Validate identity before spending an inference on an obsolete definition.
  if (OB_FAIL(validate_result_(task, null_value, allocator, discarded))) {
  } else if (OB_FAIL(execution.assign(task.execute_sql_))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->read(result, execution.ptr()))) {
  } else if (OB_ISNULL(rows = result.get_result())) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(rows->next())) {
    if (OB_ITER_END == ret) { ret = OB_ENTRY_NOT_EXIST; }
  } else {
    ObObj value;
    if (rows->get_column_count() != 1) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(rows->get_obj(0L, value, nullptr, &allocator))) {
    } else {
      if (value.is_lob_storage()) {
        ObString text;
        const ObCollationType collation = value.get_collation_type();
        if (OB_FAIL(lob_helper::read_real_string_data(&allocator, value, text, nullptr))) {
        } else {
          value.set_varchar(text);
          value.set_collation_type(collation);
        }
      }
      if (OB_SUCC(ret)) { ret = validate_result_(task, value, allocator, literal); }
      const int next_ret = rows->next();
      if (OB_SUCC(ret) && OB_ITER_END != next_ret) { ret = OB_SUCCESS == next_ret ? OB_ERR_UNEXPECTED : next_ret; }
    }
  }
  // Release the read-only SQL connection before starting any stage write.
  result.reset();
  const int inference_ret = ret;
  ret = stage_result_(task, inference_ret, literal);
  if (OB_SUCCESS != inference_ret) {
    LOG_WARN("AI column inference attempt failed", K(inference_ret),
        K(task.table_id_), K(task.column_id_), K(task.retry_count_));
  }
  return ret;
}

int AiColumnService::finish_group_(const Task &key)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnPublish");
  Tasks tasks;
  Tasks publish_tasks;
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  } else if (OB_FAIL(load_group_(trans, key.request_id_, key.table_id_, true, allocator, tasks))) {
  } else {
    bool all_terminal = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      const Task &task = tasks.at(i);
      if (task.worker_ != ObString(worker_id_) || task.claim_epoch_ != key.claim_epoch_) {
        ret = OB_EAGAIN;
      }
      if (OB_SUCC(ret)) {
        ObObj null_value;
        null_value.set_null();
        ObSqlString discarded;
        const int definition_ret = validate_result_(task, null_value, allocator, discarded);
        if (definition_ret == OB_SCHEMA_ERROR || definition_ret == OB_TABLE_NOT_EXIST
            || definition_ret == OB_ENTRY_NOT_EXIST) {
          // Dropping/redefining one column does not cancel the valid siblings
          // of its original group. Never even resolve the stale column name.
        } else if (OB_SUCCESS != definition_ret) {
          ret = definition_ret;
        } else if (OB_FAIL(publish_tasks.push_back(task))) {
        } else {
          all_terminal = all_terminal && terminal(task.state_);
        }
      }
    }
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_FAIL(ret) || tasks.empty()) {
    } else if (!all_terminal) {
      if (OB_FAIL(sql.append_fmt("UPDATE %s SET lease_until=0,worker_id=NULL WHERE", TASK_TABLE))) {
      } else if (OB_FAIL(append_group_condition_(sql, key, true))) {
      } else { ret = write_sql(trans, sql, affected); }
    } else {
      // All surviving members commit together; individual guarded assignments
      // preserve native VECTOR/JSON conversion instead of CASE type unification.
      for (int64_t i = 0; OB_SUCC(ret) && i < publish_tasks.count(); ++i) {
        const Task &task = publish_tasks.at(i);
        sql.reset();
        if (OB_FAIL(sql.append("UPDATE "))) {
        } else if (OB_FAIL(append_identifier(sql, task.database_))) {
        } else if (OB_FAIL(sql.append("."))) {
        } else if (OB_FAIL(append_identifier(sql, task.table_))) {
        } else if (OB_FAIL(sql.append(" SET "))) {
        } else if (OB_FAIL(append_identifier(sql, task.column_))) {
        } else if (OB_FAIL(sql.append_fmt("=%.*s,", task.result_sql_.length(), task.result_sql_.ptr()))) {
        } else if (OB_FAIL(append_identifier(sql, task.state_column_))) {
        } else if (OB_FAIL(sql.append_fmt("=%d WHERE (%.*s) AND ", task.state_ == 2 ? 1 : 2,
                          task.predicate_.length(), task.predicate_.ptr()))) {
        } else if (OB_FAIL(append_identifier(sql, task.request_column_))) {
        } else if (OB_FAIL(sql.append("="))) {
        } else if (OB_FAIL(append_string(sql, task.request_id_))) {
        } else if (OB_FAIL(sql.append_fmt(
            " AND EXISTS(SELECT 1 FROM oceanbase.__all_ai_column WHERE table_id=%ld "
            "AND column_id=%ld AND definition_version=%ld) AND EXISTS(SELECT 1 FROM %s WHERE",
            task.table_id_, task.column_id_, task.definition_version_, TASK_TABLE))) {
        } else if (OB_FAIL(append_group_condition_(sql, key, true))) {
        } else if (OB_FAIL(sql.append_fmt(
            ") AND EXISTS(SELECT 1 FROM oceanbase.__all_table ai_t "
            "JOIN oceanbase.__all_database ai_d ON ai_d.database_id=ai_t.database_id "
            "WHERE ai_t.table_id=%ld AND ai_t.table_name=", task.table_id_))) {
        } else if (OB_FAIL(append_string(sql, task.table_))) {
        } else if (OB_FAIL(sql.append(" AND ai_d.database_name="))) {
        } else if (OB_FAIL(append_string(sql, task.database_))) {
        } else if (OB_FAIL(sql.append(")"))) {
        } else if (OB_FAIL(write_sql(trans, sql, affected))) {
          // Keep staged results for a rename racing with this transaction.
        } else if (0 == affected) {
          ObSchemaGetterGuard current_guard;
          const ObTableSchema *current_table = nullptr;
          const ObDatabaseSchema *current_database = nullptr;
          if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(current_guard))) {
          } else if (OB_FAIL(current_guard.get_table_schema(task.table_id_, current_table))) {
          } else if (nullptr != current_table) {
            if (OB_FAIL(current_guard.get_database_schema(current_table->get_database_id(), current_database))) {
            } else if (nullptr == current_database || current_table->get_table_name_str() != task.table_
                       || current_database->get_database_name_str() != task.database_) {
              ret = OB_EAGAIN;
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        sql.reset();
        if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE", TASK_TABLE))) {
        } else if (OB_FAIL(append_group_condition_(sql, key, true))) {
        } else { ret = write_sql(trans, sql, affected); }
      }
    }
  }
  return finish_transaction(trans, ret);
}

void AiColumnService::run1()
{
  lib::set_thread_name("AiColumnWorker");
  while (!has_set_stop()) {
    int ret = OB_SUCCESS;
    ObArenaAllocator allocator("AiColumnWorker");
    Tasks tasks;
    if (OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(claim_group_(allocator, tasks))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.count(); ++i) {
        if (terminal(tasks.at(i).state_)) {
        } else if (OB_FAIL(renew_group_(tasks.at(i)))) {
        } else if (OB_FAIL(execute_task_(tasks.at(i), allocator))) {
        }
      }
      if (OB_SUCC(ret) && !tasks.empty() && !has_set_stop()) {
        ret = finish_group_(tasks.at(0));
      }
      if (!tasks.empty() && (OB_SUCCESS != ret || has_set_stop())) {
        // A retryable SQL/publication failure must not wait for crash recovery.
        // This remains fenced even if another worker has already reclaimed it.
        const int release_ret = release_group_(tasks.at(0));
        if (OB_SUCCESS != release_ret) {
          LOG_WARN("failed to release AI column group", K(release_ret));
        }
      }
    }
    if (OB_SUCCESS != ret && OB_ENTRY_NOT_EXIST != ret && OB_NOT_INIT != ret
        && OB_EAGAIN != ret && REACH_TIME_INTERVAL(10L * 1000 * 1000)) {
      LOG_WARN("AI column background iteration failed", K(ret));
    }
    // Includes a bounded idle wait and prevents one broken task monopolizing
    // the CPU. Stop is observed at most 100ms after the current provider call.
    if (OB_SUCCESS != ret || tasks.empty()) { usleep(POLL_US); }
  }
}
}
}
