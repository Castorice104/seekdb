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
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include <cmath>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

namespace
{
struct RerankResult
{
  int64_t index_;
  double score_;
};

// Validate the complete batch before using any provider-supplied index.
int append_rerank_batch(ObJsonArray *response, const int64_t count,
                        const int64_t base, ObArray<RerankResult> &results)
{
  int ret = OB_SUCCESS;
  ObArray<bool> seen;
  if (nullptr == response || response->json_type() != ObJsonNodeType::J_ARRAY
      || response->element_count() != count) {
    ret = OB_INVALID_DATA;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    if (OB_FAIL(seen.push_back(false))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObJsonNode *node = response->get_value(i);
    if (nullptr == node || node->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else {
      ObJsonObject *object = static_cast<ObJsonObject *>(node);
      ObJsonNode *index = object->get_value("index");
      ObJsonNode *score = object->get_value("relevance_score");
      int64_t local_index = -1;
      double value = 0;
      if (nullptr == index || nullptr == score) {
        ret = OB_INVALID_DATA;
      } else if (index->json_type() == ObJsonNodeType::J_INT) {
        local_index = index->get_int();
      } else if (index->json_type() == ObJsonNodeType::J_UINT
                 && index->get_uint() < static_cast<uint64_t>(count)) {
        local_index = static_cast<int64_t>(index->get_uint());
      } else {
        ret = OB_INVALID_DATA;
      }
      if (OB_FAIL(ret)) {
      } else if (local_index < 0 || local_index >= count || seen.at(local_index)) {
        ret = OB_INVALID_DATA;
      } else if (score->json_type() != ObJsonNodeType::J_INT
                 && score->json_type() != ObJsonNodeType::J_UINT
                 && score->json_type() != ObJsonNodeType::J_DOUBLE
                 && score->json_type() != ObJsonNodeType::J_DECIMAL) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(score->to_double(value))) {
        ret = OB_INVALID_DATA;
      } else if (!std::isfinite(value)) {
        ret = OB_INVALID_DATA;
      } else {
        seen.at(local_index) = true;
        RerankResult result{base + local_index, value};
        if (OB_FAIL(results.push_back(result))) {
        }
      }
    }
  }
  if (ret == OB_INVALID_DATA) {
    LOG_USER_ERROR(OB_INVALID_DATA, "ai_rerank, invalid response count, index or relevance_score");
  }
  return ret;
}
} // namespace

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *arg_model = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_key = nullptr;
  if (expr.arg_cnt_ == 4
      ? OB_FAIL(expr.eval_param_value(ctx, arg_model, arg_query, arg_documents, arg_key))
      : OB_FAIL(expr.eval_param_value(ctx, arg_model, arg_query, arg_documents))) {
  } else if (arg_model->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObString model_id;
    ObString query;
    ObString key("text");
    const bool explicit_key = nullptr != arg_key && !arg_key->is_null();
    ObIJsonBase *documents_base = nullptr;
    bool is_null = false;
    ObJsonArray *documents = nullptr;
    ObJsonArray *texts = nullptr;
    ObJsonArray *output = nullptr;
    bool object_documents = false;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                    *arg_model, expr.args_[MODEL_IDX]->datum_meta_,
                    expr.args_[MODEL_IDX]->obj_meta_.has_lob_header(), model_id))) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                    *arg_query, expr.args_[QUERY_IDX]->datum_meta_,
                    expr.args_[QUERY_IDX]->obj_meta_.has_lob_header(), query))) {
    } else if (explicit_key && OB_FAIL(ObTextStringHelper::read_real_string_data(
                    ctx.exec_ctx_, allocator, *arg_key, expr.args_[DOC_KEY_IDX]->datum_meta_,
                    expr.args_[DOC_KEY_IDX]->obj_meta_.has_lob_header(), key))) {
    } else if (model_id.empty() || key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or document key is empty");
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator,
                    DOCUMENTS_IDX, documents_base, is_null))) {
    } else if (nullptr == documents_base || documents_base->json_type() != ObJsonNodeType::J_ARRAY
               || documents_base->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, documents must be a nonempty array");
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, texts))) {
    } else {
      documents = static_cast<ObJsonArray *>(documents_base);
      object_documents = nullptr != documents->get_value(0)
          && documents->get_value(0)->json_type() == ObJsonNodeType::J_OBJECT;
      for (int64_t i = 0; OB_SUCC(ret) && i < documents->element_count(); ++i) {
        ObJsonNode *document = documents->get_value(i);
        ObJsonNode *text = document;
        if (nullptr == document) {
          ret = OB_INVALID_ARGUMENT;
        } else if (object_documents) {
          if (document->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            text = static_cast<ObJsonObject *>(document)->get_value(key);
          }
        } else if (explicit_key) {
          ret = OB_INVALID_ARGUMENT;
        }
        if (OB_FAIL(ret)) {
        } else if (nullptr == text || text->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(texts->append(text))) {
        }
      }
      if (ret == OB_INVALID_ARGUMENT) {
        LOG_USER_ERROR(OB_INVALID_ARGUMENT,
                       "ai_rerank, documents must be strings or objects with a string document key");
      }
    }
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver =
        share::server_service<query::ObIAiEndpointResolver>();
    ObArray<RerankResult> results;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
    } else if (nullptr == resolver || nullptr == info) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
    } else {
      ObAIFuncModel model(allocator, *info, endpoint);
      const int64_t batch_size = 20;
      for (int64_t base = 0; OB_SUCC(ret) && base < texts->element_count(); base += batch_size) {
        const int64_t count = std::min<int64_t>(batch_size, texts->element_count() - base);
        ObJsonArray *batch = nullptr;
        ObJsonArray *response = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, batch))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          if (OB_FAIL(batch->append(texts->get_value(base + i)))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_rerank(query, batch, response))) {
        } else if (OB_FAIL(append_rerank_batch(response, count, base, results))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      // Original index is the tie breaker, independent of provider response order.
      std::sort(&results.at(0), &results.at(0) + results.count(),
                [](const RerankResult &left, const RerankResult &right) {
                  return left.score_ != right.score_ ? left.score_ > right.score_
                                                    : left.index_ < right.index_;
                });
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < results.count(); ++i) {
        const RerankResult &rank = results.at(i);
        ObJsonObject *item = nullptr;
        ObJsonDouble *score = OB_NEWx(ObJsonDouble, &allocator, rank.score_);
        if (nullptr == score) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (object_documents) {
          item = static_cast<ObJsonObject *>(documents->get_value(rank.index_)->clone(&allocator, true));
          if (nullptr == item) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(item->add(MODEL_SCORE_KEY, score))) {
          }
        } else {
          ObJsonInt *index = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, item))) {
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, rank.index_, index))) {
          } else if (OB_FAIL(item->add(INDEX_KEY, index))) {
          } else if (OB_FAIL(item->add(SCORE_KEY, score))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(output->append(item))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString binary;
      if (OB_FAIL(output->get_raw_binary(binary, &allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, binary))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   bool got_data = false;
  //   ObObj const_data;
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
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
