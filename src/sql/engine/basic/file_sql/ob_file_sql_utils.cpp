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

#include "sql/engine/basic/file_sql/ob_file_sql_utils.h"
#include "sql/engine/basic/file_sql/parquet_reader.h"
#include "common/timezone/ob_time_convert.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <glob.h>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

enum FileValueKind
{
  FK_NULL = 0,
  FK_BOOL,
  FK_INT,
  FK_DOUBLE,
  FK_DATE,
  FK_DATETIME,
  FK_STR,
};

const char *KIND_TYPE_NAMES[] = { "NULL", "BOOLEAN", "BIGINT", "DOUBLE", "DATE", "DATETIME", "VARCHAR" };

void set_error(char *errbuf, const int64_t errbuf_len, const char *fmt, ...)
{
  if (NULL != errbuf && errbuf_len > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, static_cast<size_t>(errbuf_len), fmt, ap);
    va_end(ap);
    errbuf[errbuf_len - 1] = '\0';
  }
}

int make_std_string(const common::ObString &str, std::string &out)
{
  out.assign(str.ptr() == NULL ? "" : str.ptr(), static_cast<size_t>(str.length()));
  return common::OB_SUCCESS;
}

bool is_space(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

// ------------- scalar text parsing -------------

bool parse_int64_text(const char *s, const int64_t len, int64_t &value)
{
  bool ok = len > 0;
  int64_t idx = 0;
  bool negative = false;
  if (ok && ('+' == s[0] || '-' == s[0])) {
    negative = '-' == s[0];
    idx = 1;
  }
  if (ok && idx >= len) {
    ok = false;
  }
  uint64_t acc = 0;
  const uint64_t limit = negative ? static_cast<uint64_t>(INT64_MAX) + 1ULL : static_cast<uint64_t>(INT64_MAX);
  for (; ok && idx < len; ++idx) {
    if (s[idx] < '0' || s[idx] > '9') {
      ok = false;
    } else {
      const uint64_t digit = static_cast<uint64_t>(s[idx] - '0');
      if (acc > (limit - digit) / 10) {
        ok = false;
      } else {
        acc = acc * 10 + digit;
      }
    }
  }
  if (ok) {
    value = negative ? (acc == static_cast<uint64_t>(INT64_MAX) + 1ULL ? INT64_MIN : -static_cast<int64_t>(acc))
                     : static_cast<int64_t>(acc);
  }
  return ok;
}

bool parse_double_text(const char *s, const int64_t len, double &value)
{
  bool ok = len > 0 && len < 512;
  if (ok) {
    char buf[512];
    MEMCPY(buf, s, len);
    buf[len] = '\0';
    char *endptr = NULL;
    errno = 0;
    value = strtod(buf, &endptr);
    ok = endptr == buf + len && errno != ERANGE;
  }
  return ok;
}

bool parse_bool_text(const char *s, const int64_t len, bool &value, const bool allow_numeric = false)
{
  bool ok = true;
  if (4 == len && 0 == strncasecmp(s, "true", 4)) {
    value = true;
  } else if (5 == len && 0 == strncasecmp(s, "false", 5)) {
    value = false;
  } else if (allow_numeric && 1 == len && ('1' == s[0] || '0' == s[0])) {
    value = '1' == s[0];
  } else {
    ok = false;
  }
  return ok;
}

bool all_digits(const char *s, const int64_t start, const int64_t count)
{
  bool ok = true;
  for (int64_t i = 0; ok && i < count; ++i) {
    ok = s[start + i] >= '0' && s[start + i] <= '9';
  }
  return ok;
}

// parse "YYYY-MM-DD" into y/m/d, strict format check
bool split_date_text(const char *s, const int64_t len, int32_t &y, int32_t &m, int32_t &d)
{
  bool ok = 10 == len && '-' == s[4] && '-' == s[7] && all_digits(s, 0, 4)
            && all_digits(s, 5, 2) && all_digits(s, 8, 2);
  if (ok) {
    y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    m = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    ok = y >= 1 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
  }
  return ok;
}

bool split_datetime_text(const char *s,
                         const int64_t len,
                         int32_t &y, int32_t &m, int32_t &d,
                         int32_t &hh, int32_t &mm, int32_t &ss)
{
  bool ok = len >= 19 && (s[10] == ' ' || s[10] == 'T') && ':' == s[13] && ':' == s[16]
            && all_digits(s, 11, 2) && all_digits(s, 14, 2) && all_digits(s, 17, 2);
  if (ok) {
    ok = split_date_text(s, 10, y, m, d);
  }
  if (ok) {
    hh = (s[11] - '0') * 10 + (s[12] - '0');
    mm = (s[14] - '0') * 10 + (s[15] - '0');
    ss = (s[17] - '0') * 10 + (s[18] - '0');
    ok = hh <= 23 && mm <= 59 && ss <= 59;
  }
  if (ok && len > 19) {
    int64_t idx = 19;
    if ('.' == s[idx]) {
      ++idx;
      const int64_t frac_begin = idx;
      while (idx < len && s[idx] >= '0' && s[idx] <= '9') {
        ++idx;
      }
      ok = idx > frac_begin;
    }
    // allow trailing 'Z' or timezone offset, ignored
    if (ok && idx < len && ('Z' == s[idx] || 'z' == s[idx])) {
      ++idx;
    } else if (ok && idx < len && ('+' == s[idx] || '-' == s[idx])) {
      ++idx;
      if (idx + 1 < len && all_digits(s, idx, 2)) {
        idx += 2;
        if (idx < len && ':' == s[idx]) {
          ++idx;
        }
        if (idx + 1 < len && all_digits(s, idx, 2)) {
          idx += 2;
        }
      }
    }
    ok = idx == len;
  }
  return ok;
}

bool date_text_to_value(const char *s, const int64_t len, int32_t &days)
{
  int32_t y = 0, m = 0, d = 0;
  bool ok = split_date_text(s, len, y, m, d);
  if (ok) {
    const int64_t int_value = static_cast<int64_t>(y) * 10000 + m * 100 + d;
    common::ObDateSqlMode date_sql_mode;
    ok = common::OB_SUCCESS == common::ObTimeConverter::int_to_date(int_value, days, date_sql_mode);
  }
  return ok;
}

bool datetime_text_to_value(const char *s, const int64_t len, int64_t &seconds)
{
  int32_t y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
  bool ok = split_datetime_text(s, len, y, m, d, hh, mm, ss);
  if (ok) {
    const int64_t int_value = static_cast<int64_t>(y) * 10000000000LL + static_cast<int64_t>(m) * 100000000LL
                              + static_cast<int64_t>(d) * 1000000LL + static_cast<int64_t>(hh) * 10000
                              + static_cast<int64_t>(mm) * 100 + ss;
    common::ObTimeConvertCtx cvrt_ctx(NULL, false);
    common::ObDateSqlMode date_sql_mode;
    ok = common::OB_SUCCESS == common::ObTimeConverter::int_to_datetime(int_value, 0, cvrt_ctx, seconds, date_sql_mode);
  }
  return ok;
}

FileValueKind classify_text(const char *s, const int64_t len)
{
  FileValueKind kind = FK_STR;
  bool bool_value = false;
  int64_t int_value = 0;
  double double_value = 0.0;
  int32_t days = 0;
  int64_t seconds = 0;
  if (parse_bool_text(s, len, bool_value)) {
    kind = FK_BOOL;
  } else if (parse_int64_text(s, len, int_value)) {
    kind = FK_INT;
  } else if (parse_double_text(s, len, double_value)) {
    kind = FK_DOUBLE;
  } else if (date_text_to_value(s, len, days)) {
    kind = FK_DATE;
  } else if (datetime_text_to_value(s, len, seconds)) {
    kind = FK_DATETIME;
  } else {
    kind = FK_STR;
  }
  return kind;
}

FileValueKind merge_kind(FileValueKind a, FileValueKind b)
{
  FileValueKind result = FK_STR;
  if (FK_NULL == a) {
    result = b;
  } else if (FK_NULL == b) {
    result = a;
  } else if (a == b) {
    result = a;
  } else if ((FK_INT == a && FK_DOUBLE == b) || (FK_DOUBLE == a && FK_INT == b)) {
    result = FK_DOUBLE;
  } else if ((FK_DATE == a && FK_DATETIME == b) || (FK_DATETIME == a && FK_DATE == b)) {
    result = FK_DATETIME;
  } else {
    result = FK_STR;
  }
  return result;
}

common::ObObjType kind_to_obj_type(FileValueKind kind)
{
  common::ObObjType type = common::ObNullType;
  switch (kind) {
    case FK_BOOL: type = common::ObTinyIntType; break;
    case FK_INT: type = common::ObIntType; break;
    case FK_DOUBLE: type = common::ObDoubleType; break;
    case FK_DATE: type = common::ObDateType; break;
    case FK_DATETIME: type = common::ObDateTimeType; break;
    case FK_STR: type = common::ObVarcharType; break;
    default: type = common::ObNullType; break;
  }
  return type;
}

// ------------- CSV record reader -------------

class CsvRecordReader
{
public:
  CsvRecordReader()
    : fp_(NULL), buf_pos_(0), buf_len_(0), eof_(false), io_error_(false), record_no_(0),
      delimiter_(','), quote_('"'), escape_('"'), has_escape_(true), buf_(1 << 16)
  {}
  ~CsvRecordReader() { close(); }
  void close()
  {
    if (NULL != fp_) {
      fclose(fp_);
      fp_ = NULL;
    }
  }
  int open(const std::string &path,
           const char delimiter,
           const char quote,
           const char escape,
           const bool has_escape,
           char *errbuf,
           const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    close();
    path_ = path;
    delimiter_ = delimiter;
    quote_ = quote;
    escape_ = escape;
    has_escape_ = has_escape;
    if (NULL == (fp_ = fopen(path.c_str(), "rb"))) {
      ret = common::OB_FILE_NOT_EXIST;
      set_error(errbuf, errbuf_len, "cannot open file '%.*s': %s",
                static_cast<int>(path.size()), path.data(), strerror(errno));
    } else {
      buf_pos_ = 0;
      buf_len_ = 0;
      eof_ = false;
      io_error_ = false;
      record_no_ = 0;
      // strip UTF-8 BOM
      unsigned char bom[3];
      const size_t n = fread(bom, 1, 3, fp_);
      if (!(3 == n && 0xEF == bom[0] && 0xBB == bom[1] && 0xBF == bom[2])) {
        rewind(fp_);
      }
    }
    return ret;
  }
  int rewind_reader(char *errbuf, const int64_t errbuf_len)
  {
    return open(path_, delimiter_, quote_, escape_, has_escape_, errbuf, errbuf_len);
  }
  int64_t record_no() const { return record_no_; }

  // return 1: a record is produced, 0: end of file, negative: error
  int next_record(std::vector<std::string> &fields,
                  std::vector<bool> &quoted,
                  char *errbuf,
                  const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    bool produced = false;
    while (OB_SUCC(ret) && !produced) {
      fields.clear();
      quoted.clear();
      int c = get_char();
      if (c < 0) {
        if (io_error_) {
          ret = common::OB_IO_ERROR;
          set_error(errbuf, errbuf_len, "read error on file '%.*s'",
                    static_cast<int>(path_.size()), path_.data());
        } else {
          ret = common::OB_ITER_END;
        }
        break;
      }
      bool record_done = false;
      while (OB_SUCC(ret) && !record_done) {
        std::string field;
        bool was_quoted = false;
        if (c == static_cast<unsigned char>(quote_)) {
          was_quoted = true;
          c = get_char();
          bool quoted_done = false;
          while (OB_SUCC(ret) && !quoted_done) {
            if (c < 0) {
              ret = common::OB_INVALID_ARGUMENT;
              set_error(errbuf, errbuf_len,
                        "unterminated quoted field near record %ld of '%.*s'",
                        static_cast<long>(record_no_ + 1), static_cast<int>(path_.size()), path_.data());
            } else if (has_escape_ && escape_ != quote_ && c == static_cast<unsigned char>(escape_)) {
              const int next = get_char();
              if (next < 0) {
                ret = common::OB_INVALID_ARGUMENT;
                set_error(errbuf, errbuf_len, "dangling escape character near record %ld of '%.*s'",
                          static_cast<long>(record_no_ + 1), static_cast<int>(path_.size()), path_.data());
              } else {
                field.push_back(static_cast<char>(next));
                c = get_char();
              }
            } else if (c == static_cast<unsigned char>(quote_)) {
              const int next = get_char();
              if (next == static_cast<unsigned char>(quote_)) {
                field.push_back(quote_);
                c = get_char();
              } else {
                c = next;
                quoted_done = true;
              }
            } else {
              field.push_back(static_cast<char>(c));
              c = get_char();
            }
          }
          if (OB_SUCC(ret) && c >= 0 && c != delimiter_ && c != '\n' && c != '\r') {
            ret = common::OB_INVALID_ARGUMENT;
            set_error(errbuf, errbuf_len,
                      "unexpected character '%c' after quoted field near record %ld of '%.*s'",
                      static_cast<char>(c), static_cast<long>(record_no_ + 1),
                      static_cast<int>(path_.size()), path_.data());
          }
        } else {
          while (c >= 0 && c != delimiter_ && c != '\n' && c != '\r') {
            field.push_back(static_cast<char>(c));
            c = get_char();
          }
        }
        if (OB_SUCC(ret)) {
          fields.push_back(field);
          quoted.push_back(was_quoted);
          if (c == delimiter_) {
            c = get_char();
            if (c < 0) {
              // trailing delimiter: an empty last field
              fields.push_back(std::string());
              quoted.push_back(false);
              record_done = true;
            }
          } else if (c == '\r') {
            const int next = peek_char();
            if ('\n' == next) {
              get_char();
            }
            record_done = true;
          } else if (c == '\n' || c < 0) {
            record_done = true;
          } else {
            ret = common::OB_INVALID_ARGUMENT;
            set_error(errbuf, errbuf_len, "unexpected CSV format near record %ld of '%.*s'",
                      static_cast<long>(record_no_ + 1), static_cast<int>(path_.size()), path_.data());
          }
        }
      }
      if (OB_SUCC(ret)) {
        ++record_no_;
        // skip empty lines
        if (fields.size() == 1 && fields[0].empty() && !quoted[0]) {
          // keep scanning
        } else {
          produced = true;
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = produced ? common::OB_SUCCESS : common::OB_ITER_END;
    }
    return ret;
  }

private:
  int fill_buf()
  {
    int ret = common::OB_SUCCESS;
    if (eof_) {
      ret = common::OB_ITER_END;
    } else {
      buf_len_ = fread(&buf_[0], 1, buf_.size(), fp_);
      buf_pos_ = 0;
      if (0 == buf_len_) {
        if (0 != ferror(fp_)) {
          io_error_ = true;
          ret = common::OB_IO_ERROR;
        } else {
          eof_ = true;
          ret = common::OB_ITER_END;
        }
      }
    }
    return ret;
  }
  int get_char()
  {
    int c = -1;
    if (buf_pos_ >= buf_len_ && common::OB_SUCCESS != fill_buf()) {
      c = -1;
    } else {
      c = static_cast<unsigned char>(buf_[buf_pos_++]);
    }
    return c;
  }
  int peek_char()
  {
    int c = -1;
    if (buf_pos_ >= buf_len_ && common::OB_SUCCESS != fill_buf()) {
      c = -1;
    } else {
      c = static_cast<unsigned char>(buf_[buf_pos_]);
    }
    return c;
  }

  FILE *fp_;
  std::string path_;
  size_t buf_pos_;
  size_t buf_len_;
  bool eof_;
  bool io_error_;
  int64_t record_no_;
  char delimiter_;
  char quote_;
  char escape_;
  bool has_escape_;
  std::vector<char> buf_;
};

// ------------- JSON text helpers -------------

struct JsonValue
{
  JsonValue() : kind_(FK_NULL), int_value_(0), double_value_(0.0), bool_value_(false) {}
  FileValueKind kind_;
  int64_t int_value_;
  double double_value_;
  bool bool_value_;
  std::string str_value_;
};

struct JsonField
{
  std::string key_;
  JsonValue value_;
};

class JsonTextParser
{
public:
  JsonTextParser(const char *data, const size_t len) : p_(data), end_(data + len), err_() {}
  bool skip_ws()
  {
    while (p_ < end_ && is_space(*p_)) {
      ++p_;
    }
    return true;
  }
  bool at_end()
  {
    skip_ws();
    return p_ >= end_;
  }
  bool expect(const char c)
  {
    skip_ws();
    if (p_ < end_ && *p_ == c) {
      ++p_;
      return true;
    }
    err_ = "unexpected character";
    return false;
  }
  bool parse_string(std::string &out)
  {
    bool ok = true;
    skip_ws();
    if (p_ >= end_ || '"' != *p_) {
      err_ = "expected a string";
      ok = false;
    } else {
      ++p_;
      out.clear();
      while (ok && p_ < end_) {
        const unsigned char c = static_cast<unsigned char>(*p_++);
        if ('"' == c) {
          break;
        } else if ('\\' == c) {
          if (p_ >= end_) {
            err_ = "unterminated escape";
            ok = false;
          } else {
            const char e = *p_++;
            switch (e) {
              case '"': out.push_back('"'); break;
              case '\\': out.push_back('\\'); break;
              case '/': out.push_back('/'); break;
              case 'b': out.push_back('\b'); break;
              case 'f': out.push_back('\f'); break;
              case 'n': out.push_back('\n'); break;
              case 'r': out.push_back('\r'); break;
              case 't': out.push_back('\t'); break;
              case 'u': {
                uint32_t code = 0;
                if (!parse_hex4(code)) {
                  err_ = "invalid unicode escape";
                  ok = false;
                } else if (code >= 0xD800 && code <= 0xDBFF && p_ + 5 < end_
                           && '\\' == p_[0] && 'u' == p_[1]) {
                  uint32_t low = 0;
                  p_ += 2;
                  if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                    err_ = "invalid unicode surrogate";
                    ok = false;
                  } else {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    append_utf8(code, out);
                  }
                } else {
                  append_utf8(code, out);
                }
                break;
              }
              default:
                err_ = "invalid escape character";
                ok = false;
                break;
            }
          }
        } else if (c < 0x20) {
          err_ = "control character in string";
          ok = false;
        } else {
          out.push_back(static_cast<char>(c));
        }
      }
      if (ok && (p_ > end_ || '"' != *(p_ - 1))) {
        err_ = "unterminated string";
        ok = false;
      }
    }
    return ok;
  }
  bool parse_number(JsonValue &value)
  {
    bool ok = true;
    skip_ws();
    const char *begin = p_;
    while (p_ < end_ && (('-' == *p_) || ('+' == *p_) || ('.' == *p_) || ('e' == *p_)
                         || ('E' == *p_) || (*p_ >= '0' && *p_ <= '9'))) {
      ++p_;
    }
    const int64_t len = p_ - begin;
    if (len <= 0) {
      err_ = "expected a number";
      ok = false;
    } else {
      bool has_dot = false;
      for (int64_t i = 0; i < len; ++i) {
        if ('.' == begin[i] || 'e' == begin[i] || 'E' == begin[i]) {
          has_dot = true;
        }
      }
      if (!has_dot && parse_int64_text(begin, len, value.int_value_)) {
        value.kind_ = FK_INT;
      } else if (parse_double_text(begin, len, value.double_value_)) {
        value.kind_ = FK_DOUBLE;
      } else {
        err_ = "invalid number";
        ok = false;
      }
    }
    return ok;
  }
  bool parse_value(JsonValue &value)
  {
    bool ok = true;
    skip_ws();
    if (p_ >= end_) {
      err_ = "unexpected end of json";
      ok = false;
    } else if ('"' == *p_) {
      value.kind_ = FK_STR;
      ok = parse_string(value.str_value_);
    } else if ('{' == *p_ || '[' == *p_) {
      err_ = "nested object or array is not supported";
      ok = false;
    } else if ('t' == *p_ && p_ + 4 <= end_ && 0 == strncmp(p_, "true", 4)) {
      p_ += 4;
      value.kind_ = FK_BOOL;
      value.bool_value_ = true;
    } else if ('f' == *p_ && p_ + 5 <= end_ && 0 == strncmp(p_, "false", 5)) {
      p_ += 5;
      value.kind_ = FK_BOOL;
      value.bool_value_ = false;
    } else if ('n' == *p_ && p_ + 4 <= end_ && 0 == strncmp(p_, "null", 4)) {
      p_ += 4;
      value.kind_ = FK_NULL;
    } else {
      ok = parse_number(value);
    }
    return ok;
  }
  bool parse_object(std::vector<JsonField> &fields)
  {
    bool ok = expect('{');
    fields.clear();
    if (ok) {
      skip_ws();
      if (p_ < end_ && '}' == *p_) {
        ++p_;
      } else {
        while (ok) {
          JsonField field;
          ok = parse_string(field.key_);
          if (ok) {
            ok = expect(':');
          }
          if (ok) {
            ok = parse_value(field.value_);
          }
          if (ok) {
            fields.push_back(field);
            skip_ws();
            if (p_ < end_ && ',' == *p_) {
              ++p_;
            } else if (p_ < end_ && '}' == *p_) {
              ++p_;
              break;
            } else {
              err_ = "expected ',' or '}'";
              ok = false;
            }
          }
        }
      }
    }
    return ok;
  }
  const char *position() const { return p_; }
  const std::string &error() const { return err_; }

private:
  bool parse_hex4(uint32_t &code)
  {
    bool ok = p_ + 4 <= end_;
    code = 0;
    for (int i = 0; ok && i < 4; ++i) {
      const char c = p_[i];
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        ok = false;
      }
      if (ok) {
        code = (code << 4) | digit;
      }
    }
    if (ok) {
      p_ += 4;
    }
    return ok;
  }
  void append_utf8(const uint32_t code, std::string &out)
  {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  const char *p_;
  const char *end_;
  std::string err_;
};

int read_entire_file(const std::string &path, std::string &content, char *errbuf,
                     const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  FILE *fp = fopen(path.c_str(), "rb");
  if (NULL == fp) {
    ret = common::OB_FILE_NOT_EXIST;
    set_error(errbuf, errbuf_len, "cannot open file '%.*s': %s",
              static_cast<int>(path.size()), path.data(), strerror(errno));
  } else {
    char buf[65536];
    size_t n = 0;
    content.clear();
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
      content.append(buf, n);
    }
    if (0 != ferror(fp)) {
      ret = common::OB_IO_ERROR;
      set_error(errbuf, errbuf_len, "read error on file '%.*s'",
                static_cast<int>(path.size()), path.data());
    }
    fclose(fp);
  }
  return ret;
}

class TextLineReader
{
public:
  TextLineReader() : fp_(NULL), buf_pos_(0), buf_len_(0), eof_(false), io_error_(false), buf_(1 << 16) {}
  ~TextLineReader() { close(); }
  void close()
  {
    if (NULL != fp_) {
      fclose(fp_);
      fp_ = NULL;
    }
  }
  int open(const std::string &path, char *errbuf, const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    close();
    path_ = path;
    if (NULL == (fp_ = fopen(path.c_str(), "rb"))) {
      ret = common::OB_FILE_NOT_EXIST;
      set_error(errbuf, errbuf_len, "cannot open file '%.*s': %s",
                static_cast<int>(path.size()), path.data(), strerror(errno));
    } else {
      buf_pos_ = 0;
      buf_len_ = 0;
      eof_ = false;
      io_error_ = false;
      unsigned char bom[3];
      const size_t n = fread(bom, 1, 3, fp_);
      if (!(3 == n && 0xEF == bom[0] && 0xBB == bom[1] && 0xBF == bom[2])) {
        rewind(fp_);
      }
    }
    return ret;
  }
  int rewind_reader(char *errbuf, const int64_t errbuf_len)
  {
    return open(path_, errbuf, errbuf_len);
  }
  // 1: line read, 0: eof, negative: error
  int next_line(std::string &line, char *errbuf, const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    line.clear();
    bool got_any = false;
    int c = get_char();
    if (c < 0) {
      if (io_error_) {
        ret = common::OB_IO_ERROR;
        set_error(errbuf, errbuf_len, "read error on file '%.*s'",
                  static_cast<int>(path_.size()), path_.data());
      } else {
        ret = common::OB_ITER_END;
      }
    } else {
      got_any = true;
      while (c >= 0 && c != '\n') {
        if (c != '\r') {
          line.push_back(static_cast<char>(c));
        }
        c = get_char();
      }
    }
    if (common::OB_SUCCESS == ret && !got_any) {
      ret = common::OB_ITER_END;
    }
    return ret;
  }

private:
  int fill_buf()
  {
    int ret = common::OB_SUCCESS;
    if (eof_) {
      ret = common::OB_ITER_END;
    } else {
      buf_len_ = fread(&buf_[0], 1, buf_.size(), fp_);
      buf_pos_ = 0;
      if (0 == buf_len_) {
        if (0 != ferror(fp_)) {
          io_error_ = true;
          ret = common::OB_IO_ERROR;
        } else {
          eof_ = true;
          ret = common::OB_ITER_END;
        }
      }
    }
    return ret;
  }
  int get_char()
  {
    int c = -1;
    if (buf_pos_ >= buf_len_ && common::OB_SUCCESS != fill_buf()) {
      c = -1;
    } else {
      c = static_cast<unsigned char>(buf_[buf_pos_++]);
    }
    return c;
  }

  FILE *fp_;
  std::string path_;
  size_t buf_pos_;
  size_t buf_len_;
  bool eof_;
  bool io_error_;
  std::vector<char> buf_;
};

// ------------- shared readers -------------

int convert_text_to_value(const ObFileSqlColumnMeta &column,
                          const char *text,
                          const int64_t len,
                          const int64_t row_no,
                          ObFileSqlValue &value,
                          const char *file_name,
                          char *errbuf,
                          const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  value.is_null_ = false;
  switch (column.obj_type_) {
    case common::ObNullType:
      value.is_null_ = true;
      break;
    case common::ObVarcharType:
      value.string_value_.assign_ptr(const_cast<char *>(text), static_cast<int32_t>(len));
      break;
    case common::ObIntType: {
      int64_t int_value = 0;
      if (!parse_int64_text(text, len, int_value)) {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "invalid integer '%.*s' at row %ld of '%.*s'",
                  static_cast<int>(len), text, static_cast<long>(row_no),
                  static_cast<int>(strlen(file_name)), file_name);
      } else {
        value.int_value_ = int_value;
      }
      break;
    }
    case common::ObDoubleType: {
      double double_value = 0.0;
      if (!parse_double_text(text, len, double_value)) {
        int64_t int_value = 0;
        if (parse_int64_text(text, len, int_value)) {
          double_value = static_cast<double>(int_value);
        } else {
          ret = common::OB_INVALID_ARGUMENT;
          set_error(errbuf, errbuf_len, "invalid double '%.*s' at row %ld of '%.*s'",
                    static_cast<int>(len), text, static_cast<long>(row_no),
                    static_cast<int>(strlen(file_name)), file_name);
        }
      }
      if (common::OB_SUCCESS == ret) {
        value.double_value_ = double_value;
      }
      break;
    }
    case common::ObTinyIntType: {
      bool bool_value = false;
      if (!parse_bool_text(text, len, bool_value, true)) {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "invalid boolean '%.*s' at row %ld of '%.*s'",
                  static_cast<int>(len), text, static_cast<long>(row_no),
                  static_cast<int>(strlen(file_name)), file_name);
      } else {
        value.int_value_ = bool_value ? 1 : 0;
      }
      break;
    }
    case common::ObDateType: {
      int32_t days = 0;
      if (!date_text_to_value(text, len, days)) {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "invalid date '%.*s' at row %ld of '%.*s'",
                  static_cast<int>(len), text, static_cast<long>(row_no),
                  static_cast<int>(strlen(file_name)), file_name);
      } else {
        value.int_value_ = days;
      }
      break;
    }
    case common::ObDateTimeType: {
      int64_t seconds = 0;
      if (!datetime_text_to_value(text, len, seconds)) {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "invalid datetime '%.*s' at row %ld of '%.*s'",
                  static_cast<int>(len), text, static_cast<long>(row_no),
                  static_cast<int>(strlen(file_name)), file_name);
      } else {
        value.int_value_ = seconds;
      }
      break;
    }
    default:
      ret = common::OB_NOT_SUPPORTED;
      set_error(errbuf, errbuf_len, "unsupported file column type %d", static_cast<int>(column.obj_type_));
      break;
  }
  return ret;
}

