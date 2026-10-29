/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_stmt_expr_visitor.h"
#include "sql/resolver/expr/ob_raw_expr.h"

namespace oceanbase { namespace sql {

// Elide numerical scores only after proving every use is a WHERE truth test.
// Separate scope walks are essential: relation-expression collection deduplicates
// roots, including a MATCH shared by SELECT and WHERE.
class FtsRelevanceUsage
{
public:
  static int needs_score(const ObDMLStmt &stmt, const ObMatchFunRawExpr &match, bool &needed)
  {
    int ret = common::OB_SUCCESS;
    needed = true;
    int64_t child_count = 0;
    if (!stmt.is_select_stmt() || match.get_expr_type() != T_FUN_MATCH_AGAINST
        || match.get_mode_flag() != NATURAL_LANGUAGE_MODE
        || !match.get_columns_boosts().empty() || nullptr != match.get_param_text_expr()) {
      // Boolean/weighted/custom searches and DML retain the established path.
    } else if (OB_FAIL(stmt.get_child_stmt_size(child_count))) {
    } else if (child_count != 0) {
      // Correlated references can escape this query block.
    } else {
      common::ObSEArray<ObRawExpr *, 16> roots;
      ObStmtExprGetter non_where;
      non_where.set_relation_scope();
      non_where.remove_scope(SCOPE_WHERE);
      bool found = false;
      bool numeric = false;
      int64_t budget = 4096;
      if (OB_FAIL(stmt.get_relation_exprs(roots, non_where))) {
      } else {
        for (int64_t i = 0; !numeric && i < roots.count(); ++i) {
          visit(roots.at(i), match, false, 0, budget, found, numeric);
        }
        roots.reuse();
        if (!numeric && OB_FAIL(stmt.get_relation_exprs(roots, SCOPE_WHERE))) {
        } else {
          for (int64_t i = 0; !numeric && i < roots.count(); ++i) {
            visit(roots.at(i), match, true, 0, budget, found, numeric);
          }
          needed = numeric || !found;
        }
      }
    }
    return ret;
  }

private:
  static void visit(const ObRawExpr *expr, const ObRawExpr &match, bool boolean_context,
                    int depth, int64_t &budget, bool &found, bool &numeric)
  {
    if (numeric || nullptr == expr) {
    } else if (--budget < 0 || depth > 128) {
      // Shared expression DAGs may expand exponentially. Uncertain or very
      // complex cases keep scores; optimization must not cause a SQL error.
      numeric = true;
    } else if (expr == &match) {
      found = true;
      numeric = !boolean_context;
    } else {
      const ObItemType type = expr->get_expr_type();
      const bool child_boolean = boolean_context &&
          (type == T_OP_BOOL || type == T_OP_AND || type == T_OP_OR || type == T_OP_NOT);
      for (int64_t i = 0; !numeric && i < expr->get_param_count(); ++i) {
        visit(expr->get_param_expr(i), match, child_boolean, depth + 1, budget, found, numeric);
      }
    }
  }
};

}} // namespace oceanbase::sql
