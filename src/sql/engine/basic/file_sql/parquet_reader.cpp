// Copyright (c) 2025 OceanBase.
//
// Dependency-free Parquet reader (C++17, standard library only).
// See parquet_reader.h for the public API and the supported feature set.

#include "parquet_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {

constexpr int64_t kMaxPageValues = 8 * 1024 * 1024;
constexpr int64_t kMaxPageUncompressedBytes = 64 * 1024 * 1024;
constexpr int64_t kMaxDictionaryValues = 8 * 1024 * 1024;
constexpr uint32_t kMaxListElements = 100 * 1000 * 1000;
constexpr uint32_t kMaxSchemaElements = 1000 * 1000;
constexpr uint32_t kMaxRowGroups = 1000 * 1000;
constexpr uint32_t kMaxRowGroupColumns = 1000 * 1000;
constexpr int kMaxThriftDepth = 32;
constexpr int kMaxSchemaDepth = 64;

// ---------------------------------------------------------------------------
// Parquet enums (subset that we understand)
// ---------------------------------------------------------------------------

enum PhysicalType {
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7
};

enum ValueEncoding {
  ENC_PLAIN = 0,
  ENC_PLAIN_DICTIONARY = 2,
  ENC_RLE = 3,
  ENC_BIT_PACKED = 4,
  ENC_RLE_DICTIONARY = 8
};

enum CompressionCodec { CODEC_UNCOMPRESSED = 0, CODEC_SNAPPY = 1 };

enum PageType {
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3
};

enum RepetitionType { REP_REQUIRED = 0, REP_OPTIONAL = 1, REP_REPEATED = 2 };

enum ConvertedType {
  CONV_UTF8 = 0,
  CONV_DECIMAL = 5,
  CONV_DATE = 6,
  CONV_TIMESTAMP_MILLIS = 9,
  CONV_TIMESTAMP_MICROS = 10
};

// Thrift compact protocol types.
enum CompactType {
  CT_STOP = 0,
  CT_BOOLEAN_TRUE = 1,
  CT_BOOLEAN_FALSE = 2,
  CT_BYTE = 3,
  CT_I16 = 4,
  CT_I32 = 5,
  CT_I64 = 6,
  CT_DOUBLE = 7,
  CT_BINARY = 8,
  CT_LIST = 9,
  CT_SET = 10,
  CT_MAP = 11,
  CT_STRUCT = 12
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

uint32_t load_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t load_le64(const uint8_t *p) {
  return static_cast<uint64_t>(load_le32(p)) | (static_cast<uint64_t>(load_le32(p + 4)) << 32);
}

int64_t floor_div(int64_t value, int64_t divisor) {
  int64_t quotient = value / divisor;
  const int64_t remainder = value % divisor;
  if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
    --quotient;
  }
  return quotient;
}

std::string int_to_string(int64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  return std::string(buf);
}

std::string join_path(const std::vector<std::string> &path) {
  std::string result;
  for (const std::string &part : path) {
    if (!result.empty()) {
      result.push_back('.');
    }
    result += part;
  }
  return result;
}

// ---------------------------------------------------------------------------
// File access (bounded pread-like helper, never maps the whole file)
// ---------------------------------------------------------------------------

class FileReader {
 public:
  FileReader() = default;
  ~FileReader() { close(); }

  FileReader(const FileReader &) = delete;
  FileReader &operator=(const FileReader &) = delete;

  bool open(const std::string &path, std::string &error) {
    close();
    fp_ = std::fopen(path.c_str(), "rb");
    if (fp_ == nullptr) {
      error = "cannot open file: " + path;
      return false;
    }
    if (!file_size(fp_, size_)) {
      error = "cannot determine size of file: " + path;
      close();
      return false;
    }
    return true;
  }

  void close() {
    if (fp_ != nullptr) {
      std::fclose(fp_);
      fp_ = nullptr;
    }
    size_ = 0;
  }

  int64_t size() const { return size_; }

  bool read_at(int64_t offset, void *buffer, size_t length, std::string &error) {
    if (offset < 0 || offset > size_ || length > static_cast<size_t>(size_ - offset)) {
      error = "read outside of file bounds";
      return false;
    }
    if (length == 0) {
      return true;
    }
    if (fp_ == nullptr) {
      error = "file is not open";
      return false;
    }
    if (!seek_set(fp_, offset)) {
      error = "seek failed";
      return false;
    }
    if (std::fread(buffer, 1, length, fp_) != length) {
      error = "unexpected end of file";
      return false;
    }
    return true;
  }

 private:
  static bool seek_set(std::FILE *fp, int64_t offset) {
#if defined(_WIN32)
    return _fseeki64(fp, offset, SEEK_SET) == 0;
#else
    // Linux/Unix is LP64, so long is 64-bit and std::fseek is sufficient.
    return std::fseek(fp, static_cast<long>(offset), SEEK_SET) == 0;
#endif
  }

  static bool file_size(std::FILE *fp, int64_t &size) {
#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
      return false;
    }
    const int64_t result = _ftelli64(fp);
#else
    if (std::fseek(fp, 0, SEEK_END) != 0) {
      return false;
    }
    const long result = std::ftell(fp);
#endif
    if (result < 0) {
      return false;
    }
    size = static_cast<int64_t>(result);
    return true;
  }

  std::FILE *fp_ = nullptr;
  int64_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Minimal Thrift Compact Protocol reader
// ---------------------------------------------------------------------------

class CompactReader {
 public:
  CompactReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return !bad_; }
  void mark_bad() { bad_ = true; }
  size_t pos() const { return pos_; }
  size_t remaining() const { return (bad_ || pos_ > size_) ? 0 : size_ - pos_; }

  bool byte(uint8_t &value) {
    if (!ensure(1)) {
      return false;
    }
    value = data_[pos_++];
    return true;
  }

  bool varint(uint64_t &value) {
    value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
      uint8_t b = 0;
      if (!byte(b)) {
        return false;
      }
      if (i == 9 && (b & 0xfe) != 0) {
        bad_ = true;
        return false;
      }
      value |= static_cast<uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) {
        return true;
      }
      shift += 7;
    }
    bad_ = true;
    return false;
  }

  bool zigzag(int64_t &value) {
    uint64_t raw = 0;
    if (!varint(raw)) {
      return false;
    }
    value = static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
    return true;
  }

  bool i32(int32_t &value) {
    int64_t raw = 0;
    if (!zigzag(raw)) {
      return false;
    }
    if (raw < std::numeric_limits<int32_t>::min() || raw > std::numeric_limits<int32_t>::max()) {
      bad_ = true;
      return false;
    }
    value = static_cast<int32_t>(raw);
    return true;
  }

  bool i64(int64_t &value) { return zigzag(value); }

  bool dbl(double &value) {
    if (!ensure(8)) {
      return false;
    }
    uint64_t bits = load_le64(data_ + pos_);
    pos_ += 8;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  bool binary(std::string &value) {
    uint64_t length = 0;
    if (!varint(length)) {
      return false;
    }
    if (length > remaining() || length > (static_cast<uint64_t>(1) << 30)) {
      bad_ = true;
      return false;
    }
    value.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(length));
    pos_ += static_cast<size_t>(length);
    return true;
  }

  bool list_header(uint8_t &element_type, uint32_t &size) {
    uint8_t b = 0;
    if (!byte(b)) {
      return false;
    }
    element_type = static_cast<uint8_t>(b & 0x0f);
    uint32_t count = static_cast<uint32_t>(b >> 4);
    if (count == 15) {
      uint64_t raw = 0;
      if (!varint(raw) || raw > kMaxListElements) {
        bad_ = true;
        return false;
      }
      count = static_cast<uint32_t>(raw);
    }
    // Every compact-protocol element consumes at least one byte.
    if (count > kMaxListElements || count > remaining()) {
      bad_ = true;
      return false;
    }
    size = count;
    return true;
  }

  bool skip(uint8_t type) { return skip_impl(type, 0); }

 private:
  bool ensure(size_t length) {
    if (bad_ || pos_ > size_ || length > size_ - pos_) {
      bad_ = true;
      return false;
    }
    return true;
  }

  bool skip_impl(uint8_t type, int depth);

  const uint8_t *data_;
  size_t size_;
  size_t pos_ = 0;
  bool bad_ = false;
};

class CompactStructReader {
 public:
  explicit CompactStructReader(CompactReader &reader) : reader_(reader) {}

