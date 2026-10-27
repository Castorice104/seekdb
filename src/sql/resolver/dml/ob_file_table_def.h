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
#include "lib/container/ob_se_array.h"
#include "lib/allocator/page_arena.h"
#include "sql/resolver/expr/ob_raw_expr.h"

namespace oceanbase
{
namespace sql
{

enum FileSqlFormatType
{
  FILE_SQL_FORMAT_INVALID = 0,
  FILE_SQL_FORMAT_CSV = 1,
  FILE_SQL_FORMAT_JSONL = 2,
  FILE_SQL_FORMAT_PARQUET = 3,
  FILE_SQL_FORMAT_GLOB = 4,
};

struct ObFileTableColumnDef
{
  ObFileTableColumnDef() : res_type_(), nullable_(true), max_length_(0) {}
  common::ObString name_;
  // result type of the column, includes type, collation and accuracy
  ObExprResType res_type_;
  bool nullable_;
  int64_t max_length_;
  // duckdb style type name used by DESCRIBE, e.g. BIGINT/DOUBLE/BOOLEAN/VARCHAR/DATE/DATETIME/NULL
  common::ObString type_name_;
  TO_STRING_KV(K(name_), K(res_type_), K(nullable_), K(max_length_), K(type_name_));
};

// Metadata of a file table function, resolved and validated at resolve time.
class ObFileTableDef
{
public:
  ObFileTableDef()
    : format_(FILE_SQL_FORMAT_INVALID),
      path_(),
      header_(true),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      has_escape_(true),
      nullstr_(),
      sample_size_(-1),
      json_format_(0),
      file_size_(0),
      file_mtime_(0),
      file_inode_(0),
      file_device_(0)
  {}
  ~ObFileTableDef() {}
  bool is_valid() const { return FILE_SQL_FORMAT_INVALID != format_; }
  TO_STRING_KV(K_(format), K_(path), K_(header), K_(delimiter), K_(quote), K_(escape),
               K_(has_escape), K_(nullstr), K_(sample_size), K_(file_size), K_(file_mtime),
               K_(file_inode), K_(file_device), K_(columns));

public:
  FileSqlFormatType format_;
  common::ObString path_;
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  bool has_escape_;
  common::ObString nullstr_;
  int64_t sample_size_;
  int64_t json_format_;
  // file identity used to detect changes between resolve and execution
  int64_t file_size_;
  int64_t file_mtime_;
  int64_t file_inode_;
  int64_t file_device_;
  common::ObSEArray<ObFileTableColumnDef, 32, common::ModulePageAllocator, true> columns_;
};

} // namespace sql
} // namespace oceanbase
