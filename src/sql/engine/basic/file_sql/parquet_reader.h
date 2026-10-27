#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

enum class ParquetLogicalType {
  INT32_TYPE,
  INT64_TYPE,
  BOOL_TYPE,
  FLOAT_TYPE,
  DOUBLE_TYPE,
  STRING_TYPE,
  DATE_TYPE,
  DATETIME_TYPE,
  UNSUPPORTED_TYPE
};

struct ParquetColumn {
  std::string name;
  ParquetLogicalType type;
  bool nullable;
};

struct ParquetValue {
  bool is_null = false;
  int64_t int_value = 0;      // INT32/INT64/BOOL(0/1)/DATE(days since epoch)/DATETIME(seconds since epoch)
  double double_value = 0.0;  // FLOAT/DOUBLE
  std::string string_value;   // STRING (UTF-8 bytes)
};

// Dependency-free Parquet reader (standard library only).
//
// Supported: flat schemas, UNCOMPRESSED and SNAPPY, data page v1/v2,
// PLAIN / PLAIN_DICTIONARY / RLE_DICTIONARY values, RLE and BIT_PACKED
// definition levels, multiple row groups, column pruning.
// Unsupported columns (nested groups, repeated fields, INT96, DECIMAL,
// FIXED_LEN_BYTE_ARRAY, unsupported codecs/encodings) are reported as
// ParquetLogicalType::UNSUPPORTED_TYPE; reading them fails with a message in
// error() instead of crashing.
class ParquetReader {
 public:
  ParquetReader();
  ~ParquetReader();

  ParquetReader(const ParquetReader &) = delete;
  ParquetReader &operator=(const ParquetReader &) = delete;

  // 0 on success, negative on failure; human-readable reason in error().
  int open(const std::string &path);
  int set_projected_columns(const std::vector<int> &column_indexes);  // optional, before or after open is fine; -1 or empty means all columns
  const std::vector<ParquetColumn> &columns() const;
  int64_t num_rows() const;
  // 0 = row filled, 1 = end of file, negative = error. Row is filled in projected-column order, and has exactly projected column count (or all columns).
  int next_row(std::vector<ParquetValue> &row);
  int rewind();
  const std::string &error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
