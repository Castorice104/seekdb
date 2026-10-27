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

#include "sql/engine/basic/ob_file_scan_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/resolver/ob_resolver_utils.h"

namespace oceanbase
{
namespace sql
{

using namespace common;

OB_SERIALIZE_MEMBER((ObFileScanSpec, ObOpSpec),
                    format_,
                    path_,
                    header_,
                    delimiter_,
                    quote_,
                    escape_,
                    has_escape_,
                    nullstr_,
                    json_format_,
                    file_size_,
                    file_mtime_,
                    file_inode_,
                    file_device_,
                    column_names_,
                    column_types_,
                    column_collations_,
                    column_nullable_,
                    column_max_lengths_,
                    column_exprs_,
                    column_idxs_);


void ObFileScanOp::release_reader()
{
  if (NULL != reader_) {
    delete reader_;
    reader_ = NULL;
  }
}

int ObFileScanOp::build_columns()
{
  int ret = OB_SUCCESS;
  columns_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_types_.count(); ++i) {
    file_sql::ObFileSqlColumnMeta column;
    column.name_ = MY_SPEC.column_names_.at(i);
    column.obj_type_ = static_cast<ObObjType>(MY_SPEC.column_types_.at(i));
    column.collation_ = static_cast<ObCollationType>(MY_SPEC.column_collations_.at(i));
    column.nullable_ = 0 != MY_SPEC.column_nullable_.at(i);
    column.max_length_ = MY_SPEC.column_max_lengths_.at(i);
    if (OB_FAIL(columns_.push_back(column))) {
    }
  }
  return ret;
}

int ObFileScanOp::build_projection()
{
  int ret = OB_SUCCESS;
  projection_.reset();
  pos_map_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_idxs_.count(); ++i) {
    const int64_t column_idx = MY_SPEC.column_idxs_.at(i);
    int64_t pos = -1;
    for (int64_t j = 0; j < projection_.count(); ++j) {
      if (projection_.at(j) == column_idx) {
        pos = j;
        break;
      }
    }
    if (-1 == pos) {
      pos = projection_.count();
      if (OB_FAIL(projection_.push_back(column_idx))) {
        break;
      }
    }
    if (OB_FAIL(pos_map_.push_back(pos))) {
      break;
    }
  }
  // keep projected columns in ascending order
  for (int64_t i = 0; OB_SUCC(ret) && i < projection_.count(); ++i) {
    for (int64_t j = i + 1; j < projection_.count(); ++j) {
      if (projection_.at(j) < projection_.at(i)) {
        const int64_t tmp = projection_.at(i);
        projection_.at(i) = projection_.at(j);
        projection_.at(j) = tmp;
      }
    }
  }
  // rebuild position map after sorting
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_idxs_.count(); ++i) {
    for (int64_t j = 0; j < projection_.count(); ++j) {
      if (projection_.at(j) == MY_SPEC.column_idxs_.at(i)) {
        pos_map_.at(i) = j;
        break;
      }
    }
  }
  return ret;
}

int ObFileScanOp::init_reader()
{
  int ret = OB_SUCCESS;
  char errbuf[256];
  errbuf[0] = '\0';
  file_sql::ObFileSqlOptions options;
  options.header_ = MY_SPEC.header_;
  options.delimiter_ = MY_SPEC.delimiter_;
  options.quote_ = MY_SPEC.quote_;
  options.escape_ = MY_SPEC.escape_;
  options.has_escape_ = MY_SPEC.has_escape_;
  options.nullstr_ = MY_SPEC.nullstr_;
  options.json_format_ = static_cast<int>(MY_SPEC.json_format_);
  if (OB_FAIL(file_sql::create_file_reader(ctx_.get_allocator(),
                                           static_cast<FileSqlFormatType>(MY_SPEC.format_),
                                           MY_SPEC.path_,
                                           options,
                                           columns_,
                                           projection_,
                                           reader_,
                                           errbuf,
                                           sizeof(errbuf)))) {
    LOG_WARN("failed to create file scan reader", K(ret), K(errbuf));
  }
  return ret;
}

int ObFileScanOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  bool enable_file_sql = false;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enable_file_sql))) {
  } else if (!enable_file_sql && !session->is_inner()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file table function, please set enable_file_sql = ON");
  } else if (OB_FAIL(build_columns())) {
  } else if (OB_FAIL(build_projection())) {
  }
  if (OB_SUCC(ret) && FILE_SQL_FORMAT_GLOB != MY_SPEC.format_ && !session->is_inner()) {
    ObString secure_file_priv;
    file_sql::ObFileSqlIdentity identity;
    char errbuf[256];
    errbuf[0] = '\0';
    if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, MY_SPEC.path_))) {
    } else if (OB_FAIL(file_sql::get_file_identity(MY_SPEC.path_, identity, errbuf, sizeof(errbuf)))) {
      LOG_WARN("failed to stat file", K(ret), K(MY_SPEC.path_), K(errbuf));
    } else if (identity.size_ != MY_SPEC.file_size_ ||
               identity.mtime_ != MY_SPEC.file_mtime_ ||
               identity.inode_ != MY_SPEC.file_inode_ ||
               identity.device_ != MY_SPEC.file_device_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("file has been changed after the query was resolved", K(ret),
               K(MY_SPEC.path_), K(identity.size_), K(MY_SPEC.file_size_),
               K(identity.mtime_), K(MY_SPEC.file_mtime_));
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("file scan op open failed", K(ret), K(MY_SPEC.path_));
  } else if (OB_FAIL(init_reader())) {
    LOG_WARN("file scan init reader failed", K(ret), K(MY_SPEC.path_));
  }
  return ret;
}

int ObFileScanOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (NULL == reader_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(reader_->rewind())) {
  }
  return ret;
}

int ObFileScanOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(reader_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = reader_->next_row();
    if (OB_SUCCESS == ret) {
      clear_evaluated_flag();
      const ObIArray<file_sql::ObFileSqlValue> &row = reader_->get_row();
      for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
        ObExpr *expr = MY_SPEC.column_exprs_.at(i);
        if (OB_ISNULL(expr) || i >= pos_map_.count() || pos_map_.at(i) >= row.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const file_sql::ObFileSqlValue &value = row.at(pos_map_.at(i));
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (value.is_null_ || ObNullType == expr->datum_meta_.get_type()) {
            datum.set_null();
          } else {
            const ObObjMeta &meta = expr->obj_meta_;
            ObObj obj;
            switch (meta.get_type()) {
              case ObVarcharType:
                obj.set_varchar(value.string_value_);
                obj.set_collation_type(meta.get_collation_type());
                break;
              case ObDoubleType:
              case ObFloatType:
                obj.set_double(value.double_value_);
                break;
              case ObDateType:
                obj.set_date(static_cast<int32_t>(value.int_value_));
                break;
              case ObDateTimeType:
                obj.set_datetime(value.int_value_);
                break;
              case ObTinyIntType:
                obj.set_bool(0 != value.int_value_);
                break;
              default:
                obj.set_int(value.int_value_);
                break;
            }
            if (OB_FAIL(datum.from_obj(obj, expr->obj_datum_map_))) {
              LOG_WARN("failed to convert file value to datum", K(ret), K(obj));
            }
          }
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
    } else if (OB_ITER_END == ret) {
      // do nothing
    }
  }
  return ret;
}

int ObFileScanOp::inner_close()
{
  int ret = OB_SUCCESS;
  release_reader();
  return ret;
}

void ObFileScanOp::destroy()
{
  release_reader();
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
