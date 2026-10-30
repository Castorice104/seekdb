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

#include <gtest/gtest.h>
#include "lib/charset/ob_charset.h"
#include "share/text_analysis/ob_token_stream.h"
#include <string>
#include <vector>

using namespace oceanbase;
using namespace oceanbase::common;

namespace
{
unsigned int (*original_character_length)(const ObCharsetInfo *, unsigned int) = nullptr;
int64_t character_length_calls = 0;
unsigned int count_character_length(const ObCharsetInfo *cs, unsigned int ch)
{
  ++character_length_calls;
  return original_character_length(cs, ch);
}
std::vector<std::string> scan_tokens(const std::string &document, const ObCharsetInfo *cs)
{
  share::ObTextWhitespaceTokenizer scanner;
  ObDatum input;
  input.set_string(document.data(), document.size());
  EXPECT_EQ(OB_SUCCESS, scanner.open(input, cs));
  std::vector<std::string> result;
  ObDatum word;
  int64_t frequency = 0;
  int ret = OB_SUCCESS;
  while ((ret = scanner.get_next(word, frequency)) == OB_SUCCESS) {
    EXPECT_EQ(1, frequency);
    result.emplace_back(word.ptr_, static_cast<uint32_t>(word.len_));
  }
  EXPECT_EQ(OB_ITER_END, ret);
  return result;
}
}

TEST(FtsTokenScan, ascii_scan_avoids_multibyte_callback_per_byte)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const auto *base = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ASSERT_NE(nullptr, base);
  ObCharsetInfo cs = *base;
  ObCharsetHandler handler = *base->cset;
  original_character_length = handler.mbcharlen;
  handler.mbcharlen = count_character_length;
  cs.cset = &handler;
  character_length_calls = 0;
  const std::string document = "One,two THREE;four 123_456\tEND.";
  const std::vector<std::string> expected = {"One", "two", "THREE", "four", "123", "456", "END"};
  EXPECT_EQ(expected, scan_tokens(document, &cs));
  EXPECT_LT(character_length_calls, static_cast<int64_t>(document.size() / 8));
}

TEST(FtsTokenScan, all_ascii_bytes_follow_charset_delimiters)
{
  const auto *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ASSERT_NE(nullptr, cs);
  for (int ch = 0; ch < 128; ++ch) {
    const std::string document(1, static_cast<char>(ch));
    const bool delimiter = ob_isspace(cs, ch) || ob_iscntrl(cs, ch) || ob_ispunct(cs, ch);
    const auto expected = delimiter ? std::vector<std::string>{} : std::vector<std::string>{document};
    EXPECT_EQ(expected, scan_tokens(document, cs)) << ch;
  }
}

TEST(FtsTokenScan, mixed_utf8_and_invalid_lead_keep_boundaries)
{
  const auto *cs = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  const std::vector<std::string> expected = {"Alpha", "中文", "测试", "Café", "123", "😀42"};
  EXPECT_EQ(expected, scan_tokens("Alpha,中文 测试! Café-123 😀42", cs));
  EXPECT_EQ(std::vector<std::string>{"before"}, scan_tokens(std::string("before") + char(0xff) + " after", cs));
  EXPECT_EQ(std::vector<std::string>{}, scan_tokens(" \t\n\r!.,", cs));
}

TEST(FtsTokenScan, other_charset_retains_decoder_callback)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const auto *base = ObCharset::get_charset(CS_TYPE_BINARY);
  ASSERT_NE(nullptr, base);
  ObCharsetInfo cs = *base;
  ObCharsetHandler handler = *base->cset;
  original_character_length = handler.mbcharlen;
  handler.mbcharlen = count_character_length;
  cs.cset = &handler;
  character_length_calls = 0;
  EXPECT_EQ((std::vector<std::string>{"Alpha", "42"}), scan_tokens("Alpha 42", &cs));
  EXPECT_GT(character_length_calls, 0);
}
