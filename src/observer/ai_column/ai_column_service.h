/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include "share/ob_thread_pool.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace observer
{
// Durable execution states, separate from the row's PENDING/READY/FAILED state.
enum class AiColumnTaskState : int64_t
{
  PENDING = 0,
  RUNNING = 1,
  SUCCESS_STAGED = 2,
  RETRY_WAIT = 3,
  FAILURE_STAGED = 4
};

// One bounded worker owns a whole request group. HTTP is evaluated only after
// the claim transaction commits; terminal results survive lease loss/restart.
class AiColumnService : public share::ObThreadPool
{
public:
  AiColumnService();
  ~AiColumnService();
  static int server_module_init(AiColumnService *&service, lib::IRunWrapper *wrapper);
  int init(lib::IRunWrapper *wrapper);
  int start();
  void stop();
  void wait();
  void destroy();

private:
  struct Task
  {
    common::ObString request_id_;
    int64_t table_id_ = 0;
    int64_t column_id_ = 0;
    common::ObString predicate_;
    common::ObString database_;
    common::ObString table_;
    common::ObString column_;
    common::ObString state_column_;
    common::ObString request_column_;
    common::ObString execute_sql_;
    common::ObString result_sql_;
    int64_t definition_version_ = 0;
    int64_t state_ = 0;
    int64_t retry_count_ = 0;
    int64_t max_retries_ = 0;
    int64_t lease_until_ = 0;
    int64_t eligible_us_ = 0;
    int64_t claim_epoch_ = 0;
    common::ObString worker_;
  };
  using Tasks = common::ObSEArray<Task, 4>;
  void run1() override;
  int claim_group_(common::ObIAllocator &allocator, Tasks &tasks);
  int load_group_(common::ObISQLClient &client, const common::ObString &request_id,
                  int64_t table_id, bool for_update,
                  common::ObIAllocator &allocator, Tasks &tasks);
  int renew_group_(const Task &task);
  int release_group_(const Task &task);
  int execute_task_(Task &task, common::ObIAllocator &allocator);
  int validate_result_(const Task &task, const common::ObObj &value,
                       common::ObIAllocator &allocator, common::ObSqlString &literal);
  int stage_result_(Task &task, int inference_ret, const common::ObSqlString &literal);
  int finish_group_(const Task &task);
  int append_group_condition_(common::ObSqlString &sql, const Task &task, bool fenced) const;
  bool inited_;
  char worker_id_[128];
  DISALLOW_COPY_AND_ASSIGN(AiColumnService);
};
}
}
