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

#include "lib/string/ob_string.h"
#include "lib/ob_define.h"
#include "common/object/ob_object.h"
#include "share/schema/ob_column_schema.h"
#include "lib/utility/serialization.h"

namespace oceanbase
{
namespace sql
{

// Resolved definition of one AI column. It is carried by the DDL statement
// from the resolver to the executor, which persists it into
// oceanbase.__all_ai_column and creates the hidden state/request/spec columns.
struct ObAiColumnInfo
{
  enum AiColumnType
  {
    AI_COLUMN_INVALID = 0,
    AI_COLUMN_COMPLETE = 1,
    AI_COLUMN_EMBED = 2,
  };
  ObAiColumnInfo()
    : column_name_(), column_id_(common::OB_INVALID_ID), ai_type_(AI_COLUMN_INVALID),
      result_type_(common::ObNullType), vector_dim_(0), max_retries_(0), sync_mode_(0),
      state_column_id_(common::OB_INVALID_ID), request_column_id_(common::OB_INVALID_ID),
      result_spec_column_id_(common::OB_INVALID_ID), expr_text_(), normalized_expr_(),
      depends_columns_(), definition_version_(1)
  {}
  bool is_valid() const
  {
    return !column_name_.empty() && common::OB_INVALID_ID != column_id_
        && AI_COLUMN_INVALID != ai_type_;
  }
  TO_STRING_KV(K(column_name_), K(column_id_), K(ai_type_), K(result_type_),
               K(vector_dim_), K(max_retries_), K(sync_mode_), K(state_column_id_),
               K(request_column_id_), K(result_spec_column_id_));
  common::ObString column_name_;
  uint64_t column_id_;
  int64_t ai_type_;
  common::ObObjType result_type_;
  int64_t vector_dim_;
  int64_t max_retries_;
  int64_t sync_mode_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString expr_text_;
  common::ObString normalized_expr_;
  common::ObString depends_columns_;
  int64_t definition_version_;
};

inline int set_ai_column_info(share::schema::ObColumnSchemaV2 &column,
                              common::ObIAllocator &alloc, const ObAiColumnInfo &info)
{
  int ret = common::OB_SUCCESS;
  const int64_t format_version = 1;
  int64_t size = common::serialization::encoded_length(format_version);
  size += common::serialization::encoded_length(info.column_name_);
  size += common::serialization::encoded_length(info.column_id_);
  size += common::serialization::encoded_length(info.ai_type_);
  size += common::serialization::encoded_length(info.result_type_);
  size += common::serialization::encoded_length(info.vector_dim_);
  size += common::serialization::encoded_length(info.max_retries_);
  size += common::serialization::encoded_length(info.sync_mode_);
  size += common::serialization::encoded_length(info.state_column_id_);
  size += common::serialization::encoded_length(info.request_column_id_);
  size += common::serialization::encoded_length(info.result_spec_column_id_);
  size += common::serialization::encoded_length(info.expr_text_);
  size += common::serialization::encoded_length(info.normalized_expr_);
  size += common::serialization::encoded_length(info.depends_columns_);
  size += common::serialization::encoded_length(info.definition_version_);
  char *buf = static_cast<char *>(alloc.alloc(size));
  int64_t pos = 0;
  if (OB_ISNULL(buf)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ret = common::serialization::encode(buf, size, pos, format_version);
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.column_name_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.column_id_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.ai_type_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.result_type_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.vector_dim_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.max_retries_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.sync_mode_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.state_column_id_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.request_column_id_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.result_spec_column_id_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.expr_text_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.normalized_expr_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.depends_columns_); }
    if (OB_SUCC(ret)) { ret = common::serialization::encode(buf, size, pos, info.definition_version_); }
    if (OB_SUCC(ret)) { ret = column.set_ai_column_info(common::ObString(pos, buf)); }
  }
  return ret;
}

inline int get_ai_column_info(const share::schema::ObColumnSchemaV2 &column,
                              common::ObIAllocator &alloc, ObAiColumnInfo &info)
{
  int ret = common::OB_SUCCESS;
  const common::ObString &encoded = column.get_ai_column_info_str();
  int64_t pos = 0;
  int64_t version = 0;
  info = ObAiColumnInfo();
  if (!column.is_ai_column()) {
    ret = common::OB_ENTRY_NOT_EXIST;
  } else if (OB_FAIL(common::serialization::decode(encoded.ptr(), encoded.length(), pos, version))) {
  } else if (1 != version) {
    ret = common::OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.column_name_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.column_id_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.ai_type_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.result_type_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.vector_dim_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.max_retries_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.sync_mode_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.state_column_id_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.request_column_id_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.result_spec_column_id_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.expr_text_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.normalized_expr_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.depends_columns_); }
  if (OB_SUCC(ret)) { ret = common::serialization::decode(encoded.ptr(), encoded.length(), pos, info.definition_version_); }
  if (OB_SUCC(ret) && pos != encoded.length()) { ret = common::OB_DESERIALIZE_ERROR; }
  if (OB_SUCC(ret)) { common::ObString value = info.column_name_; ret = common::ob_write_string(alloc, value, info.column_name_); }
  if (OB_SUCC(ret)) { common::ObString value = info.expr_text_; ret = common::ob_write_string(alloc, value, info.expr_text_); }
  if (OB_SUCC(ret)) { common::ObString value = info.normalized_expr_; ret = common::ob_write_string(alloc, value, info.normalized_expr_); }
  if (OB_SUCC(ret)) { common::ObString value = info.depends_columns_; ret = common::ob_write_string(alloc, value, info.depends_columns_); }
  // Column IDs/names may be assigned or changed by the DDL service.
  info.column_id_ = column.get_column_id();
  if (OB_SUCC(ret)) { ret = common::ob_write_string(alloc, column.get_column_name_str(), info.column_name_); }
  return ret;
}

} // namespace sql
} // namespace oceanbase