FileValueKind obj_type_to_kind(const common::ObObjType type)
{
  FileValueKind kind = FK_STR;
  switch (type) {
    case common::ObNullType: kind = FK_NULL; break;
    case common::ObTinyIntType: kind = FK_BOOL; break;
    case common::ObIntType: kind = FK_INT; break;
    case common::ObDoubleType: kind = FK_DOUBLE; break;
    case common::ObDateType: kind = FK_DATE; break;
    case common::ObDateTimeType: kind = FK_DATETIME; break;
    default: kind = FK_STR; break;
  }
  return kind;
}

class CsvFileReader : public ObFileSqlReader
{
public:
  CsvFileReader() : parser_(), columns_(NULL), projection_(), skip_header_(true), started_(false) {}
  virtual ~CsvFileReader() {}
  int init(const std::string &path,
           const ObFileSqlOptions &options,
           const common::ObIArray<ObFileSqlColumnMeta> &columns,
           const common::ObIArray<int64_t> &projection,
           char *errbuf,
           const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    path_ = path;
    options_ = options;
    columns_ = &columns;
    projection_.clear();
    for (int64_t i = 0; i < projection.count(); ++i) {
      projection_.push_back(projection.at(i));
    }
    row_.prepare_allocate(projection.count());
    skip_header_ = options.header_;
    started_ = false;
    if (OB_FAIL(parser_.open(path, options.delimiter_, options.quote_, options.escape_,
                             options.has_escape_, errbuf, errbuf_len))) {
    }
    return ret;
  }
  virtual int next_row()
  {
    int ret = common::OB_SUCCESS;
    char errbuf[256];
    errbuf[0] = '\0';
    if (!started_) {
      started_ = true;
      if (skip_header_) {
        if (OB_FAIL(parser_.next_record(fields_, quoted_, errbuf, sizeof(errbuf)))) {
          if (common::OB_ITER_END == ret) {
            // header only file: no rows
          }
        }
      }
    }
    if (common::OB_SUCCESS == ret) {
      ret = parser_.next_record(fields_, quoted_, errbuf, sizeof(errbuf));
    }
    if (common::OB_SUCCESS == ret) {
      bool is_null = false;
      for (int64_t i = 0; common::OB_SUCCESS == ret && i < static_cast<int64_t>(projection_.size()); ++i) {
        const int64_t column_idx = projection_[i];
        if (column_idx < 0 || column_idx >= static_cast<int64_t>(fields_.size())) {
          ret = common::OB_INVALID_ARGUMENT;
          set_error(errbuf, sizeof(errbuf), "row %ld has %ld fields, expected at least %ld",
                    static_cast<long>(parser_.record_no()),
                    static_cast<long>(fields_.size()), static_cast<long>(column_idx + 1));
        } else {
          const std::string &field = fields_[column_idx];
          const bool was_quoted = quoted_[column_idx];
          is_null = !was_quoted
                    && ((options_.nullstr_.empty() && field.empty())
                        || (!options_.nullstr_.empty()
                            && field.size() == static_cast<size_t>(options_.nullstr_.length())
                            && 0 == memcmp(field.data(), options_.nullstr_.ptr(), field.size())));
          if (is_null) {
            row_.at(i).is_null_ = true;
          } else if (OB_FAIL(convert_text_to_value(columns_->at(column_idx), field.data(),
                                                   static_cast<int64_t>(field.size()),
                                                   parser_.record_no(), row_.at(i),
                                                   path_.c_str(), errbuf, sizeof(errbuf)))) {
          }
        }
      }
    }
    if (common::OB_SUCCESS != ret && common::OB_ITER_END != ret) {
      LOG_WARN("failed to read csv file", K(ret), K(path_.c_str()), K(errbuf));
    }
    return ret;
  }
  virtual int rewind()
  {
    char errbuf[256];
    started_ = false;
    return parser_.rewind_reader(errbuf, sizeof(errbuf));
  }

private:
  CsvRecordReader parser_;
  std::string path_;
  ObFileSqlOptions options_;
  const common::ObIArray<ObFileSqlColumnMeta> *columns_;
  std::vector<int64_t> projection_;
  std::vector<std::string> fields_;
  std::vector<bool> quoted_;
  bool skip_header_;
  bool started_;
};

