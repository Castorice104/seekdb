#pragma once

#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_table_schema.h"
#include "sql/resolver/ddl/ob_ai_column_info.h"

namespace oceanbase { namespace sql {
class ObExecContext;

// SQL construction shared by transactional DML maintenance and RETRY.
int ai_column_append_identifier(common::ObSqlString &sql, const common::ObString &name);
int ai_column_append_literal(common::ObSqlString &sql, const common::ObString &value);
int ai_column_build_task_sql(const share::schema::ObTableSchema &table,
                            const common::ObString &database_name,
                            const ObAiColumnInfo &info,
                            const common::ObString &predicate,
                            const common::ObString &request_id,
                            common::ObSqlString &sql);
int ai_column_regenerate_column(ObExecContext &ctx, uint64_t table_id,
                               uint64_t column_id, const common::ObString &predicate);

} }
