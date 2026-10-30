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

#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr.h"
#include "data_plane/access/ob_tablet_scan.h"
#include "storage/retrieval/ob_i_sparse_retrieval_iter.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::storage;

TEST(FtsRetrievalHotPath, direct_bool_filter_requires_exact_double_projection)
{
  ObExpr projection;
  projection.datum_meta_.type_ = ObDoubleType;
  ObExpr filter;
  filter.type_ = T_OP_BOOL;
  filter.datum_meta_.type_ = ObInt32Type;
  ObExpr *arguments[] = {&projection};
  filter.arg_cnt_ = 1;
  filter.args_ = arguments;
  ObSparseRetrievalMergeParam param;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
  param.filter_expr_ = &filter;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
  param.relevance_proj_expr_ = &projection;
  EXPECT_TRUE(param.can_eval_bool_filter_directly());
  ObExpr other;
  other.datum_meta_.type_ = ObDoubleType;
  arguments[0] = &other;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
  arguments[0] = &projection;
  filter.type_ = T_OP_GT;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
  filter.type_ = T_OP_BOOL;
  projection.datum_meta_.type_ = ObNumberType;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
  projection.datum_meta_.type_ = ObDoubleType;
  filter.need_stack_check_ = true;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
  filter.need_stack_check_ = false;
  filter.arg_cnt_ = 2;
  EXPECT_FALSE(param.can_eval_bool_filter_directly());
}