int convert_json_value(const ObFileSqlColumnMeta &column,
                       const JsonValue &json_value,
                       const int64_t row_no,
                       const std::string &path,
                       std::string &scratch,
                       ObFileSqlValue &value,
                       char *errbuf,
                       const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  value.is_null_ = json_value.kind_ == FK_NULL;
  if (common::OB_SUCCESS == ret && !value.is_null_) {
    if (common::ObVarcharType == column.obj_type_) {
      if (FK_STR == json_value.kind_) {
        value.string_value_.assign_ptr(const_cast<char *>(json_value.str_value_.data()),
                                       static_cast<int32_t>(json_value.str_value_.size()));
      } else if (FK_BOOL == json_value.kind_) {
        scratch = json_value.bool_value_ ? "true" : "false";
        value.string_value_.assign_ptr(const_cast<char *>(scratch.data()),
                                       static_cast<int32_t>(scratch.size()));
      } else if (FK_INT == json_value.kind_) {
        scratch = std::to_string(json_value.int_value_);
        value.string_value_.assign_ptr(const_cast<char *>(scratch.data()),
                                       static_cast<int32_t>(scratch.size()));
      } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", json_value.double_value_);
        scratch.assign(buf);
        value.string_value_.assign_ptr(const_cast<char *>(scratch.data()),
                                       static_cast<int32_t>(scratch.size()));
      }
    } else if (FK_STR == json_value.kind_) {
      ret = convert_text_to_value(column, json_value.str_value_.data(),
                                  static_cast<int64_t>(json_value.str_value_.size()),
                                  row_no, value, path.c_str(), errbuf, errbuf_len);
    } else if (FK_INT == json_value.kind_) {
      if (common::ObIntType == column.obj_type_ || common::ObTinyIntType == column.obj_type_) {
        value.int_value_ = json_value.int_value_;
      } else if (common::ObDoubleType == column.obj_type_) {
        value.double_value_ = static_cast<double>(json_value.int_value_);
      } else {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "type mismatch at row %ld of '%.*s'", static_cast<long>(row_no),
                  static_cast<int>(path.size()), path.data());
      }
    } else if (FK_DOUBLE == json_value.kind_) {
      if (common::ObDoubleType == column.obj_type_) {
        value.double_value_ = json_value.double_value_;
      } else {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "type mismatch at row %ld of '%.*s'", static_cast<long>(row_no),
                  static_cast<int>(path.size()), path.data());
      }
    } else if (FK_BOOL == json_value.kind_) {
      if (common::ObTinyIntType == column.obj_type_) {
        value.int_value_ = json_value.bool_value_ ? 1 : 0;
      } else {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "type mismatch at row %ld of '%.*s'", static_cast<long>(row_no),
                  static_cast<int>(path.size()), path.data());
      }
    } else {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "type mismatch at row %ld of '%.*s'", static_cast<long>(row_no),
                static_cast<int>(path.size()), path.data());
    }
  }
  return ret;
}

