#pragma once

namespace oceanbase
{
namespace share { namespace schema { class ObSchemaGetterGuard; } }
namespace sql
{
class ObDMLStmt;
class ObExecContext;
class ObPhysicalPlan;
// Capture resolved references before rewrite removes subqueries/constant predicates.
int collect_ai_column_read_refs(const ObDMLStmt &stmt,
                               share::schema::ObSchemaGetterGuard &schema_guard,
                               ObPhysicalPlan &plan);
// Runs after start_stmt and before opening any physical operator or writing rows.
int check_ai_column_strict_read(ObExecContext &ctx, const ObPhysicalPlan &plan);
}
}
