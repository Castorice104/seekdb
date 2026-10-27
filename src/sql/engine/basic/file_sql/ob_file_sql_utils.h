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
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "sql/resolver/dml/ob_file_table_def.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum JsonFormatType
{
  JSON_FORMAT_AUTO = 0,
  JSON_FORMAT_NDJSON = 1,
  JSON_FORMAT_ARRAY = 2,
};

struct ObFileSqlOptions
{
  ObFileSqlOptions()
    : header_(true), delimiter_(','), quote_('"'), escape_('"'), has_escape_(true),
      nullstr_(), sample_size_(-1), json_format_(JSON_FORMAT_AUTO) {}
  bool header_;
  char delimiter_;
  char quote_;
  char escape_;
  bool has_escape_;
  common::ObString nullstr_;
  int64_t sample_size_;
  int json_format_;
};

struct ObFileSqlColumnMeta
{
  ObFileSqlColumnMeta()
    : obj_type_(common::ObNullType), collation_(common::CS_TYPE_UTF8MB4_GENERAL_CI),
      nullable_(true), max_length_(0) {}
  common::ObString name_;
  common::ObObjType obj_type_;
  common::ObCollationType collation_;
  bool nullable_;
  int64_t max_length_;
  common::ObString type_name_;
};

struct ObFileSqlIdentity
{
  ObFileSqlIdentity() : size_(0), mtime_(0), inode_(0), device_(0) {}
  int64_t size_;
  int64_t mtime_;
  int64_t inode_;
  int64_t device_;
};

struct ObFileSqlValue
{
  ObFileSqlValue() : is_null_(true), int_value_(0), double_value_(0.0), string_value_() {}
  bool is_null_;
  int64_t int_value_;
  double double_value_;
  common::ObString string_value_;
};

// Infer the schema of a file. The whole file is scanned (sample_size is always -1).
int infer_file_schema(common::ObIAllocator &alloc,
                      FileSqlFormatType format,
                      const common::ObString &path,
                      const ObFileSqlOptions &options,
                      common::ObIArray<ObFileSqlColumnMeta> &columns,
                      ObFileSqlIdentity &identity,
                      char *errbuf,
                      const int64_t errbuf_len);

// A streaming reader over one file. next_row() fills exactly the projected columns,
// in ascending column order. Values stay valid until the next call.
class ObFileSqlReader
{
public:
  ObFileSqlReader() : alloc_(NULL), row_() {}
  virtual ~ObFileSqlReader() {}
  virtual int next_row() = 0;
  virtual int rewind() = 0;
  virtual int64_t row_count() const { return -1; }
  const common::ObIArray<ObFileSqlValue> &get_row() const { return row_; }
  void set_reader_allocator(common::ObIAllocator *alloc) { alloc_ = alloc; }
protected:
  common::ObIAllocator *alloc_;
  common::ObSEArray<ObFileSqlValue, 64> row_;
};

int create_file_reader(common::ObIAllocator &alloc,
                       FileSqlFormatType format,
                       const common::ObString &path,
                       const ObFileSqlOptions &options,
                       const common::ObIArray<ObFileSqlColumnMeta> &columns,
                       const common::ObIArray<int64_t> &projection,
                       ObFileSqlReader *&reader,
                       char *errbuf,
                       const int64_t errbuf_len);

// Open the file and return its identity, used to detect changes between resolve and execution.
int get_file_identity(const common::ObString &path,
                      ObFileSqlIdentity &identity,
                      char *errbuf,
                      const int64_t errbuf_len);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