int parse_json_array_file(const std::string &content,
                          std::vector<std::vector<JsonField> > &docs,
                          std::string &err)
{
  int ret = common::OB_SUCCESS;
  JsonTextParser parser(content.data(), content.size());
  docs.clear();
  if (!parser.expect('[')) {
    err = parser.error();
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    bool first = true;
    parser.skip_ws();
    if (parser.at_end()) {
      err = "unexpected end of json array";
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      const char *cur = parser.position();
      if (cur < content.data() + content.size() && ']' == *cur) {
        parser.expect(']');
      } else {
        while (common::OB_SUCCESS == ret) {
          if (!first && !parser.expect(',')) {
            err = parser.error();
            ret = common::OB_INVALID_ARGUMENT;
          } else {
            std::vector<JsonField> doc;
            if (!parser.parse_object(doc)) {
              err = parser.error();
              ret = common::OB_INVALID_ARGUMENT;
            } else {
              docs.push_back(doc);
              first = false;
              parser.skip_ws();
              if (parser.at_end()) {
                err = "unterminated json array";
                ret = common::OB_INVALID_ARGUMENT;
              } else {
                const char *p = parser.position();
                if (']' == *p) {
                  parser.expect(']');
                  parser.skip_ws();
                  if (!parser.at_end()) {
                    err = "trailing characters after json array";
                    ret = common::OB_INVALID_ARGUMENT;
                  }
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

class JsonFileReader : public ObFileSqlReader
{
public:
  JsonFileReader()
    : columns_(NULL), projection_(), array_mode_(false), array_idx_(0), row_no_(0), scratch_()
  {}
  virtual ~JsonFileReader() {}
  int init(const std::string &path,
           const ObFileSqlOptions &options,
           const bool array_mode,
           const common::ObIArray<ObFileSqlColumnMeta> &columns,
           const common::ObIArray<int64_t> &projection,
           char *errbuf,
           const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    path_ = path;
    options_ = options;
    columns_ = &columns;
    array_mode_ = array_mode;
    projection_.clear();
    for (int64_t i = 0; i < projection.count(); ++i) {
      projection_.push_back(projection.at(i));
    }
    row_.prepare_allocate(projection.count());
    array_idx_ = 0;
    row_no_ = 0;
    if (array_mode_) {
      std::string content;
      std::string err;
      if (OB_FAIL(read_entire_file(path, content, errbuf, errbuf_len))) {
      } else if (OB_FAIL(parse_json_array_file(content, array_docs_, err))) {
        set_error(errbuf, errbuf_len, "invalid json array in '%.*s': %s",
                  static_cast<int>(path.size()), path.data(), err.c_str());
      }
    } else if (OB_FAIL(line_reader_.open(path, errbuf, errbuf_len))) {
    }
    return ret;
  }
  virtual int next_row()
  {
    int ret = common::OB_SUCCESS;
    char errbuf[256];
    errbuf[0] = '\0';
    if (array_mode_) {
      if (array_idx_ >= static_cast<int64_t>(array_docs_.size())) {
        ret = common::OB_ITER_END;
      } else {
        ret = fill_row(array_docs_[array_idx_], ++row_no_, errbuf, sizeof(errbuf));
        ++array_idx_;
      }
    } else {
      bool got = false;
      while (common::OB_SUCCESS == ret && !got) {
        ret = line_reader_.next_line(line_, errbuf, sizeof(errbuf));
        if (common::OB_SUCCESS == ret) {
          bool only_space = true;
          for (size_t i = 0; only_space && i < line_.size(); ++i) {
            only_space = is_space(line_[i]);
          }
          if (only_space) {
            // skip empty lines
          } else {
            got = true;
            ++row_no_;
            JsonTextParser parser(line_.data(), line_.size());
            std::vector<JsonField> doc;
            if (!parser.parse_object(doc)) {
              ret = common::OB_INVALID_ARGUMENT;
              set_error(errbuf, sizeof(errbuf), "invalid json at row %ld of '%.*s': %s",
                        static_cast<long>(row_no_), static_cast<int>(path_.size()), path_.data(),
                        parser.error().c_str());
            } else {
              parser.skip_ws();
              if (!parser.at_end()) {
                ret = common::OB_INVALID_ARGUMENT;
                set_error(errbuf, sizeof(errbuf), "trailing characters at row %ld of '%.*s'",
                          static_cast<long>(row_no_), static_cast<int>(path_.size()), path_.data());
              } else {
                ret = fill_row(doc, row_no_, errbuf, sizeof(errbuf));
              }
            }
          }
        }
      }
    }
    if (common::OB_SUCCESS != ret && common::OB_ITER_END != ret && errbuf[0] != '\0') {
      LOG_WARN("failed to read json file", K(ret), K(path_.c_str()), K(errbuf));
    }
    return ret;
  }
  virtual int rewind()
  {
    char errbuf[256];
    row_no_ = 0;
    if (array_mode_) {
      array_idx_ = 0;
      return common::OB_SUCCESS;
    } else {
      return line_reader_.rewind_reader(errbuf, sizeof(errbuf));
    }
  }

private:
  int fill_row(const std::vector<JsonField> &doc,
               const int64_t row_no,
               char *errbuf,
               const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    for (int64_t i = 0; common::OB_SUCCESS == ret && i < static_cast<int64_t>(projection_.size()); ++i) {
      const int64_t column_idx = projection_[i];
      const JsonValue *found = NULL;
      if (column_idx >= 0 && column_idx < columns_->count()) {
        const common::ObString &name = columns_->at(column_idx).name_;
        for (size_t j = 0; NULL == found && j < doc.size(); ++j) {
          if (doc[j].key_.size() == static_cast<size_t>(name.length())
              && 0 == memcmp(doc[j].key_.data(), name.ptr(), name.length())) {
            found = &doc[j].value_;
          }
        }
      }
      if (NULL == found) {
        row_.at(i).is_null_ = true;
      } else {
        ret = convert_json_value(columns_->at(column_idx), *found, row_no, path_, scratch_,
                                 row_.at(i), errbuf, errbuf_len);
      }
    }
    return ret;
  }

  std::string path_;
  ObFileSqlOptions options_;
  const common::ObIArray<ObFileSqlColumnMeta> *columns_;
  std::vector<int64_t> projection_;
  bool array_mode_;
  TextLineReader line_reader_;
  std::string line_;
  std::vector<std::vector<JsonField> > array_docs_;
  int64_t array_idx_;
  int64_t row_no_;
  std::string scratch_;
};

class ParquetFileReader : public ObFileSqlReader
{
public:
  ParquetFileReader() : columns_(NULL), projection_(), pq_values_() {}
  virtual ~ParquetFileReader() {}
  int init(const std::string &path,
           const common::ObIArray<ObFileSqlColumnMeta> &columns,
           const common::ObIArray<int64_t> &projection,
           char *errbuf,
           const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    path_ = path;
    columns_ = &columns;
    projection_.clear();
    std::vector<int> pq_projection;
    for (int64_t i = 0; i < projection.count(); ++i) {
      projection_.push_back(projection.at(i));
      pq_projection.push_back(static_cast<int>(projection.at(i)));
    }
    row_.prepare_allocate(projection.count());
    if (0 != parquet_.open(path)) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "cannot read parquet file '%.*s': %s",
                static_cast<int>(path.size()), path.data(), parquet_.error().c_str());
    } else if (!pq_projection.empty() && 0 != parquet_.set_projected_columns(pq_projection)) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "cannot project columns of parquet file '%.*s': %s",
                static_cast<int>(path.size()), path.data(), parquet_.error().c_str());
    }
    return ret;
  }
  virtual int next_row()
  {
    int ret = common::OB_SUCCESS;
    char errbuf[256];
    errbuf[0] = '\0';
    const int rc = parquet_.next_row(pq_values_);
    if (1 == rc) {
      ret = common::OB_ITER_END;
    } else if (rc < 0) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, sizeof(errbuf), "failed to read parquet file '%.*s': %s",
                static_cast<int>(path_.size()), path_.data(), parquet_.error().c_str());
      LOG_WARN("failed to read parquet row", K(ret), K(errbuf));
    } else {
      for (int64_t i = 0; common::OB_SUCCESS == ret && i < static_cast<int64_t>(projection_.size()); ++i) {
        if (i >= static_cast<int64_t>(pq_values_.size())) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          const ParquetValue &src = pq_values_[i];
          ObFileSqlValue &dst = row_.at(i);
          if (src.is_null) {
            dst.is_null_ = true;
          } else {
            const common::ObObjType type = columns_->at(projection_[i]).obj_type_;
            dst.is_null_ = false;
            switch (type) {
              case common::ObVarcharType:
                dst.string_value_.assign_ptr(const_cast<char *>(src.string_value.data()),
                                             static_cast<int32_t>(src.string_value.size()));
                break;
              case common::ObDoubleType:
                dst.double_value_ = src.double_value;
                break;
              case common::ObIntType:
              case common::ObTinyIntType:
              case common::ObDateType:
              case common::ObDateTimeType:
                dst.int_value_ = src.int_value;
                break;
              default:
                ret = common::OB_NOT_SUPPORTED;
                break;
            }
          }
        }
      }
    }
    return ret;
  }
  virtual int rewind()
  {
    return 0 == parquet_.rewind() ? common::OB_SUCCESS : common::OB_INVALID_ARGUMENT;
  }

private:
  std::string path_;
  const common::ObIArray<ObFileSqlColumnMeta> *columns_;
  std::vector<int64_t> projection_;
  ParquetReader parquet_;
  std::vector<ParquetValue> pq_values_;
};

class GlobFileReader : public ObFileSqlReader
{
public:
  GlobFileReader() : idx_(0) {}
  virtual ~GlobFileReader() {}
  int init(const std::string &pattern, char *errbuf, const int64_t errbuf_len)
  {
    int ret = common::OB_SUCCESS;
    pattern_ = pattern;
    row_.prepare_allocate(1);
    glob_t glob_result;
    memset(&glob_result, 0, sizeof(glob_result));
    const int rc = glob(pattern.c_str(), 0, NULL, &glob_result);
    if (GLOB_NOMATCH == rc) {
      // no matched file, an empty result
    } else if (0 != rc) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "invalid glob pattern '%.*s'",
                static_cast<int>(pattern.size()), pattern.data());
    } else {
      for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
        struct stat st;
        if (0 == ::stat(glob_result.gl_pathv[i], &st) && S_ISREG(st.st_mode)) {
          files_.push_back(glob_result.gl_pathv[i]);
        }
      }
    }
    globfree(&glob_result);
    std::sort(files_.begin(), files_.end());
    return ret;
  }
  virtual int next_row()
  {
    int ret = common::OB_SUCCESS;
    if (idx_ >= static_cast<int64_t>(files_.size())) {
      ret = common::OB_ITER_END;
    } else {
      row_.at(0).is_null_ = false;
      row_.at(0).string_value_.assign_ptr(const_cast<char *>(files_[idx_].data()),
                                          static_cast<int32_t>(files_[idx_].size()));
      ++idx_;
    }
    return ret;
  }
  virtual int rewind()
  {
    idx_ = 0;
    return common::OB_SUCCESS;
  }

