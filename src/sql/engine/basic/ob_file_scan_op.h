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

#include "sql/engine/ob_operator.h"
#include "lib/container/ob_array_serialization.h"
#include "sql/engine/basic/file_sql/ob_file_sql_utils.h"

namespace oceanbase
{
namespace sql
{

class ObFileScanSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileScanSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      format_(0),
      path_(),
      header_(true),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      has_escape_(true),
      nullstr_(),
      json_format_(0),
      file_size_(0),
      file_mtime_(0),
      file_inode_(0),
      file_device_(0),
      column_names_(alloc),
      column_types_(alloc),
      column_collations_(alloc),
      column_nullable_(alloc),
      column_max_lengths_(alloc),
      column_exprs_(alloc),
      column_idxs_(alloc)
  {}
  virtual ~ObFileScanSpec() {}

  int64_t format_;
  common::ObString path_;
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  bool has_escape_;
  common::ObString nullstr_;
  int64_t json_format_;
  int64_t file_size_;
  int64_t file_mtime_;
  int64_t file_inode_;
  int64_t file_device_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> column_names_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_types_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_collations_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_nullable_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_max_lengths_;
  // expressions produced by this operator, one per accessed file column
  ExprFixedArray column_exprs_;
  // file column index of each expression in column_exprs_
  common::ObFixedArray<int64_t, common::ObIAllocator> column_idxs_;
};

class ObFileScanOp : public ObOperator
{
public:
  ObFileScanOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input), reader_(NULL), pos_map_(), columns_(), projection_()
  {}
  virtual ~ObFileScanOp() { release_reader(); }
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  void release_reader();
  int build_columns();
  int build_projection();
  int init_reader();
  file_sql::ObFileSqlReader *reader_;
  common::ObSEArray<int64_t, 16> pos_map_;
  common::ObSEArray<file_sql::ObFileSqlColumnMeta, 32> columns_;
  common::ObSEArray<int64_t, 32> projection_;
};

} // namespace sql
} // namespace oceanbase
