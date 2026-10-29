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

#include <memory>
#include <vector>
#include <gtest/gtest.h>
#include "sql/optimizer/ob_fts_relevance_usage.h"
#include "sql/resolver/dml/ob_select_stmt.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

TEST(FtsRelevanceUsage, boolean_filter_and_shared_numeric_projection)
{
  ObArenaAllocator alloc;
  ObSelectStmt stmt;
  ObMatchFunRawExpr match;match.set_expr_type(T_FUN_MATCH_AGAINST);
  ObOpRawExpr truth(alloc);truth.set_expr_type(T_OP_BOOL);
  ASSERT_EQ(OB_SUCCESS,truth.set_param_expr(&match));
  ASSERT_EQ(OB_SUCCESS,stmt.get_condition_exprs().push_back(&truth));
  bool needed=true;
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));
  EXPECT_FALSE(needed);
  SelectItem item;item.expr_=&truth;
  ASSERT_EQ(OB_SUCCESS,stmt.get_select_items().push_back(item));
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));
  EXPECT_TRUE(needed); // the identical deduplicated root occurs in SELECT too
}

TEST(FtsRelevanceUsage, numeric_threshold_order_and_boolean_mode_keep_scores)
{
  ObArenaAllocator alloc;
  ObSelectStmt stmt;
  ObMatchFunRawExpr match;match.set_expr_type(T_FUN_MATCH_AGAINST);
  ObObj value;value.set_int(0);ObConstRawExpr zero(value,T_INT);
  ObOpRawExpr threshold(alloc);threshold.set_expr_type(T_OP_GT);
  ASSERT_EQ(OB_SUCCESS,threshold.set_param_exprs(&match,&zero));
  ASSERT_EQ(OB_SUCCESS,stmt.get_condition_exprs().push_back(&threshold));
  bool needed=false;
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));EXPECT_TRUE(needed);
  stmt.get_condition_exprs().reuse();
  ASSERT_EQ(OB_SUCCESS,stmt.get_condition_exprs().push_back(&match));
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));EXPECT_FALSE(needed);
  OrderItem order;order.expr_=&match;order.order_type_=default_asc_direction();
  ASSERT_EQ(OB_SUCCESS,stmt.get_order_items().push_back(order));
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));EXPECT_TRUE(needed);
  stmt.get_order_items().reuse();match.set_mode_flag(BOOLEAN_MODE);
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));EXPECT_TRUE(needed);
}

TEST(FtsRelevanceUsage, shared_dag_budget_and_subqueries_are_conservative)
{
  ObArenaAllocator alloc;
  ObSelectStmt stmt;
  ObMatchFunRawExpr match;match.set_expr_type(T_FUN_MATCH_AGAINST);
  ObObj value;value.set_int(1);ObConstRawExpr one(value,T_INT);
  std::vector<std::unique_ptr<ObOpRawExpr>> nodes;ObRawExpr *prev=&one;
  for(int i=0;i<20;++i){
    nodes.emplace_back(new ObOpRawExpr(alloc));auto &node=*nodes.back();
    node.set_expr_type(T_OP_AND);ASSERT_EQ(OB_SUCCESS,node.set_param_exprs(prev,prev));prev=&node;
  }
  ASSERT_EQ(OB_SUCCESS,stmt.get_condition_exprs().push_back(prev));
  ASSERT_EQ(OB_SUCCESS,stmt.get_condition_exprs().push_back(&match));
  bool needed=false;
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));EXPECT_TRUE(needed);
  stmt.get_condition_exprs().reuse();
  ASSERT_EQ(OB_SUCCESS,stmt.get_condition_exprs().push_back(&match));
  ObSelectStmt child;ObQueryRefRawExpr subquery;subquery.set_ref_stmt(&child);ASSERT_EQ(OB_SUCCESS,stmt.get_subquery_exprs().push_back(&subquery));
  ASSERT_EQ(OB_SUCCESS,FtsRelevanceUsage::needs_score(stmt,match,needed));EXPECT_TRUE(needed);
}