private:
  std::string pattern_;
  std::vector<std::string> files_;
  int64_t idx_;
};

// ------------- schema inference -------------

bool is_null_text(const std::string &field, const bool was_quoted, const ObFileSqlOptions &options)
{
  bool is_null = false;
  if (!was_quoted) {
    if (options.nullstr_.empty()) {
      is_null = field.empty();
    } else if (field.size() == static_cast<size_t>(options.nullstr_.length())
               && 0 == memcmp(field.data(), options.nullstr_.ptr(), field.size())) {
      is_null = true;
    }
  }
  return is_null;
}

void dedup_column_names(std::vector<std::string> &names)
{
  std::vector<std::string> result(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    std::string base = names[i].empty() ? ("column" + std::to_string(i)) : names[i];
    std::string candidate = base;
    int suffix = 1;
    bool conflict = true;
    while (conflict) {
      conflict = false;
      for (size_t j = 0; !conflict && j < result.size(); ++j) {
        if (!result[j].empty() && 0 == result[j].compare(candidate)) {
          conflict = true;
        }
      }
      if (conflict) {
        ++suffix;
        candidate = base + "__" + std::to_string(suffix);
      }
    }
    result[i] = candidate;
  }
  names.swap(result);
}

int build_file_columns(common::ObIAllocator &alloc,
                       const std::vector<std::string> &names,
                       const std::vector<FileValueKind> &kinds,
                       const std::vector<bool> &nullable,
                       const std::vector<int64_t> &max_lengths,
                       common::ObIArray<ObFileSqlColumnMeta> &columns,
                       char *errbuf,
                       const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  for (size_t i = 0; common::OB_SUCCESS == ret && i < names.size(); ++i) {
    ObFileSqlColumnMeta column;
    const common::ObString src(static_cast<int32_t>(names[i].size()),
                               const_cast<char *>(names[i].data()));
    common::ObString copied;
    if (OB_FAIL(common::ob_write_string(alloc, src, copied))) {
      set_error(errbuf, errbuf_len, "out of memory when copying column name");
    } else {
      column.name_ = copied;
      column.obj_type_ = kind_to_obj_type(kinds[i]);
      column.collation_ = common::CS_TYPE_UTF8MB4_GENERAL_CI;
      column.nullable_ = nullable[i];
      column.max_length_ = common::ObVarcharType == column.obj_type_ ? max_lengths[i] : 0;
      column.type_name_ = common::ObString::make_string(KIND_TYPE_NAMES[static_cast<int>(kinds[i])]);
      if (OB_FAIL(columns.push_back(column))) {
      }
    }
  }
  return ret;
}