  bool next(int16_t &field_id, uint8_t &type, bool &stop) {
    uint8_t b = 0;
    if (!reader_.byte(b)) {
      stop = true;
      return false;
    }
    if (b == CT_STOP) {
      stop = true;
      return true;
    }
    type = static_cast<uint8_t>(b & 0x0f);
    const int delta = (b >> 4) & 0x0f;
    int64_t id = 0;
    if (delta == 0) {
      if (!reader_.zigzag(id)) {
        stop = true;
        return false;
      }
    } else {
      id = static_cast<int64_t>(last_id_) + delta;
    }
    if (id < std::numeric_limits<int16_t>::min() || id > std::numeric_limits<int16_t>::max()) {
      reader_.mark_bad();
      stop = true;
      return false;
    }
    field_id = static_cast<int16_t>(id);
    last_id_ = field_id;
    stop = false;
    return true;
  }

 private:
  CompactReader &reader_;
  int16_t last_id_ = 0;
};

bool CompactReader::skip_impl(uint8_t type, int depth) {
  if (depth > kMaxThriftDepth) {
    bad_ = true;
    return false;
  }
  switch (type) {
    case CT_BOOLEAN_TRUE:
    case CT_BOOLEAN_FALSE:
      return true;
    case CT_BYTE: {
      uint8_t b = 0;
      return byte(b);
    }
    case CT_I16:
    case CT_I32:
    case CT_I64: {
      int64_t v = 0;
      return zigzag(v);
    }
    case CT_DOUBLE: {
      if (!ensure(8)) {
        return false;
      }
      pos_ += 8;
      return true;
    }
    case CT_BINARY: {
      std::string s;
      return binary(s);
    }
    case CT_LIST:
    case CT_SET: {
      uint8_t element_type = 0;
      uint32_t count = 0;
      if (!list_header(element_type, count)) {
        return false;
      }
      for (uint32_t i = 0; i < count; ++i) {
        if (!skip_impl(element_type, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    case CT_MAP: {
      uint64_t count = 0;
      if (!varint(count)) {
        return false;
      }
      if (count > kMaxListElements || count > remaining()) {
        bad_ = true;
        return false;
      }
      if (count == 0) {
        return true;
      }
      uint8_t types = 0;
      if (!byte(types)) {
        return false;
      }
      const uint8_t key_type = static_cast<uint8_t>(types >> 4);
      const uint8_t value_type = static_cast<uint8_t>(types & 0x0f);
      for (uint64_t i = 0; i < count; ++i) {
        if (!skip_impl(key_type, depth + 1) || !skip_impl(value_type, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    case CT_STRUCT: {
      CompactStructReader struct_reader(*this);
      int16_t field_id = 0;
      uint8_t field_type = 0;
      bool stop = false;
      while (struct_reader.next(field_id, field_type, stop)) {
        if (stop) {
          return true;
        }
        if (!skip_impl(field_type, depth + 1)) {
          return false;
        }
      }
      return false;
    }
    default:
      bad_ = true;
      return false;
  }
}

// ---------------------------------------------------------------------------
// Snappy raw block decompression
// ---------------------------------------------------------------------------

bool snappy_decompress(const uint8_t *src, size_t src_len, size_t expected,
                       std::vector<uint8_t> &output, std::string &error) {
  output.clear();
  if (expected > static_cast<size_t>(kMaxPageUncompressedBytes)) {
    error = "page is too large after decompression";
    return false;
  }
  size_t pos = 0;
  uint64_t declared = 0;
  int shift = 0;
  bool have_header = false;
  for (int i = 0; i < 5; ++i) {
    if (pos >= src_len) {
      error = "truncated snappy block header";
      return false;
    }
    const uint8_t b = src[pos++];
    declared |= static_cast<uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) {
      have_header = true;
      break;
    }
    shift += 7;
  }
  if (!have_header) {
    error = "invalid snappy block header";
    return false;
  }
  if (declared != expected) {
    error = "snappy uncompressed size mismatch";
    return false;
  }
  output.resize(expected);
  size_t out_pos = 0;
  while (pos < src_len) {
    const uint8_t tag = src[pos++];
    const uint32_t kind = tag & 0x03;
    if (kind == 0) {
      size_t length = static_cast<size_t>(tag >> 2);
      if (length < 60) {
        length += 1;
      } else {
        const size_t extra = length - 59;
        if (extra > 4 || extra > src_len - pos) {
          error = "truncated snappy literal";
          return false;
        }
        uint32_t long_length = 0;
        for (size_t i = 0; i < extra; ++i) {
          long_length |= static_cast<uint32_t>(src[pos + i]) << (8 * i);
        }
        pos += extra;
        length = static_cast<size_t>(long_length) + 1;
      }
      if (length > src_len - pos || length > expected - out_pos) {
        error = "invalid snappy literal length";
        return false;
      }
      std::memcpy(output.data() + out_pos, src + pos, length);
      out_pos += length;
      pos += length;
    } else {
      size_t length = 0;
      size_t offset = 0;
      if (kind == 1) {
        length = ((static_cast<size_t>(tag) >> 2) & 0x07) + 4;
        if (pos >= src_len) {
          error = "truncated snappy copy";
          return false;
        }
        offset = ((static_cast<size_t>(tag) >> 5) << 8) | src[pos++];
      } else if (kind == 2) {
        length = (static_cast<size_t>(tag) >> 2) + 1;
        if (src_len - pos < 2) {
          error = "truncated snappy copy";
          return false;
        }
        offset = static_cast<size_t>(src[pos]) | (static_cast<size_t>(src[pos + 1]) << 8);
        pos += 2;
      } else {
        length = (static_cast<size_t>(tag) >> 2) + 1;
        if (src_len - pos < 4) {
          error = "truncated snappy copy";
          return false;
        }
        offset = static_cast<size_t>(load_le32(src + pos));
        pos += 4;
      }
      if (offset == 0 || offset > out_pos || length > expected - out_pos) {
        error = "invalid snappy copy";
        return false;
      }
      for (size_t i = 0; i < length; ++i) {
        output[out_pos] = output[out_pos - offset];
        ++out_pos;
      }
    }
  }
  if (out_pos != expected) {
    error = "snappy output size mismatch";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Level decoders
// ---------------------------------------------------------------------------

uint32_t read_bits_le(const uint8_t *data, size_t bit_offset, int bit_width) {
  uint32_t value = 0;
  for (int i = 0; i < bit_width; ++i) {
    const size_t bit = bit_offset + static_cast<size_t>(i);
    value |= static_cast<uint32_t>((data[bit >> 3] >> (bit & 7)) & 1u) << i;
  }
  return value;
}

// RLE / bit-packed hybrid decoder (Parquet "RLE" encoding).
class RleDecoder {
 public:
  RleDecoder(const uint8_t *data, size_t size, int bit_width)
      : data_(data), size_(size), bit_width_(bit_width) {}

  bool get(uint32_t &value) {
    for (;;) {
      if (run_remaining_ > 0) {
        if (is_rle_) {
          value = rle_value_;
          --run_remaining_;
          return true;
        }
        const size_t bit_offset = static_cast<size_t>(run_index_) * static_cast<size_t>(bit_width_);
        value = read_bits_le(data_ + run_byte_pos_, bit_offset, bit_width_);
        ++run_index_;
        --run_remaining_;
        return true;
      }
      if (!load_run()) {
        return false;
      }
    }
  }

 private:
  bool read_varint(uint64_t &value) {
    value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
      if (pos_ >= size_) {
        return false;
      }
      const uint8_t b = data_[pos_++];
      if (i == 9 && (b & 0xfe) != 0) {
        return false;
      }
      value |= static_cast<uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) {
        return true;
      }
      shift += 7;
    }
    return false;
  }

  bool load_run() {
    uint64_t header = 0;
    if (!read_varint(header)) {
      return false;
    }
    if ((header & 1) != 0) {
      const uint64_t groups = header >> 1;
      if (groups == 0 || groups > (static_cast<uint64_t>(1) << 28)) {
        return false;
      }
      const uint64_t values = groups * 8;
      const uint64_t bytes = groups * static_cast<uint64_t>(bit_width_);
      if (bytes > static_cast<uint64_t>(size_ - pos_)) {
        return false;
      }
      run_byte_pos_ = pos_;
      pos_ += static_cast<size_t>(bytes);
      run_remaining_ = values;
      run_index_ = 0;
      is_rle_ = false;
      return true;
    }
    const uint64_t run_length = header >> 1;
    if (run_length == 0 || run_length > (static_cast<uint64_t>(1) << 31)) {
      return false;
    }
    const size_t bytes_per_value = static_cast<size_t>((bit_width_ + 7) / 8);
    if (bytes_per_value > size_ - pos_) {
      return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < bytes_per_value; ++i) {
      value |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
    }
    pos_ += bytes_per_value;
    rle_value_ = value;
    run_remaining_ = run_length;
    is_rle_ = true;
    return true;
  }

  const uint8_t *data_;
  size_t size_;
  int bit_width_;
  size_t pos_ = 0;
  bool is_rle_ = false;
  uint64_t run_remaining_ = 0;
  uint64_t run_index_ = 0;
  size_t run_byte_pos_ = 0;
  uint32_t rle_value_ = 0;
};

// Deprecated BIT_PACKED encoding: values are packed MSB first.
class BitPackedMsbDecoder {
 public:
  BitPackedMsbDecoder(const uint8_t *data, size_t size, int bit_width)
      : data_(data), size_(size), bit_width_(bit_width) {}

  bool get(uint32_t &value) {
    if (bit_pos_ + static_cast<size_t>(bit_width_) > size_ * 8) {
      return false;
    }
    uint32_t result = 0;
    for (int i = 0; i < bit_width_; ++i) {
      const size_t bit = bit_pos_++;
      result = (result << 1) | ((data_[bit >> 3] >> (7 - (bit & 7))) & 1u);
    }
    value = result;
    return true;
  }

 private:
  const uint8_t *data_;
  size_t size_;
  int bit_width_;
  size_t bit_pos_ = 0;
};

int level_bit_width(int max_level) {
  int width = 0;
  while ((1 << width) <= max_level) {
    ++width;
  }
  return width;
}

bool decode_levels_v1(const uint8_t *data, size_t size, int64_t count, int bit_width, int encoding,
                      std::vector<uint8_t> &levels, size_t &consumed, std::string &error) {
  consumed = 0;
  levels.assign(static_cast<size_t>(count), 0);
  if (bit_width == 0) {
    return true;
  }
  if (encoding == ENC_RLE) {
    if (size < 4) {
      error = "truncated definition levels";
      return false;
    }
    const uint32_t length = load_le32(data);
    if (static_cast<uint64_t>(length) + 4 > size) {
      error = "definition levels exceed the page";
      return false;
    }
    RleDecoder decoder(data + 4, length, bit_width);
    for (int64_t i = 0; i < count; ++i) {
      uint32_t value = 0;
      if (!decoder.get(value)) {
        error = "truncated RLE definition levels";
        return false;
      }
      levels[static_cast<size_t>(i)] = static_cast<uint8_t>(value);
    }
    consumed = static_cast<size_t>(length) + 4;
    return true;
  }
  if (encoding == ENC_BIT_PACKED) {
    const uint64_t bits = static_cast<uint64_t>(count) * static_cast<uint64_t>(bit_width);
    const size_t bytes = static_cast<size_t>((bits + 7) / 8);
    if (bytes > size) {
      error = "truncated bit-packed definition levels";
      return false;
    }
    BitPackedMsbDecoder decoder(data, bytes, bit_width);
    for (int64_t i = 0; i < count; ++i) {
      uint32_t value = 0;
      if (!decoder.get(value)) {
        error = "truncated bit-packed definition levels";
        return false;
      }
      levels[static_cast<size_t>(i)] = static_cast<uint8_t>(value);
    }
    consumed = bytes;
    return true;
  }
  error = "unsupported definition level encoding";
  return false;
}

bool decode_levels_v2(const uint8_t *data, size_t size, int64_t count, int bit_width,
                      std::vector<uint8_t> &levels, std::string &error) {
  levels.assign(static_cast<size_t>(count), 0);
  if (bit_width == 0) {
    return true;
  }
  RleDecoder decoder(data, size, bit_width);
  for (int64_t i = 0; i < count; ++i) {
    uint32_t value = 0;
    if (!decoder.get(value)) {
      error = "truncated RLE definition levels";
      return false;
    }
    levels[static_cast<size_t>(i)] = static_cast<uint8_t>(value);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Value decoding
// ---------------------------------------------------------------------------

struct ColumnDecodeInfo {
  int physical_type = -1;
  ParquetLogicalType logical = ParquetLogicalType::UNSUPPORTED_TYPE;
  int timestamp_unit = 0;  // 1 = millis, 2 = micros, 3 = nanos
};

void apply_integer_value(const ColumnDecodeInfo &info, int64_t raw, ParquetValue &value) {
  if (info.logical == ParquetLogicalType::DATETIME_TYPE) {
    int64_t divisor = 1000000;
    if (info.timestamp_unit == 1) {
      divisor = 1000;
    } else if (info.timestamp_unit == 3) {
      divisor = 1000000000;
    }
    value.int_value = floor_div(raw, divisor);
  } else {
    // INT32, INT64 and DATE (days since epoch) are stored as-is.
    value.int_value = raw;
  }
}

bool decode_plain_values(const ColumnDecodeInfo &info, const uint8_t *data, size_t size,
                         int64_t count, std::vector<ParquetValue> &out, std::string &error) {
  out.clear();
  if (count < 0 || count > kMaxPageValues) {
    error = "too many values in a page";
    return false;
  }
  const size_t reserve_count = static_cast<size_t>(std::min<int64_t>(count, 64 * 1024));
  switch (info.physical_type) {
    case PT_BOOLEAN: {
      const size_t bytes = (static_cast<size_t>(count) + 7) / 8;
      if (bytes > size) {
        error = "truncated BOOLEAN values";
        return false;
      }
      out.reserve(reserve_count);
      for (int64_t i = 0; i < count; ++i) {
        ParquetValue value;
        value.int_value = (data[static_cast<size_t>(i) >> 3] >> (i & 7)) & 1;
        out.push_back(std::move(value));
      }
      return true;
    }
    case PT_INT32: {
      if (static_cast<uint64_t>(count) > size / 4) {
        error = "truncated INT32 values";
        return false;
      }
      out.reserve(reserve_count);
      for (int64_t i = 0; i < count; ++i) {
        const uint32_t raw = load_le32(data + static_cast<size_t>(i) * 4);
        int32_t signed_value = 0;
        std::memcpy(&signed_value, &raw, sizeof(signed_value));
        ParquetValue value;
        apply_integer_value(info, signed_value, value);
        out.push_back(std::move(value));
      }
      return true;
    }
    case PT_INT64: {
      if (static_cast<uint64_t>(count) > size / 8) {
        error = "truncated INT64 values";
        return false;
      }
      out.reserve(reserve_count);
      for (int64_t i = 0; i < count; ++i) {
        const uint64_t raw = load_le64(data + static_cast<size_t>(i) * 8);
        int64_t signed_value = 0;
        std::memcpy(&signed_value, &raw, sizeof(signed_value));
        ParquetValue value;
        apply_integer_value(info, signed_value, value);
        out.push_back(std::move(value));
      }
      return true;
    }
    case PT_FLOAT: {
      if (static_cast<uint64_t>(count) > size / 4) {
        error = "truncated FLOAT values";
        return false;
      }
      out.reserve(reserve_count);
      for (int64_t i = 0; i < count; ++i) {
        const uint32_t bits = load_le32(data + static_cast<size_t>(i) * 4);
        float raw = 0.0F;
        std::memcpy(&raw, &bits, sizeof(raw));
        ParquetValue value;
        value.double_value = static_cast<double>(raw);
        out.push_back(std::move(value));
      }
      return true;
    }
    case PT_DOUBLE: {
      if (static_cast<uint64_t>(count) > size / 8) {
        error = "truncated DOUBLE values";
        return false;
      }
      out.reserve(reserve_count);
      for (int64_t i = 0; i < count; ++i) {
        const uint64_t bits = load_le64(data + static_cast<size_t>(i) * 8);
        double raw = 0.0;
        std::memcpy(&raw, &bits, sizeof(raw));
        ParquetValue value;
        value.double_value = raw;
        out.push_back(std::move(value));
      }
      return true;
    }
    case PT_BYTE_ARRAY: {
      size_t pos = 0;
      out.reserve(reserve_count);
      for (int64_t i = 0; i < count; ++i) {
        if (size - pos < 4) {
          error = "truncated BYTE_ARRAY values";
          return false;
        }
        const uint32_t length = load_le32(data + pos);
        pos += 4;
        if (length > size - pos) {
          error = "BYTE_ARRAY value exceeds the page";
          return false;
        }
        ParquetValue value;
        value.string_value.assign(reinterpret_cast<const char *>(data + pos), length);
        pos += length;
        out.push_back(std::move(value));
      }
      return true;
    }
    default:
      error = "unsupported physical type";
      return false;
  }
}

bool decode_encoded_values(const ColumnDecodeInfo &info, const std::vector<ParquetValue> &dictionary,
                           bool has_dictionary, const uint8_t *data, size_t size, int64_t count,
                           int encoding, std::vector<ParquetValue> &out, std::string &error) {
  if (encoding == ENC_PLAIN) {
    return decode_plain_values(info, data, size, count, out, error);
  }
  if (encoding == ENC_PLAIN_DICTIONARY || encoding == ENC_RLE_DICTIONARY) {
    if (!has_dictionary) {
      error = "dictionary encoded page without a dictionary page";
      return false;
    }
    if (size < 1) {
      error = "truncated dictionary index stream";
      return false;
    }
    const int bit_width = data[0];
    if (bit_width > 32) {
      error = "invalid dictionary index bit width";
      return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(std::min<int64_t>(count, 64 * 1024)));
    RleDecoder decoder(data + 1, size - 1, bit_width);
    for (int64_t i = 0; i < count; ++i) {
      uint32_t index = 0;
      if (!decoder.get(index)) {
        error = "truncated dictionary index stream";
        return false;
      }
      if (index >= dictionary.size()) {
        error = "dictionary index is out of range";
        return false;
      }
      out.push_back(dictionary[index]);
    }
    return true;
  }
  error = "unsupported value encoding " + int_to_string(encoding);
  return false;
}

// ---------------------------------------------------------------------------
// Metadata structures and Thrift parsers
// ---------------------------------------------------------------------------

struct RawSchemaElement {
  int32_t type = -1;
  bool has_type = false;
  int32_t type_length = -1;
  int32_t repetition_type = -1;
  bool has_repetition = false;
  std::string name;
  int32_t num_children = 0;
  int32_t converted_type = -1;
  bool has_converted = false;
  int logical_kind = 0;    // 1 STRING, 5 DECIMAL, 6 DATE, 8 TIMESTAMP, 9 INTEGER
  int timestamp_unit = 0;  // 1 MILLIS, 2 MICROS, 3 NANOS
  int integer_bit_width = 0;
};

struct RawColumnMeta {
  int32_t type = -1;
  bool has_type = false;
  std::vector<int32_t> encodings;
  std::vector<std::string> path;
  int32_t codec = CODEC_UNCOMPRESSED;
  bool has_codec = false;
  int64_t num_values = -1;
  int64_t total_uncompressed_size = -1;
  int64_t total_compressed_size = -1;
  int64_t data_page_offset = -1;
  int64_t dictionary_page_offset = -1;
  bool has_dictionary_page_offset = false;
  bool has_meta = false;
};

struct RawRowGroup {
  int64_t num_rows = -1;
  std::vector<RawColumnMeta> columns;
};

struct RawFileMeta {
  int64_t num_rows = -1;
  std::vector<RawSchemaElement> schema;
  std::vector<RawRowGroup> row_groups;
};

bool parse_time_unit(CompactReader &reader, int &unit) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id >= 1 && field_id <= 3 && type == CT_STRUCT) {
      unit = field_id;
      if (!reader.skip(type)) {
        return false;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_timestamp_type(CompactReader &reader, RawSchemaElement &element) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id == 2 && type == CT_STRUCT) {
      if (!parse_time_unit(reader, element.timestamp_unit)) {
        return false;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_integer_type(CompactReader &reader, RawSchemaElement &element) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id == 1 && type == CT_BYTE) {
      uint8_t bit_width = 0;
      if (!reader.byte(bit_width)) {
        return false;
      }
      element.integer_bit_width = bit_width;
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_logical_type(CompactReader &reader, RawSchemaElement &element) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id == 1 && type == CT_STRUCT) {
      element.logical_kind = 1;  // STRING
      if (!reader.skip(type)) {
        return false;
      }
    } else if (field_id == 5 && type == CT_STRUCT) {
      element.logical_kind = 5;  // DECIMAL
      if (!reader.skip(type)) {
        return false;
      }
    } else if (field_id == 6 && type == CT_STRUCT) {
      element.logical_kind = 6;  // DATE
      if (!reader.skip(type)) {
        return false;
      }
    } else if (field_id == 8 && type == CT_STRUCT) {
      element.logical_kind = 8;  // TIMESTAMP
      if (!parse_timestamp_type(reader, element)) {
        return false;
      }
    } else if (field_id == 9 && type == CT_STRUCT) {
      element.logical_kind = 9;  // INTEGER
      if (!parse_integer_type(reader, element)) {
        return false;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_schema_element(CompactReader &reader, RawSchemaElement &element) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    switch (field_id) {
      case 1:
        if (type == CT_I32) {
          if (!reader.i32(element.type)) {
            return false;
          }
          element.has_type = true;
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 2:
        if (type == CT_I32) {
          if (!reader.i32(element.type_length)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 3:
        if (type == CT_I32) {
          if (!reader.i32(element.repetition_type)) {
            return false;
          }
          element.has_repetition = true;
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 4:
        if (type == CT_BINARY) {
          if (!reader.binary(element.name)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 5:
        if (type == CT_I32) {
          if (!reader.i32(element.num_children)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 6:
        if (type == CT_I32) {
          if (!reader.i32(element.converted_type)) {
            return false;
          }
          element.has_converted = true;
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 7:
      case 8:
      case 9: {
        int32_t ignored = 0;
        if (type == CT_I32) {
          if (!reader.i32(ignored)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      }
      case 10:
        if (type == CT_STRUCT) {
          if (!parse_logical_type(reader, element)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type)) {
          return false;
        }
        break;
    }
  }
  return false;
}

bool parse_column_meta(CompactReader &reader, RawColumnMeta &meta) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      meta.has_meta = true;
      return true;
    }
    switch (field_id) {
      case 1:
        if (type == CT_I32) {
          if (!reader.i32(meta.type)) {
            return false;
          }
          meta.has_type = true;
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 2: {
        if (type == CT_LIST) {
          uint8_t element_type = 0;
          uint32_t count = 0;
          if (!reader.list_header(element_type, count) || element_type != CT_I32) {
            return false;
          }
          meta.encodings.clear();
          for (uint32_t i = 0; i < count; ++i) {
            int32_t value = 0;
            if (!reader.i32(value)) {
              return false;
            }
            meta.encodings.push_back(value);
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      }
      case 3: {
        if (type == CT_LIST) {
          uint8_t element_type = 0;
          uint32_t count = 0;
          if (!reader.list_header(element_type, count) || element_type != CT_BINARY || count > 1024) {
            return false;
          }
          meta.path.clear();
          for (uint32_t i = 0; i < count; ++i) {
            std::string part;
            if (!reader.binary(part)) {
              return false;
            }
            meta.path.push_back(std::move(part));
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      }
      case 4:
        if (type == CT_I32) {
          if (!reader.i32(meta.codec)) {
            return false;
          }
          meta.has_codec = true;
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 5:
        if (type == CT_I64) {
          if (!reader.i64(meta.num_values)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 6:
        if (type == CT_I64) {
          if (!reader.i64(meta.total_uncompressed_size)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 7:
        if (type == CT_I64) {
          if (!reader.i64(meta.total_compressed_size)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 9:
        if (type == CT_I64) {
          if (!reader.i64(meta.data_page_offset)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 11:
        if (type == CT_I64) {
          if (!reader.i64(meta.dictionary_page_offset)) {
            return false;
          }
          meta.has_dictionary_page_offset = true;
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type)) {
          return false;
        }
        break;
    }
  }
  return false;
}

bool parse_column_chunk(CompactReader &reader, RawColumnMeta &meta) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return meta.has_meta;
    }
    if (field_id == 3 && type == CT_STRUCT) {
      if (!parse_column_meta(reader, meta)) {
        return false;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_row_group(CompactReader &reader, RawRowGroup &group) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id == 1 && type == CT_LIST) {
      uint8_t element_type = 0;
      uint32_t count = 0;
      if (!reader.list_header(element_type, count) || element_type != CT_STRUCT ||
          count > kMaxRowGroupColumns) {
        return false;
      }
      group.columns.clear();
      group.columns.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        RawColumnMeta meta;
        if (!parse_column_chunk(reader, meta)) {
          return false;
        }
        group.columns.push_back(std::move(meta));
      }
    } else if (field_id == 3 && type == CT_I64) {
      if (!reader.i64(group.num_rows)) {
        return false;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_file_meta(CompactReader &reader, RawFileMeta &meta) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id == 2 && type == CT_LIST) {
      uint8_t element_type = 0;
      uint32_t count = 0;
      if (!reader.list_header(element_type, count) || element_type != CT_STRUCT ||
          count > kMaxSchemaElements) {
        return false;
      }
      meta.schema.clear();
      meta.schema.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        RawSchemaElement element;
        if (!parse_schema_element(reader, element)) {
          return false;
        }
        meta.schema.push_back(std::move(element));
      }
    } else if (field_id == 3 && type == CT_I64) {
      if (!reader.i64(meta.num_rows)) {
        return false;
      }
    } else if (field_id == 4 && type == CT_LIST) {
      uint8_t element_type = 0;
      uint32_t count = 0;
      if (!reader.list_header(element_type, count) || element_type != CT_STRUCT ||
          count > kMaxRowGroups) {
        return false;
      }
      meta.row_groups.clear();
      meta.row_groups.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        RawRowGroup group;
        if (!parse_row_group(reader, group)) {
          return false;
        }
        meta.row_groups.push_back(std::move(group));
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Page headers
// ---------------------------------------------------------------------------

struct PageHeader {
  int32_t type = -1;
  int32_t uncompressed_page_size = -1;
  int32_t compressed_page_size = -1;
  int32_t num_values = 0;
  int32_t encoding = -1;
  int32_t definition_level_encoding = -1;
  int32_t repetition_level_encoding = -1;
  int32_t dictionary_num_values = 0;
  int32_t dictionary_encoding = -1;
  int32_t v2_num_values = 0;
  int32_t v2_num_nulls = 0;
  int32_t v2_num_rows = 0;
  int32_t v2_encoding = -1;
  int32_t v2_definition_levels_byte_length = 0;
  int32_t v2_repetition_levels_byte_length = 0;
  bool v2_is_compressed = true;
};

bool parse_data_page_header(CompactReader &reader, PageHeader &header) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id >= 1 && field_id <= 4 && type == CT_I32) {
      int32_t value = 0;
      if (!reader.i32(value)) {
        return false;
      }
      if (field_id == 1) {
        header.num_values = value;
      } else if (field_id == 2) {
        header.encoding = value;
      } else if (field_id == 3) {
        header.definition_level_encoding = value;
      } else {
        header.repetition_level_encoding = value;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_dictionary_page_header(CompactReader &reader, PageHeader &header) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id == 1 && type == CT_I32) {
      if (!reader.i32(header.dictionary_num_values)) {
        return false;
      }
    } else if (field_id == 2 && type == CT_I32) {
      if (!reader.i32(header.dictionary_encoding)) {
        return false;
      }
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_data_page_header_v2(CompactReader &reader, PageHeader &header) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    if (field_id >= 1 && field_id <= 6 && type == CT_I32) {
      int32_t value = 0;
      if (!reader.i32(value)) {
        return false;
      }
      switch (field_id) {
        case 1:
          header.v2_num_values = value;
          break;
        case 2:
          header.v2_num_nulls = value;
          break;
        case 3:
          header.v2_num_rows = value;
          break;
        case 4:
          header.v2_encoding = value;
          break;
        case 5:
          header.v2_definition_levels_byte_length = value;
          break;
        default:
          header.v2_repetition_levels_byte_length = value;
          break;
      }
    } else if (field_id == 7 && (type == CT_BOOLEAN_TRUE || type == CT_BOOLEAN_FALSE)) {
      header.v2_is_compressed = (type == CT_BOOLEAN_TRUE);
    } else if (!reader.skip(type)) {
      return false;
    }
  }
  return false;
}

bool parse_page_header(CompactReader &reader, PageHeader &header) {
  CompactStructReader fields(reader);
  int16_t field_id = 0;
  uint8_t type = 0;
  bool stop = false;
  while (fields.next(field_id, type, stop)) {
    if (stop) {
      return true;
    }
    switch (field_id) {
      case 1:
        if (type == CT_I32) {
          if (!reader.i32(header.type)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 2:
        if (type == CT_I32) {
          if (!reader.i32(header.uncompressed_page_size)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 3:
        if (type == CT_I32) {
          if (!reader.i32(header.compressed_page_size)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 5:
        if (type == CT_STRUCT) {
          if (!parse_data_page_header(reader, header)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 7:
        if (type == CT_STRUCT) {
          if (!parse_dictionary_page_header(reader, header)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      case 8:
        if (type == CT_STRUCT) {
          if (!parse_data_page_header_v2(reader, header)) {
            return false;
          }
        } else if (!reader.skip(type)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type)) {
          return false;
        }
        break;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

struct LeafInfo {
  std::string name;
  std::vector<std::string> path;
  int physical_type = -1;
  int converted_type = -1;
  int logical_kind = 0;
  int timestamp_unit = 0;
  int repetition_type = REP_REQUIRED;
  int max_definition_level = 0;
  int max_repetition_level = 0;
  bool nested = false;
};

bool build_schema_node(const std::vector<RawSchemaElement> &elements, size_t &index,
                       const std::vector<std::string> &parent_path, int definition_level,
                       int repetition_level, int depth, std::vector<LeafInfo> &leaves,
                       std::string &error) {
  if (depth > kMaxSchemaDepth) {
    error = "Parquet schema is nested too deeply";
    return false;
  }
  if (index >= elements.size()) {
    error = "Parquet schema is truncated";
    return false;
  }
  const RawSchemaElement &element = elements[index++];
  const int repetition = element.has_repetition ? element.repetition_type : REP_REQUIRED;
  if (repetition < REP_REQUIRED || repetition > REP_REPEATED) {
    error = "invalid Parquet repetition type";
    return false;
  }
  const int definition =
      definition_level + ((repetition == REP_OPTIONAL || repetition == REP_REPEATED) ? 1 : 0);
  const int max_repetition =
      repetition_level + (repetition == REP_REPEATED ? 1 : 0);
  std::vector<std::string> path = parent_path;
  path.push_back(element.name);

  if (element.has_type) {
    if (element.num_children > 0) {
      error = "Parquet schema element is both a leaf and a group";
      return false;
    }
    if (element.type < PT_BOOLEAN || element.type > PT_FIXED_LEN_BYTE_ARRAY) {
      error = "unknown Parquet physical type";
      return false;
    }
    LeafInfo leaf;
    leaf.name = element.name;
    leaf.path = path;
    leaf.physical_type = element.type;
    leaf.converted_type = element.has_converted ? element.converted_type : -1;
    leaf.logical_kind = element.logical_kind;
    leaf.timestamp_unit = element.timestamp_unit;
    leaf.repetition_type = repetition;
    leaf.max_definition_level = definition;
    leaf.max_repetition_level = max_repetition;
    leaf.nested = path.size() > 1;
    if (leaf.converted_type == CONV_TIMESTAMP_MILLIS) {
      leaf.timestamp_unit = 1;
    } else if (leaf.converted_type == CONV_TIMESTAMP_MICROS) {
      leaf.timestamp_unit = 2;
    }
    leaves.push_back(std::move(leaf));
    return true;
  }

  if (element.num_children < 0) {
    error = "invalid Parquet schema group";
    return false;
  }
  for (int32_t i = 0; i < element.num_children; ++i) {
    if (!build_schema_node(elements, index, path, definition, max_repetition, depth + 1, leaves,
                           error)) {
      return false;
    }
  }
  return true;
}

bool build_schema(const std::vector<RawSchemaElement> &elements, std::vector<LeafInfo> &leaves,
                  std::string &error) {
  if (elements.empty()) {
    error = "Parquet schema is empty";
    return false;
  }
  if (elements[0].has_type) {
    error = "Parquet schema root must be a group";
    return false;
  }
  if (elements[0].num_children <= 0) {
    error = "Parquet schema has no columns";
    return false;
  }
  size_t index = 1;
  for (int32_t i = 0; i < elements[0].num_children; ++i) {
    if (!build_schema_node(elements, index, std::vector<std::string>(), 0, 0, 1, leaves, error)) {
      return false;
    }
  }
  if (index != elements.size()) {
    error = "Parquet schema element count does not match the schema tree";
    return false;
  }
  return true;
}

ParquetLogicalType classify_leaf(const LeafInfo &leaf, std::string &reason) {
  const bool decimal = leaf.converted_type == CONV_DECIMAL || leaf.logical_kind == 5;
  const bool date = leaf.converted_type == CONV_DATE || leaf.logical_kind == 6;
  const bool timestamp = leaf.converted_type == CONV_TIMESTAMP_MILLIS ||
                         leaf.converted_type == CONV_TIMESTAMP_MICROS || leaf.logical_kind == 8;
  if (leaf.nested) {
    reason = "nested column groups are not supported";
    return ParquetLogicalType::UNSUPPORTED_TYPE;
  }
  if (leaf.max_repetition_level > 0) {
    reason = "repeated fields are not supported";
    return ParquetLogicalType::UNSUPPORTED_TYPE;
  }
  switch (leaf.physical_type) {
    case PT_BOOLEAN:
      return ParquetLogicalType::BOOL_TYPE;
    case PT_INT32:
      if (decimal) {
        reason = "DECIMAL is not supported";
        return ParquetLogicalType::UNSUPPORTED_TYPE;
      }
      return date ? ParquetLogicalType::DATE_TYPE : ParquetLogicalType::INT32_TYPE;
    case PT_INT64:
      if (decimal) {
        reason = "DECIMAL is not supported";
        return ParquetLogicalType::UNSUPPORTED_TYPE;
      }
      if (timestamp) {
        if (leaf.logical_kind == 8 && leaf.timestamp_unit == 3) {
          reason = "TIMESTAMP with nanosecond precision is not supported";
          return ParquetLogicalType::UNSUPPORTED_TYPE;
        }
        return ParquetLogicalType::DATETIME_TYPE;
      }
      return ParquetLogicalType::INT64_TYPE;
    case PT_FLOAT:
      return ParquetLogicalType::FLOAT_TYPE;
    case PT_DOUBLE:
      return ParquetLogicalType::DOUBLE_TYPE;
    case PT_BYTE_ARRAY:
      if (decimal) {
        reason = "DECIMAL is not supported";
        return ParquetLogicalType::UNSUPPORTED_TYPE;
      }
      return ParquetLogicalType::STRING_TYPE;
    case PT_INT96:
      reason = "INT96 is not supported";
      return ParquetLogicalType::UNSUPPORTED_TYPE;
    case PT_FIXED_LEN_BYTE_ARRAY:
      reason = "FIXED_LEN_BYTE_ARRAY is not supported";
      return ParquetLogicalType::UNSUPPORTED_TYPE;
    default:
      reason = "unknown physical type";
      return ParquetLogicalType::UNSUPPORTED_TYPE;
  }
}

// ---------------------------------------------------------------------------
// Reader implementation
// ---------------------------------------------------------------------------

struct ColumnDesc {
  ParquetColumn info;
  std::string reason;
};

struct ColState {
  std::string name;
  ColumnDecodeInfo decode;
  int codec = CODEC_UNCOMPRESSED;
  int max_definition_level = 0;
  std::vector<uint8_t> chunk;
  size_t page_pos = 0;
  int64_t values_remaining = 0;
  bool finished = false;
  bool has_dictionary = false;
  std::vector<ParquetValue> dictionary;
  std::vector<ParquetValue> buffer;
  int64_t buffer_start = 0;
};

}  // namespace

struct ParquetReader::Impl {
  FileReader file;
  std::string error;
  bool opened = false;
  std::vector<ColumnDesc> column_descs;
  std::vector<ParquetColumn> columns;
  std::vector<LeafInfo> leaves;
  std::vector<RawRowGroup> row_groups;
  std::vector<std::vector<int>> chunk_of_leaf;
  std::vector<int> user_projection;
  bool has_user_projection = false;
  std::vector<int> projection;
  std::string unsupported_message;
  int64_t total_rows = 0;

  size_t row_group_index = 0;
  int64_t row_in_group = 0;
  int64_t rows_in_group = 0;
  bool need_group_init = true;
  std::vector<ColState> states;

  ~Impl() { file.close(); }

  int open(const std::string &path);
  int set_projection(const std::vector<int> &column_indexes);
  int next_row(std::vector<ParquetValue> &row);
  int rewind();

  void reset() {
    opened = false;
    file.close();
    error.clear();
    column_descs.clear();
    columns.clear();
    leaves.clear();
    row_groups.clear();
    chunk_of_leaf.clear();
    projection.clear();
    unsupported_message.clear();
    total_rows = 0;
    row_group_index = 0;
    row_in_group = 0;
    rows_in_group = 0;
    need_group_init = true;
    states.clear();
  }

  bool load_file();
  bool build_columns(std::string &err);
  bool apply_projection(std::string &err);
  int match_leaf(const std::vector<std::string> &path) const;
  bool init_row_group(std::string &err);
  bool ensure_row(ColState &state, int64_t row, std::string &err);

  bool decompress_page(const ColState &state, const PageHeader &header, const uint8_t *source,
                       size_t source_size, std::vector<uint8_t> &holder, const uint8_t *&output,
                       size_t &output_size, std::string &err) const;
  bool decode_dictionary_page(ColState &state, const PageHeader &header, const uint8_t *body,
                              size_t body_size, std::string &err);
  bool decode_data_page(ColState &state, const PageHeader &header, const uint8_t *body,
                        size_t body_size, bool version2, std::string &err);
};

bool ParquetReader::Impl::load_file() {
  const int64_t file_size = file.size();
  if (file_size < 12) {
    error = "file is too small to be a Parquet file";
    return false;
  }
  uint8_t magic[4];
  if (!file.read_at(0, magic, sizeof(magic), error)) {
    return false;
  }
  if (std::memcmp(magic, "PAR1", 4) != 0) {
    error = "missing PAR1 magic at the beginning of the file";
    return false;
  }
  uint8_t trailer[8];
  if (!file.read_at(file_size - 8, trailer, sizeof(trailer), error)) {
    return false;
  }
  if (std::memcmp(trailer + 4, "PAR1", 4) != 0) {
    error = "missing PAR1 magic at the end of the file";
    return false;
  }
  const uint32_t footer_length = load_le32(trailer);
  if (footer_length == 0 || static_cast<int64_t>(footer_length) > file_size - 12) {
    error = "invalid Parquet footer length";
    return false;
  }
  const int64_t footer_offset = file_size - 8 - static_cast<int64_t>(footer_length);
  std::vector<uint8_t> footer(footer_length);
  if (!file.read_at(footer_offset, footer.data(), footer.size(), error)) {
    return false;
  }
  CompactReader reader(footer.data(), footer.size());
  RawFileMeta meta;
  if (!parse_file_meta(reader, meta) || !reader.ok()) {
    error = "malformed Parquet FileMetaData";
    return false;
  }
  if (meta.num_rows < 0) {
    error = "Parquet FileMetaData is missing num_rows";
    return false;
  }
  if (!build_schema(meta.schema, leaves, error)) {
    return false;
  }
  if (leaves.empty()) {
    error = "Parquet schema has no leaf columns";
    return false;
  }
  if (!build_columns(error)) {
    return false;
  }
  row_groups = std::move(meta.row_groups);
  if (row_groups.empty() && meta.num_rows > 0) {
    error = "Parquet file declares rows but contains no row groups";
    return false;
  }
  chunk_of_leaf.resize(row_groups.size());
  int64_t sum_rows = 0;
  for (size_t r = 0; r < row_groups.size(); ++r) {
    RawRowGroup &group = row_groups[r];
    if (group.num_rows < 0) {
      error = "Parquet row group is missing num_rows";
      return false;
    }
    if (group.num_rows > std::numeric_limits<int64_t>::max() - sum_rows) {
      error = "Parquet row count overflow";
      return false;
    }
    sum_rows += group.num_rows;
    chunk_of_leaf[r].assign(leaves.size(), -1);
    for (size_t c = 0; c < group.columns.size(); ++c) {
      RawColumnMeta &chunk = group.columns[c];
      if (!chunk.has_meta) {
        error = "Parquet column chunk is missing meta_data";
        return false;
      }
      int leaf = match_leaf(chunk.path);
      if (leaf < 0 && chunk.path.empty() && c < leaves.size() && chunk_of_leaf[r][c] < 0) {
        leaf = static_cast<int>(c);
      }
      if (leaf < 0) {
        error = "Parquet column chunk path does not match the schema";
        return false;
      }
      if (chunk_of_leaf[r][leaf] >= 0) {
        error = "duplicate Parquet column chunk for one schema leaf";
        return false;
      }
      if (chunk.has_type && chunk.type != leaves[static_cast<size_t>(leaf)].physical_type) {
        error = "Parquet column chunk physical type does not match the schema";
        return false;
      }
      chunk_of_leaf[r][static_cast<size_t>(leaf)] = static_cast<int>(c);
    }
    for (size_t leaf = 0; leaf < leaves.size(); ++leaf) {
      if (chunk_of_leaf[r][leaf] < 0) {
        error = "Parquet row group is missing a column chunk";
        return false;
      }
    }
  }
  total_rows = sum_rows;
  if (!apply_projection(error)) {
    return false;
  }
  return true;
}

bool ParquetReader::Impl::build_columns(std::string &err) {
  columns.clear();
  column_descs.clear();
  columns.reserve(leaves.size());
  column_descs.reserve(leaves.size());
  for (const LeafInfo &leaf : leaves) {
    ColumnDesc desc;
    desc.info.name = leaf.nested ? join_path(leaf.path) : leaf.name;
    desc.info.type = classify_leaf(leaf, desc.reason);
    desc.info.nullable = leaf.max_definition_level > 0;
    columns.push_back(desc.info);
    column_descs.push_back(std::move(desc));
  }
  (void)err;
  return true;
}

int ParquetReader::Impl::match_leaf(const std::vector<std::string> &path) const {
  if (path.empty()) {
    return -1;
  }
  for (size_t i = 0; i < leaves.size(); ++i) {
    if (leaves[i].path == path) {
      return static_cast<int>(i);
    }
  }
  if (path.size() == 1) {
    for (size_t i = 0; i < leaves.size(); ++i) {
      if (leaves[i].name == path[0]) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

bool ParquetReader::Impl::apply_projection(std::string &err) {
  projection.clear();
  if (has_user_projection) {
    projection = user_projection;
  } else {
    projection.resize(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
      projection[i] = static_cast<int>(i);
    }
  }
  for (int index : projection) {
    if (index < 0 || static_cast<size_t>(index) >= columns.size()) {
      err = "projected column index is out of range";
      return false;
    }
  }
  unsupported_message.clear();
  for (int index : projection) {
    const ParquetColumn &column = columns[static_cast<size_t>(index)];
    if (column.type == ParquetLogicalType::UNSUPPORTED_TYPE) {
      unsupported_message = "column '" + column.name + "' is not supported";
      const std::string &reason = column_descs[static_cast<size_t>(index)].reason;
      if (!reason.empty()) {
        unsupported_message += ": " + reason;
      }
      break;
    }
  }
  row_group_index = 0;
  row_in_group = 0;
  rows_in_group = 0;
  need_group_init = true;
  states.clear();
  return true;
}

int ParquetReader::Impl::open(const std::string &path) {
  reset();
  if (!file.open(path, error)) {
    return -1;
  }
  if (!load_file()) {
    file.close();
    return -1;
  }
  opened = true;
  return 0;
}

int ParquetReader::Impl::set_projection(const std::vector<int> &column_indexes) {
  error.clear();
  bool all_columns = column_indexes.empty();
  for (int index : column_indexes) {
    if (index == -1) {
      all_columns = true;
      break;
    }
  }
  if (all_columns) {
    user_projection.clear();
    has_user_projection = false;
  } else {
    for (int index : column_indexes) {
      if (index < 0) {
        error = "invalid projected column index";
        return -1;
      }
      if (opened && static_cast<size_t>(index) >= columns.size()) {
        error = "projected column index is out of range";
        return -1;
      }
    }
    user_projection = column_indexes;
    has_user_projection = true;
  }
  if (opened && !apply_projection(error)) {
    return -1;
  }
  return 0;
}

bool ParquetReader::Impl::init_row_group(std::string &err) {
  states.clear();
  const RawRowGroup &group = row_groups[row_group_index];
  rows_in_group = group.num_rows;
  if (rows_in_group < 0) {
    err = "Parquet row group has an invalid row count";
    return false;
  }
  states.resize(projection.size());
  if (rows_in_group == 0) {
    return true;
  }
  for (size_t i = 0; i < projection.size(); ++i) {
    const int leaf_index = projection[i];
    const LeafInfo &leaf = leaves[static_cast<size_t>(leaf_index)];
    const int chunk_index = chunk_of_leaf[row_group_index][static_cast<size_t>(leaf_index)];
    if (chunk_index < 0) {
      err = "missing column chunk for row group";
      return false;
    }
    const RawColumnMeta &chunk = group.columns[static_cast<size_t>(chunk_index)];
    if (!chunk.has_meta) {
      err = "column chunk is missing metadata";
      return false;
    }
    if (chunk.codec != CODEC_UNCOMPRESSED && chunk.codec != CODEC_SNAPPY) {
      err = "column '" + columns[static_cast<size_t>(leaf_index)].name +
            "' uses an unsupported compression codec";
      return false;
    }
    if (chunk.num_values < rows_in_group) {
      err = "column '" + columns[static_cast<size_t>(leaf_index)].name +
            "' has fewer values than row group rows";
      return false;
    }
    if (chunk.total_compressed_size <= 0) {
      err = "column '" + columns[static_cast<size_t>(leaf_index)].name +
            "' has an invalid compressed size";
      return false;
    }
    if (chunk.data_page_offset < 0) {
      err = "column '" + columns[static_cast<size_t>(leaf_index)].name +
            "' has no data page offset";
      return false;
    }
    int64_t start = chunk.data_page_offset;
    if (chunk.has_dictionary_page_offset && chunk.dictionary_page_offset >= 0 &&
        chunk.dictionary_page_offset < start) {
      start = chunk.dictionary_page_offset;
    }
    if (start > file.size() ||
        chunk.total_compressed_size > file.size() - start) {
      err = "column '" + columns[static_cast<size_t>(leaf_index)].name +
            "' points outside of the file";
      return false;
    }

    ColState &state = states[i];
    state.name = columns[static_cast<size_t>(leaf_index)].name;
    state.decode.physical_type = leaf.physical_type;
    state.decode.logical = columns[static_cast<size_t>(leaf_index)].type;
    state.decode.timestamp_unit = leaf.timestamp_unit;
    state.codec = chunk.codec;
    state.max_definition_level = leaf.max_definition_level;
    state.chunk.resize(static_cast<size_t>(chunk.total_compressed_size));
    if (!file.read_at(start, state.chunk.data(), state.chunk.size(), err)) {
      return false;
    }
    state.page_pos = 0;
    state.values_remaining = chunk.num_values;
    state.finished = false;
    state.has_dictionary = false;
    state.dictionary.clear();
    state.buffer.clear();
    state.buffer_start = 0;
  }
  return true;
}

bool ParquetReader::Impl::decompress_page(const ColState &state, const PageHeader &header,
                                          const uint8_t *source, size_t source_size,
                                          std::vector<uint8_t> &holder, const uint8_t *&output,
                                          size_t &output_size, std::string &err) const {
  if (header.uncompressed_page_size < 0 ||
      static_cast<int64_t>(header.uncompressed_page_size) > kMaxPageUncompressedBytes) {
    err = "invalid page size";
    return false;
  }
  if (static_cast<size_t>(header.compressed_page_size) != source_size) {
    err = "page body size mismatch";
    return false;
  }
  if (state.codec == CODEC_UNCOMPRESSED) {
    output = source;
    output_size = source_size;
    return true;
  }
  if (state.codec == CODEC_SNAPPY) {
    if (!snappy_decompress(source, source_size, static_cast<size_t>(header.uncompressed_page_size),
                           holder, err)) {
      return false;
    }
    output = holder.data();
    output_size = holder.size();
    return true;
  }
  err = "unsupported compression codec";
  return false;
}

bool ParquetReader::Impl::decode_dictionary_page(ColState &state, const PageHeader &header,
                                                 const uint8_t *body, size_t body_size,
                                                 std::string &err) {
  std::vector<uint8_t> holder;
  const uint8_t *page = nullptr;
  size_t page_size = 0;
  if (!decompress_page(state, header, body, body_size, holder, page, page_size, err)) {
    return false;
  }
  const int64_t count = header.dictionary_num_values;
  if (count < 0 || count > kMaxDictionaryValues) {
    err = "invalid dictionary size";
    return false;
  }
  if (header.dictionary_encoding != ENC_PLAIN &&
      header.dictionary_encoding != ENC_PLAIN_DICTIONARY && header.dictionary_encoding != -1) {
    err = "unsupported dictionary page encoding";
    return false;
  }
  std::vector<ParquetValue> dictionary;
  if (!decode_plain_values(state.decode, page, page_size, count, dictionary, err)) {
    return false;
  }
  state.dictionary = std::move(dictionary);
  state.has_dictionary = true;
  return true;
}

bool ParquetReader::Impl::decode_data_page(ColState &state, const PageHeader &header,
                                           const uint8_t *body, size_t body_size, bool version2,
                                           std::string &err) {
  const int64_t count = version2 ? header.v2_num_values : header.num_values;
  if (count < 0 || count > kMaxPageValues) {
    err = "invalid data page value count";
    return false;
  }
  std::vector<uint8_t> holder;
  const uint8_t *page = nullptr;
  size_t page_size = 0;
  std::vector<uint8_t> levels;
  const uint8_t *values = nullptr;
  size_t values_size = 0;

  if (!version2) {
    if (!decompress_page(state, header, body, body_size, holder, page, page_size, err)) {
      return false;
    }
    if (state.max_definition_level > 0) {
      size_t consumed = 0;
      if (!decode_levels_v1(page, page_size, count,
                            level_bit_width(state.max_definition_level),
                            header.definition_level_encoding, levels, consumed, err)) {
        return false;
      }
      if (consumed > page_size) {
        err = "definition levels exceed the page";
        return false;
      }
      values = page + consumed;
      values_size = page_size - consumed;
    } else {
      values = page;
      values_size = page_size;
    }
  } else {
    const int32_t repetition_length = header.v2_repetition_levels_byte_length;
    const int32_t definition_length = header.v2_definition_levels_byte_length;
    if (repetition_length < 0 || definition_length < 0 ||
        header.uncompressed_page_size < 0 ||
        static_cast<int64_t>(header.uncompressed_page_size) > kMaxPageUncompressedBytes) {
      err = "invalid data page v2 header";
      return false;
    }
    const size_t level_lengths = static_cast<size_t>(repetition_length) +
                                 static_cast<size_t>(definition_length);
    if (level_lengths > body_size) {
      err = "data page v2 levels exceed the page";
      return false;
    }
    if (state.max_definition_level > 0) {
      if (!decode_levels_v2(body + repetition_length, static_cast<size_t>(definition_length),
                            count, level_bit_width(state.max_definition_level), levels, err)) {
        return false;
      }
    } else {
      levels.assign(static_cast<size_t>(count), 0);
    }
    if (static_cast<int64_t>(level_lengths) > header.uncompressed_page_size) {
      err = "data page v2 uncompressed size is too small";
      return false;
    }
    const size_t expected_values_size =
        static_cast<size_t>(header.uncompressed_page_size) - level_lengths;
    const uint8_t *value_source = body + level_lengths;
    const size_t value_source_size = body_size - level_lengths;
    if (state.codec == CODEC_UNCOMPRESSED || !header.v2_is_compressed) {
      if (value_source_size < expected_values_size) {
        err = "data page v2 values are truncated";
        return false;
      }
      values = value_source;
      values_size = value_source_size;
    } else if (state.codec == CODEC_SNAPPY) {
      std::vector<uint8_t> values_holder;
      if (!snappy_decompress(value_source, value_source_size, expected_values_size, values_holder,
                             err)) {
        return false;
      }
      holder = std::move(values_holder);
      values = holder.data();
      values_size = holder.size();
    } else {
      err = "unsupported compression codec";
      return false;
    }
  }

  int64_t non_null_count = 0;
  for (int64_t i = 0; i < count; ++i) {
    if (state.max_definition_level == 0 ||
        levels[static_cast<size_t>(i)] == state.max_definition_level) {
      ++non_null_count;
    }
  }
  std::vector<ParquetValue> decoded;
  const int encoding = version2 ? header.v2_encoding : header.encoding;
  if (encoding < 0) {
    err = "data page is missing its value encoding";
    return false;
  }
  if (non_null_count > 0 &&
      !decode_encoded_values(state.decode, state.dictionary, state.has_dictionary, values,
                             values_size, non_null_count, encoding, decoded, err)) {
    return false;
  }
  state.buffer.reserve(state.buffer.size() + static_cast<size_t>(count));
  size_t decoded_index = 0;
  for (int64_t i = 0; i < count; ++i) {
    if (state.max_definition_level > 0 &&
        levels[static_cast<size_t>(i)] < state.max_definition_level) {
      ParquetValue null_value;
      null_value.is_null = true;
      state.buffer.push_back(std::move(null_value));
    } else {
      if (decoded_index >= decoded.size()) {
        err = "data page has fewer values than its definition levels";
        return false;
      }
      state.buffer.push_back(std::move(decoded[decoded_index++]));
    }
  }
  if (decoded_index != decoded.size()) {
    err = "data page has more values than its definition levels";
    return false;
  }
  return true;
}

bool ParquetReader::Impl::ensure_row(ColState &state, int64_t row, std::string &err) {
  if (row < state.buffer_start) {
    err = "internal row buffer underflow";
    return false;
  }
  if (row < state.buffer_start + static_cast<int64_t>(state.buffer.size())) {
    return true;
  }
  if (!state.buffer.empty()) {
    state.buffer_start += static_cast<int64_t>(state.buffer.size());
    state.buffer.clear();
    if (row < state.buffer_start) {
      err = "internal row buffer underflow";
      return false;
    }
  }
  while (state.buffer_start + static_cast<int64_t>(state.buffer.size()) <= row) {
    if (state.finished) {
      err = "column '" + state.name + "' ended before all rows were read";
      return false;
    }
    if (state.page_pos >= state.chunk.size()) {
      err = "column '" + state.name + "' has no more pages but rows remain";
      return false;
    }
    CompactReader reader(state.chunk.data() + state.page_pos, state.chunk.size() - state.page_pos);
    PageHeader header;
    if (!parse_page_header(reader, header) || !reader.ok()) {
      err = "column '" + state.name + "': malformed page header";
      return false;
    }
    const size_t header_size = reader.pos();
    if (header_size > state.chunk.size() - state.page_pos) {
      err = "column '" + state.name + "': page header exceeds the chunk";
      return false;
    }
    state.page_pos += header_size;
    if (header.compressed_page_size < 0 ||
        static_cast<size_t>(header.compressed_page_size) > state.chunk.size() - state.page_pos) {
      err = "column '" + state.name + "': truncated page body";
      return false;
    }
    const uint8_t *body = state.chunk.data() + state.page_pos;
    const size_t body_size = static_cast<size_t>(header.compressed_page_size);
    state.page_pos += body_size;

    if (header.type == PAGE_DATA || header.type == PAGE_DATA_V2) {
      const int64_t page_values =
          header.type == PAGE_DATA ? header.num_values : header.v2_num_values;
      if (page_values < 0 || page_values > state.values_remaining) {
        err = "column '" + state.name + "': page has more values than the chunk metadata";
        return false;
      }
      if (!decode_data_page(state, header, body, body_size, header.type == PAGE_DATA_V2, err)) {
        return false;
      }
      state.values_remaining -= page_values;
      if (state.values_remaining <= 0) {
        state.finished = true;
      }
    } else if (header.type == PAGE_DICTIONARY) {
      if (!decode_dictionary_page(state, header, body, body_size, err)) {
        return false;
      }
    } else if (header.type == PAGE_INDEX) {
      // Index pages carry no values; skip them.
    } else {
      err = "column '" + state.name + "': unknown page type";
      return false;
    }
  }
  return true;
}

int ParquetReader::Impl::next_row(std::vector<ParquetValue> &row) {
  error.clear();
  if (!opened) {
    error = "reader is not open";
    return -1;
  }
  if (!unsupported_message.empty()) {
    error = unsupported_message;
    return -1;
  }
  for (;;) {
    if (row_group_index >= row_groups.size()) {
      return 1;
    }
    if (need_group_init) {
      if (!init_row_group(error)) {
        return -1;
      }
      need_group_init = false;
      row_in_group = 0;
    }
    if (row_in_group >= rows_in_group) {
      ++row_group_index;
      need_group_init = true;
      states.clear();
      continue;
    }
    row.clear();
    row.reserve(projection.size());
    for (size_t i = 0; i < projection.size(); ++i) {
      ColState &state = states[i];
      if (!ensure_row(state, row_in_group, error)) {
        return -1;
      }
      const int64_t offset = row_in_group - state.buffer_start;
      if (offset < 0 || static_cast<size_t>(offset) >= state.buffer.size()) {
        error = "internal column buffer mismatch";
        return -1;
      }
      row.push_back(state.buffer[static_cast<size_t>(offset)]);
    }
    ++row_in_group;
    return 0;
  }
}

int ParquetReader::Impl::rewind() {
  error.clear();
  if (!opened) {
    error = "reader is not open";
    return -1;
  }
  row_group_index = 0;
  row_in_group = 0;
  rows_in_group = 0;
  need_group_init = true;
  states.clear();
  return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace {

// The public API must never leak exceptions.  Allocation failures are reported
// through the regular negative status plus error().
int exception_status(std::string &error, const char *what) noexcept {
  try {
    error = "Parquet reader exception: ";
    error += (what != nullptr ? what : "unknown");
  } catch (...) {
    // Nothing more we can do if even the error string cannot be allocated.
  }
  return -1;
}

}  // namespace

ParquetReader::ParquetReader() : impl_(new Impl()) {}

ParquetReader::~ParquetReader() = default;

int ParquetReader::open(const std::string &path) {
  try {
    return impl_->open(path);
  } catch (const std::exception &e) {
    return exception_status(impl_->error, e.what());
  } catch (...) {
    return exception_status(impl_->error, "unknown exception");
  }
}

int ParquetReader::set_projected_columns(const std::vector<int> &column_indexes) {
  try {
    return impl_->set_projection(column_indexes);
  } catch (const std::exception &e) {
    return exception_status(impl_->error, e.what());
  } catch (...) {
    return exception_status(impl_->error, "unknown exception");
  }
}

const std::vector<ParquetColumn> &ParquetReader::columns() const { return impl_->columns; }

int64_t ParquetReader::num_rows() const { return impl_->opened ? impl_->total_rows : 0; }

int ParquetReader::next_row(std::vector<ParquetValue> &row) {
  try {
    return impl_->next_row(row);
  } catch (const std::exception &e) {
    return exception_status(impl_->error, e.what());
  } catch (...) {
    return exception_status(impl_->error, "unknown exception");
  }
}

int ParquetReader::rewind() {
  try {
    return impl_->rewind();
  } catch (const std::exception &e) {
    return exception_status(impl_->error, e.what());
  } catch (...) {
    return exception_status(impl_->error, "unknown exception");
  }
}

const std::string &ParquetReader::error() const { return impl_->error; }

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
