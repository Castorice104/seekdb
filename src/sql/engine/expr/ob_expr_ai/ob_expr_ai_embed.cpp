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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

namespace
{
int embed_contents(ObIAllocator &allocator, const ObString &model_id,
                   const int64_t dimension, ObArray<ObString> &contents,
                   ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver =
      share::server_service<query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  ObJsonInt *dimension_json = nullptr;
  if (dimension > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dimension_json))) {
    } else if (OB_FAIL(config->add("dimensions", dimension_json))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(resolver) || OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
    } else if (results.count() != contents.count()) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int read_embed_arguments(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                         const ObDatum &model, const ObDatum &content, const ObDatum *dim,
                         ObString &model_id, ObString &text, int64_t &dimension)
{
  int ret = OB_SUCCESS;
  dimension = 0;
  if (model.is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
  } else if (nullptr != dim && (dim->is_null() || dim->get_int() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                 model, expr.args_[0]->datum_meta_, expr.args_[0]->obj_meta_.has_lob_header(), model_id))) {
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                 content, expr.args_[1]->datum_meta_, expr.args_[1]->obj_meta_.has_lob_header(), text))) {
  } else if (model_id.empty() || text.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
  } else {
    dimension = nullptr == dim ? 0 : dim->get_int();
  }
  return ret;
}
} // namespace

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *model = nullptr;
  ObDatum *content = nullptr;
  ObDatum *dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, model, content, dim))
                         : OB_FAIL(expr.eval_param_value(ctx, model, content))) {
  } else if (content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString model_id;
    ObString text;
    int64_t dimension = 0;
    ObArray<ObString> contents;
    ObArray<ObString> results;
    if (OB_FAIL(read_embed_arguments(expr, ctx, allocator, *model, *content, dim,
                                    model_id, text, dimension))) {
    } else if (OB_FAIL(contents.push_back(text))) {
    } else if (OB_FAIL(embed_contents(allocator, model_id, dimension, contents, results))) {
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                      const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *result_datums = expr.locate_batch_datums(ctx);
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {
    }
  }
  if (OB_SUCC(ret)) {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
    batch_guard.set_batch_size(batch_size);
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObDatumVector models = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector texts = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObArray<int64_t> rows;
    ObArray<ObString> model_ids;
    ObArray<ObString> contents;
    ObArray<int64_t> dimensions;
    ObArray<ObString> pending_results;
    // Collect actual SQL rows; constants use datumvector's scalar addressing.
    for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
      if (skip.at(row) || evaluated.at(row) || texts.at(row)->is_null()) {
        continue;
      }
      batch_guard.set_batch_idx(row);
      const ObDatum *dim = expr.arg_cnt_ == 3
          ? expr.args_[DIM_IDX]->locate_expr_datumvector(ctx).at(row) : nullptr;
      ObString model_id;
      ObString text;
      int64_t dimension = 0;
      if (OB_FAIL(read_embed_arguments(expr, ctx, allocator, *models.at(row), *texts.at(row),
                                      dim, model_id, text, dimension))) {
      } else if (OB_FAIL(rows.push_back(row))) {
      } else if (OB_FAIL(model_ids.push_back(model_id))) {
      } else if (OB_FAIL(contents.push_back(text))) {
      } else if (OB_FAIL(dimensions.push_back(dimension))) {
      } else if (OB_FAIL(pending_results.push_back(ObString()))) {
      }
    }
    // A request can contain only one model and dimension. Stage all responses
    // before publishing any evaluated flags, including when a later group fails.
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      if (!pending_results.at(i).empty()) {
        continue;
      }
      ObArray<int64_t> positions;
      ObArray<ObString> request;
      ObArray<ObString> response;
      for (int64_t j = i; OB_SUCC(ret) && j < rows.count(); ++j) {
        if (pending_results.at(j).empty() && model_ids.at(j) == model_ids.at(i)
            && dimensions.at(j) == dimensions.at(i)) {
          if (OB_FAIL(positions.push_back(j))) {
          } else if (OB_FAIL(request.push_back(contents.at(j)))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(embed_contents(allocator, model_ids.at(i), dimensions.at(i), request, response))) {
      } else {
        for (int64_t j = 0; j < response.count(); ++j) {
          pending_results.at(positions.at(j)) = response.at(j);
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      batch_guard.set_batch_idx(rows.at(i));
      if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, result_datums[rows.at(i)],
                                                 pending_results.at(i)))) {
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t row = 0; row < batch_size; ++row) {
        if (!skip.at(row) && !evaluated.at(row)) {
          if (texts.at(row)->is_null()) {
            result_datums[row].set_null();
          }
          evaluated.set(row);
        }
      }
    }
  }
  return ret;
}
int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