int infer_csv_columns(common::ObIAllocator &alloc,
                      const std::string &path,
                      const ObFileSqlOptions &options,
                      common::ObIArray<ObFileSqlColumnMeta> &columns,
                      char *errbuf,
                      const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  CsvRecordReader parser;
  std::vector<std::string> fields;
  std::vector<bool> quoted;
  std::vector<std::string> names;
  std::vector<FileValueKind> kinds;
  std::vector<bool> nullable;
  std::vector<int64_t> max_lengths;
  bool header_done = false;
  int64_t expected = -1;
  if (OB_FAIL(parser.open(path, options.delimiter_, options.quote_, options.escape_,
                          options.has_escape_, errbuf, errbuf_len))) {
  }
  while (common::OB_SUCCESS == ret) {
    ret = parser.next_record(fields, quoted, errbuf, errbuf_len);
    if (common::OB_ITER_END == ret) {
      ret = common::OB_SUCCESS;
      break;
    }
    if (common::OB_SUCCESS != ret) {
      break;
    }
    if (!header_done && options.header_) {
      names = fields;
      dedup_column_names(names);
      expected = static_cast<int64_t>(names.size());
      kinds.assign(names.size(), FK_NULL);
      nullable.assign(names.size(), false);
      max_lengths.assign(names.size(), 0);
      header_done = true;
    } else {
      header_done = true;
      if (expected < 0) {
        expected = static_cast<int64_t>(fields.size());
        for (int64_t i = 0; i < expected; ++i) {
          names.push_back("column" + std::to_string(i));
        }
        kinds.assign(expected, FK_NULL);
        nullable.assign(expected, false);
        max_lengths.assign(expected, 0);
      }
      if (static_cast<int64_t>(fields.size()) != expected) {
        ret = common::OB_INVALID_ARGUMENT;
        set_error(errbuf, errbuf_len, "row %ld of '%.*s' has %ld fields, expected %ld",
                  static_cast<long>(parser.record_no()), static_cast<int>(path.size()), path.data(),
                  static_cast<long>(fields.size()), static_cast<long>(expected));
      } else {
        for (int64_t i = 0; i < expected; ++i) {
          if (is_null_text(fields[i], quoted[i], options)) {
            nullable[i] = true;
          } else {
            const FileValueKind kind = classify_text(fields[i].data(),
                                                     static_cast<int64_t>(fields[i].size()));
            kinds[i] = merge_kind(kinds[i], kind);
            if (static_cast<int64_t>(fields[i].size()) > max_lengths[i]) {
              max_lengths[i] = static_cast<int64_t>(fields[i].size());
            }
          }
        }
      }
    }
  }
  if (common::OB_SUCCESS == ret) {
    if (expected <= 0) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "cannot infer schema from empty file '%.*s'",
                static_cast<int>(path.size()), path.data());
    } else if (OB_FAIL(build_file_columns(alloc, names, kinds, nullable, max_lengths, columns,
                                          errbuf, errbuf_len))) {
    }
  }
  return ret;
}

int detect_json_array(const std::string &path, bool &array_mode, char *errbuf,
                      const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  FILE *fp = fopen(path.c_str(), "rb");
  array_mode = false;
  if (NULL == fp) {
    ret = common::OB_FILE_NOT_EXIST;
    set_error(errbuf, errbuf_len, "cannot open file '%.*s': %s",
              static_cast<int>(path.size()), path.data(), strerror(errno));
  } else {
    int c = fgetc(fp);
    // skip UTF-8 BOM
    if (0xEF == c) {
      const int c2 = fgetc(fp);
      const int c3 = fgetc(fp);
      if (0xBB == c2 && 0xBF == c3) {
        c = fgetc(fp);
      } else {
        rewind(fp);
        c = fgetc(fp);
      }
    }
    while (EOF != c && is_space(static_cast<char>(c))) {
      c = fgetc(fp);
    }
    array_mode = ('[' == c);
    if (EOF == c) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "cannot infer schema from empty file '%.*s'",
                static_cast<int>(path.size()), path.data());
    }
    fclose(fp);
  }
  return ret;
}

struct JsonColumnAcc
{
  JsonColumnAcc() : kind_(FK_NULL), nullable_(false), max_length_(0), last_seen_(-1) {}
  std::string name_;
  FileValueKind kind_;
  bool nullable_;
  int64_t max_length_;
  int64_t last_seen_;
};

void merge_json_doc(const std::vector<JsonField> &doc,
                    const int64_t doc_no,
                    std::vector<JsonColumnAcc> &accs,
                    std::map<std::string, int> &index)
{
  std::vector<bool> present(accs.size(), false);
  for (size_t i = 0; i < doc.size(); ++i) {
    const std::string &key = doc[i].key_;
    std::map<std::string, int>::iterator iter = index.find(key);
    int column_idx = 0;
    if (iter == index.end()) {
      column_idx = static_cast<int>(accs.size());
      index[key] = column_idx;
      JsonColumnAcc acc;
      acc.name_ = key;
      accs.push_back(acc);
      present.resize(accs.size(), false);
    } else {
      column_idx = iter->second;
    }
    present[column_idx] = true;
    JsonColumnAcc &acc = accs[column_idx];
    acc.last_seen_ = doc_no;
    const JsonValue &value = doc[i].value_;
    if (FK_NULL == value.kind_) {
      acc.nullable_ = true;
    } else if (FK_STR == value.kind_) {
      const FileValueKind kind = classify_text(value.str_value_.data(),
                                               static_cast<int64_t>(value.str_value_.size()));
      acc.kind_ = merge_kind(acc.kind_, kind);
      if (static_cast<int64_t>(value.str_value_.size()) > acc.max_length_) {
        acc.max_length_ = static_cast<int64_t>(value.str_value_.size());
      }
    } else {
      acc.kind_ = merge_kind(acc.kind_, value.kind_);
      if (FK_INT == value.kind_) {
        acc.max_length_ = std::max<int64_t>(acc.max_length_, 20);
      } else if (FK_DOUBLE == value.kind_) {
        acc.max_length_ = std::max<int64_t>(acc.max_length_, 24);
      }
    }
  }
  // a column that is missing in some documents is nullable
  for (size_t i = 0; i < accs.size(); ++i) {
    if (!present[i]) {
      accs[i].nullable_ = true;
    }
  }
}

void finalize_json_columns(const std::vector<JsonColumnAcc> &accs,
                           std::vector<std::string> &names,
                           std::vector<FileValueKind> &kinds,
                           std::vector<bool> &nullable,
                           std::vector<int64_t> &max_lengths)
{
  for (size_t i = 0; i < accs.size(); ++i) {
    names.push_back(accs[i].name_);
    kinds.push_back(accs[i].kind_);
    nullable.push_back(accs[i].nullable_);
    max_lengths.push_back(accs[i].max_length_);
  }
  dedup_column_names(names);
}

int infer_json_columns(common::ObIAllocator &alloc,
                       const std::string &path,
                       const ObFileSqlOptions &options,
                       common::ObIArray<ObFileSqlColumnMeta> &columns,
                       char *errbuf,
                       const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  bool array_mode = false;
  std::vector<JsonColumnAcc> accs;
  std::map<std::string, int> index;
  if (OB_FAIL(detect_json_array(path, array_mode, errbuf, errbuf_len))) {
  } else if (JSON_FORMAT_ARRAY == options.json_format_) {
    array_mode = true;
  } else if (JSON_FORMAT_NDJSON == options.json_format_) {
    array_mode = false;
  }
  if (common::OB_SUCCESS == ret) {
    if (array_mode) {
      std::string content;
      std::string err;
      std::vector<std::vector<JsonField> > docs;
      if (OB_FAIL(read_entire_file(path, content, errbuf, errbuf_len))) {
      } else if (OB_FAIL(parse_json_array_file(content, docs, err))) {
        set_error(errbuf, errbuf_len, "invalid json array in '%.*s': %s",
                  static_cast<int>(path.size()), path.data(), err.c_str());
      } else {
        for (size_t i = 0; i < docs.size(); ++i) {
          merge_json_doc(docs[i], static_cast<int64_t>(i), accs, index);
        }
      }
    } else {
      TextLineReader line_reader;
      if (OB_FAIL(line_reader.open(path, errbuf, errbuf_len))) {
      } else {
        std::string line;
        int64_t row_no = 0;
        while (common::OB_SUCCESS == ret) {
          ret = line_reader.next_line(line, errbuf, errbuf_len);
          if (common::OB_ITER_END == ret) {
            ret = common::OB_SUCCESS;
            break;
          } else if (common::OB_SUCCESS != ret) {
            break;
          } else {
            bool only_space = true;
            for (size_t k = 0; only_space && k < line.size(); ++k) {
              only_space = is_space(line[k]);
            }
            if (only_space) {
              continue;
            }
            ++row_no;
            JsonTextParser parser(line.data(), line.size());
            std::vector<JsonField> doc;
            if (!parser.parse_object(doc)) {
              ret = common::OB_INVALID_ARGUMENT;
              set_error(errbuf, errbuf_len, "invalid json at row %ld of '%.*s': %s",
                        static_cast<long>(row_no), static_cast<int>(path.size()), path.data(),
                        parser.error().c_str());
            } else {
              parser.skip_ws();
              if (!parser.at_end()) {
                ret = common::OB_INVALID_ARGUMENT;
                set_error(errbuf, errbuf_len, "trailing characters at row %ld of '%.*s'",
                          static_cast<long>(row_no), static_cast<int>(path.size()), path.data());
              } else {
                merge_json_doc(doc, row_no, accs, index);
              }
            }
          }
        }
      }
    }
  }
  if (common::OB_SUCCESS == ret) {
    if (accs.empty()) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "cannot infer schema from empty file '%.*s'",
                static_cast<int>(path.size()), path.data());
    } else {
      std::vector<std::string> names;
      std::vector<FileValueKind> kinds;
      std::vector<bool> nullable;
      std::vector<int64_t> max_lengths;
      finalize_json_columns(accs, names, kinds, nullable, max_lengths);
      if (OB_FAIL(build_file_columns(alloc, names, kinds, nullable, max_lengths, columns,
                                     errbuf, errbuf_len))) {
      }
    }
  }
  return ret;
}

int parquet_type_to_obj_type(const ParquetLogicalType type,
                             common::ObObjType &obj_type,
                             const char *&type_name)
{
  int ret = common::OB_SUCCESS;
  switch (type) {
    case ParquetLogicalType::INT32_TYPE:
    case ParquetLogicalType::INT64_TYPE:
      obj_type = common::ObIntType;
      type_name = "BIGINT";
      break;
    case ParquetLogicalType::BOOL_TYPE:
      obj_type = common::ObTinyIntType;
      type_name = "BOOLEAN";
      break;
    case ParquetLogicalType::FLOAT_TYPE:
    case ParquetLogicalType::DOUBLE_TYPE:
      obj_type = common::ObDoubleType;
      type_name = "DOUBLE";
      break;
    case ParquetLogicalType::STRING_TYPE:
      obj_type = common::ObVarcharType;
      type_name = "VARCHAR";
      break;
    case ParquetLogicalType::DATE_TYPE:
      obj_type = common::ObDateType;
      type_name = "DATE";
      break;
    case ParquetLogicalType::DATETIME_TYPE:
      obj_type = common::ObDateTimeType;
      type_name = "DATETIME";
      break;
    default:
      ret = common::OB_NOT_SUPPORTED;
      break;
  }
  return ret;
}

int infer_parquet_columns(common::ObIAllocator &alloc,
                          const std::string &path,
                          common::ObIArray<ObFileSqlColumnMeta> &columns,
                          char *errbuf,
                          const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  ParquetReader reader;
  if (0 != reader.open(path)) {
    ret = common::OB_INVALID_ARGUMENT;
    set_error(errbuf, errbuf_len, "cannot read parquet file '%.*s': %s",
              static_cast<int>(path.size()), path.data(), reader.error().c_str());
  } else {
    const std::vector<ParquetColumn> &pq_columns = reader.columns();
    for (size_t i = 0; common::OB_SUCCESS == ret && i < pq_columns.size(); ++i) {
      ObFileSqlColumnMeta column;
      common::ObObjType obj_type = common::ObNullType;
      const char *type_name = NULL;
      if (OB_SUCCESS != parquet_type_to_obj_type(pq_columns[i].type, obj_type, type_name)) {
        // keep the column so that the table schema is complete, it can not be read
        // unless the column is projected (column pruning keeps other columns usable)
        obj_type = common::ObNullType;
        type_name = "UNSUPPORTED";
      }
      {
        const common::ObString src(static_cast<int32_t>(pq_columns[i].name.size()),
                                   const_cast<char *>(pq_columns[i].name.data()));
        common::ObString copied;
        if (OB_FAIL(common::ob_write_string(alloc, src, copied))) {
          set_error(errbuf, errbuf_len, "out of memory when copying column name");
        } else {
          column.name_ = copied;
          column.obj_type_ = obj_type;
          column.collation_ = common::CS_TYPE_UTF8MB4_GENERAL_CI;
          column.nullable_ = pq_columns[i].nullable;
          column.max_length_ = common::ObVarcharType == obj_type ? 65535 : 0;
          column.type_name_ = common::ObString::make_string(type_name);
          if (OB_FAIL(columns.push_back(column))) {
          }
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

int get_file_identity(const common::ObString &path,
                      ObFileSqlIdentity &identity,
                      char *errbuf,
                      const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  std::string path_str;
  make_std_string(path, path_str);
  struct stat st;
  if (0 != ::stat(path_str.c_str(), &st)) {
    ret = common::OB_FILE_NOT_EXIST;
    set_error(errbuf, errbuf_len, "cannot access file '%.*s': %s",
              static_cast<int>(path_str.size()), path_str.data(), strerror(errno));
  } else if (!S_ISREG(st.st_mode)) {
    ret = common::OB_INVALID_ARGUMENT;
    set_error(errbuf, errbuf_len, "'%.*s' is not a regular file",
              static_cast<int>(path_str.size()), path_str.data());
  } else {
    identity.size_ = static_cast<int64_t>(st.st_size);
    identity.mtime_ = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL
                      + static_cast<int64_t>(st.st_mtim.tv_nsec);
    identity.inode_ = static_cast<int64_t>(st.st_ino);
    identity.device_ = static_cast<int64_t>(st.st_dev);
  }
  return ret;
}

int infer_file_schema(common::ObIAllocator &alloc,
                      const FileSqlFormatType format,
                      const common::ObString &path,
                      const ObFileSqlOptions &options,
                      common::ObIArray<ObFileSqlColumnMeta> &columns,
                      ObFileSqlIdentity &identity,
                      char *errbuf,
                      const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  std::string path_str;
  make_std_string(path, path_str);
  if (FILE_SQL_FORMAT_CSV == format) {
    if (OB_FAIL(get_file_identity(path, identity, errbuf, errbuf_len))) {
    } else if (OB_FAIL(infer_csv_columns(alloc, path_str, options, columns, errbuf, errbuf_len))) {
    }
  } else if (FILE_SQL_FORMAT_JSONL == format) {
    if (OB_FAIL(get_file_identity(path, identity, errbuf, errbuf_len))) {
    } else if (OB_FAIL(infer_json_columns(alloc, path_str, options, columns, errbuf, errbuf_len))) {
    }
  } else if (FILE_SQL_FORMAT_PARQUET == format) {
    if (OB_FAIL(get_file_identity(path, identity, errbuf, errbuf_len))) {
    } else if (OB_FAIL(infer_parquet_columns(alloc, path_str, columns, errbuf, errbuf_len))) {
    }
  } else if (FILE_SQL_FORMAT_GLOB == format) {
    ObFileSqlColumnMeta column;
    if (OB_FAIL(common::ob_write_string(alloc, common::ObString::make_string("file"), column.name_))) {
    } else {
      column.obj_type_ = common::ObVarcharType;
      column.collation_ = common::CS_TYPE_UTF8MB4_GENERAL_CI;
      column.nullable_ = false;
      column.max_length_ = 65535;
      column.type_name_ = common::ObString::make_string("VARCHAR");
      if (OB_FAIL(columns.push_back(column))) {
      }
    }
  } else {
    ret = common::OB_INVALID_ARGUMENT;
    set_error(errbuf, errbuf_len, "unknown file format");
  }
  return ret;
}

int create_file_reader(common::ObIAllocator &alloc,
                       const FileSqlFormatType format,
                       const common::ObString &path,
                       const ObFileSqlOptions &options,
                       const common::ObIArray<ObFileSqlColumnMeta> &columns,
                       const common::ObIArray<int64_t> &projection,
                       ObFileSqlReader *&reader,
                       char *errbuf,
                       const int64_t errbuf_len)
{
  int ret = common::OB_SUCCESS;
  std::string path_str;
  make_std_string(path, path_str);
  reader = NULL;
  for (int64_t i = 0; common::OB_SUCCESS == ret && i < projection.count(); ++i) {
    if (projection.at(i) < 0 || projection.at(i) >= columns.count()) {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "invalid projected column index");
    }
  }
  if (common::OB_SUCCESS == ret) {
    if (FILE_SQL_FORMAT_CSV == format) {
      CsvFileReader *csv_reader = new (std::nothrow) CsvFileReader();
      if (NULL == csv_reader) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(csv_reader->init(path_str, options, columns, projection, errbuf,
                                          errbuf_len))) {
        delete csv_reader;
        csv_reader = NULL;
      }
      reader = csv_reader;
    } else if (FILE_SQL_FORMAT_JSONL == format) {
      bool array_mode = false;
      if (OB_FAIL(detect_json_array(path_str, array_mode, errbuf, errbuf_len))) {
      } else if (JSON_FORMAT_ARRAY == options.json_format_) {
        array_mode = true;
      } else if (JSON_FORMAT_NDJSON == options.json_format_) {
        array_mode = false;
      }
      if (common::OB_SUCCESS == ret) {
        JsonFileReader *json_reader = new (std::nothrow) JsonFileReader();
        if (NULL == json_reader) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(json_reader->init(path_str, options, array_mode, columns, projection,
                                             errbuf, errbuf_len))) {
          delete json_reader;
          json_reader = NULL;
        }
        reader = json_reader;
      }
    } else if (FILE_SQL_FORMAT_PARQUET == format) {
      ParquetFileReader *pq_reader = new (std::nothrow) ParquetFileReader();
      if (NULL == pq_reader) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(pq_reader->init(path_str, columns, projection, errbuf, errbuf_len))) {
        delete pq_reader;
        pq_reader = NULL;
      }
      reader = pq_reader;
    } else if (FILE_SQL_FORMAT_GLOB == format) {
      GlobFileReader *glob_reader = new (std::nothrow) GlobFileReader();
      if (NULL == glob_reader) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(glob_reader->init(path_str, errbuf, errbuf_len))) {
        delete glob_reader;
        glob_reader = NULL;
      }
      reader = glob_reader;
    } else {
      ret = common::OB_INVALID_ARGUMENT;
      set_error(errbuf, errbuf_len, "unknown file format");
    }
  }
  if (common::OB_SUCCESS == ret) {
    reader->set_reader_allocator(&alloc);
  } else if (NULL != reader) {
    delete reader;
    reader = NULL;
  }
  return ret;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
