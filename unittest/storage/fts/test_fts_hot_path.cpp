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
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "share/text_analysis/ob_token_stream.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::storage;

TEST(FtsHotPath, unicode_character_classes)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  using Type = ObFTCharUtil::CharType;
  struct Sample { const char *text; Type type; };
  const Sample samples[] = {{"A",Type::ENGLISH_LETTER},{"z",Type::ENGLISH_LETTER},
    {"9",Type::ARABIC_LETTER},{".",Type::USELESS},{" ",Type::USELESS},
    {"Ａ",Type::ENGLISH_LETTER},{"ｚ",Type::ENGLISH_LETTER},
    {"９",Type::ARABIC_LETTER},{"中",Type::CHINESE},{"㐀",Type::CHINESE},
    {"か",Type::OTHER_CJK},{"한",Type::OTHER_CJK},{"😀",Type::USELESS}};
  for (const auto &s : samples) {
    Type actual = Type::USELESS;
    ASSERT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN,s.text,strlen(s.text),actual));
    EXPECT_EQ(s.type,actual) << s.text;
  }
  // Some seekdb builds expose only UTF8MB4/BINARY collations. Preserve both
  // the unsupported-charset result and surrogate validation when available.
  const ObCollationType utf16_types[] = {CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN};
  for (const auto collation : utf16_types) {
    const bool little = collation == CS_TYPE_UTF16LE_BIN;
    const char high[] = {little ? char(0) : char(0xd8), little ? char(0xd8) : char(0)};
    Type actual = Type::USELESS;
    const auto charset = ObCharset::charset_type_by_coll(collation);
    const int expected = charset == CHARSET_UTF16 || charset == CHARSET_UTF16LE
        ? OB_UNEXPECT_INTERNAL_ERROR : OB_NOT_SUPPORTED;
    EXPECT_EQ(expected,ObFTCharUtil::classify_first_char(collation,high,2,actual));
  }
  Type actual = Type::USELESS;
  EXPECT_EQ(OB_NOT_SUPPORTED,ObFTCharUtil::classify_first_char(CS_TYPE_LATIN1_BIN,"a",1,actual));
}

TEST(FtsHotPath, word_frequency_and_collation_preserved)
{
  ObArenaAllocator alloc;
  ObObjMeta meta; meta.set_varchar();meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObFTWordMap words; ASSERT_EQ(OB_SUCCESS,words.create(17,"FtsTest"));
  ObAddWordFlag flags; flags.set_groupby_word();flags.set_casedown();
  ObFTParserProperty property;
  ObAddWord add(property,meta,flags,alloc,words);
  ASSERT_EQ(OB_SUCCESS,add.process_word("Alpha",5,5,1));
  ASSERT_EQ(OB_SUCCESS,add.process_word("ALPHA",5,5,3));
  ASSERT_EQ(OB_SUCCESS,add.process_word("Beta",4,4,1));
  int64_t frequency=0;
  ASSERT_EQ(OB_SUCCESS,words.get_refactored(ObFTWord(5,"alpha",meta),frequency));
  EXPECT_EQ(4,frequency); EXPECT_EQ(2,words.size());
  ASSERT_EQ(OB_SUCCESS,words.reuse()); EXPECT_EQ(0,words.size());
  ASSERT_EQ(OB_SUCCESS,add.process_word("Gamma",5,5,2));
  ASSERT_EQ(OB_SUCCESS,words.get_refactored(ObFTWord(5,"gamma",meta),frequency));
  EXPECT_EQ(1,frequency); // preserve the existing initial-frequency behavior
}

TEST(FtsHotPath, english_normalization_growth_and_reuse)
{
  ObCharsetInfo const *cs=ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ASSERT_NE(nullptr,cs);
  share::ObTextWhitespaceTokenizer tokenizer;
  share::ObBasicEnglishNormalizer normalizer;
  ASSERT_EQ(OB_SUCCESS,normalizer.init(cs,tokenizer));
  const std::string long_word(10000,'Q');
  const std::string document="HELLO, world! 123 "+long_word+" FINAL";
  for (int repeat=0;repeat<3;++repeat) {
    if(repeat>0)normalizer.reuse();
    ObDatum input;input.set_string(document.data(),document.size());
    ASSERT_EQ(OB_SUCCESS,tokenizer.open(input,cs));
    const std::string expected[]={"hello","world","123",std::string(10000,'q'),"final"};
    for(const auto &value:expected){
      ObDatum token;int64_t freq=0;
      ASSERT_EQ(OB_SUCCESS,normalizer.get_next(token,freq));
      EXPECT_EQ(value,std::string(token.ptr_,token.len_));EXPECT_EQ(1,freq);
    }
    ObDatum token;int64_t freq=0;EXPECT_EQ(OB_ITER_END,normalizer.get_next(token,freq));
  }
  normalizer.reset();
}

TEST(FtsHotPath, collation_equal_words_keep_latest_spelling)
{
  ObArenaAllocator alloc;
  ObObjMeta meta; meta.set_varchar();meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObFTWordMap words; ASSERT_EQ(OB_SUCCESS,words.create(17,"FtsTest"));
  ObAddWordFlag flags; flags.set_groupby_word();
  ObFTParserProperty property;
  ObAddWord add(property,meta,flags,alloc,words);
  ASSERT_EQ(OB_SUCCESS,add.process_word("Alpha",5,5,1));
  ASSERT_EQ(OB_SUCCESS,add.process_word("ALPHA",5,5,1));
  ASSERT_EQ(1,words.size());
  EXPECT_EQ(ObString::make_string("ALPHA"),words.begin()->first.get_word().get_string());
  EXPECT_EQ(2,words.begin()->second);
}

TEST(FtsHotPath, casedown_identity_fast_path_matches_charset_tolower)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const std::string words[] = {
    "ALPHA", "Alpha", "alpha", "中文分词", "ＡＢＣ", "Ωμέγα", "ПРИВЕТ",
    "😀Emoji", "a\0b", "long" + std::string(4096, 'Q')
  };
  const ObCollationType collation = CS_TYPE_UTF8MB4_GENERAL_CI;
  for (const std::string &value : words) {
    ObArenaAllocator alloc;
    ObArenaAllocator expected_alloc;
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    ObFTWordMap word_map;
    ASSERT_EQ(OB_SUCCESS, word_map.create(17, "FtsCaseTest"));
    ObAddWordFlag flags;
    flags.set_groupby_word();
    flags.set_casedown();
    ObFTParserProperty property;
    ObAddWord add(property, meta, flags, alloc, word_map);
    ASSERT_EQ(OB_SUCCESS, add.process_word(value.data(), value.size(), 1, 1));
    ASSERT_EQ(1, word_map.size());
    ObString expected;
    ASSERT_EQ(OB_SUCCESS, ObCharset::tolower(
        collation, ObString(static_cast<int32_t>(value.size()), value.data()),
        expected, expected_alloc));
    const ObString actual = word_map.begin()->first.get_word().get_string();
    ASSERT_EQ(expected.length(), actual.length()) << value;
    EXPECT_EQ(0, MEMCMP(expected.ptr(), actual.ptr(), expected.length())) << value;
    EXPECT_EQ(1, word_map.begin()->second);
  }
}

namespace
{
struct ExpectedIKToken
{
  int64_t offset;
  int64_t length;
  int64_t char_count;
  ObIKTokenType type;
};

void expect_ik_tokens(TokenizeContext &ctx,
                      std::initializer_list<ExpectedIKToken> expected)
{
  ASSERT_EQ(expected.size(), ctx.result_list().size());
  auto actual = ctx.result_list().begin();
  for (const auto &token : expected) {
    EXPECT_EQ(ctx.fulltext(), actual->ptr_);
    EXPECT_EQ(token.offset, actual->offset_);
    EXPECT_EQ(token.length, actual->length_);
    EXPECT_EQ(token.char_count, actual->char_cnt_);
    EXPECT_EQ(token.type, actual->type_);
    ++actual;
  }
}
}

TEST(FtsHotPath, ik_arbitrator_preserves_overlapping_tokens_and_smart_choice)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const char *text = "abcdefgh ij";
  const auto english = ObIKTokenType::IK_ENGLISH_TOKEN;
  for (const bool smart : {false, true}) {
    ObArenaAllocator alloc;
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, alloc, text, strlen(text), smart);
    ASSERT_EQ(OB_SUCCESS, ctx.init());
    // Unsorted insertion also exercises the ordering contract consumed by arbitration.
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 9, 2, 2, english));
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 0, 3, 3, english));
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 3, 3, 3, english));
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 0, 6, 6, english));
    ObIKArbitrator arb;
    ASSERT_EQ(OB_SUCCESS, arb.process(ctx));
    for (int repeat = 0; repeat < 2; ++repeat) {
      ctx.result_list().reset();
      ASSERT_EQ(OB_SUCCESS, arb.output_result(ctx));
      if (smart) {
        expect_ik_tokens(ctx, {{0, 6, 6, english}, {9, 2, 2, english}});
      } else {
        expect_ik_tokens(ctx, {{0, 6, 6, english}, {0, 3, 3, english},
                              {3, 3, 3, english}, {9, 2, 2, english}});
      }
    }
  }
}

TEST(FtsHotPath, ik_arbitrator_preserves_full_document_output_for_each_batch)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const char *text = "甲乙 丙丁 戊己";
  const auto chinese = ObIKTokenType::IK_CHINESE_TOKEN;
  for (const bool smart : {false, true}) {
    ObArenaAllocator alloc;
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, alloc, text, strlen(text), smart);
    ASSERT_EQ(OB_SUCCESS, ctx.init());
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 0, 6, 2, chinese));
    {
      ObIKArbitrator arb;
      ASSERT_EQ(OB_SUCCESS, arb.process(ctx));
      ASSERT_EQ(OB_SUCCESS, arb.output_result(ctx));
      expect_ik_tokens(ctx, {{0, 6, 2, chinese}, {7, 3, 1, chinese},
                            {10, 3, 1, chinese}, {14, 3, 1, chinese},
                            {17, 3, 1, chinese}});
    }
    ASSERT_EQ(OB_SUCCESS, ctx.reset_resource());
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 14, 6, 2, chinese));
    {
      ObIKArbitrator arb;
      ASSERT_EQ(OB_SUCCESS, arb.process(ctx));
      ASSERT_EQ(OB_SUCCESS, arb.output_result(ctx));
      // Existing batches rescan the document, including singles from earlier batches.
      expect_ik_tokens(ctx, {{0, 3, 1, chinese}, {3, 3, 1, chinese},
                            {7, 3, 1, chinese}, {10, 3, 1, chinese},
                            {14, 6, 2, chinese}});
    }
  }
}

TEST(FtsHotPath, ik_arbitrator_empty_chains_keep_cjk_singles)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const char *text = "A 甲 か 한 😀";
  for (const bool smart : {false, true}) {
    ObArenaAllocator alloc;
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, alloc, text, strlen(text), smart);
    ASSERT_EQ(OB_SUCCESS, ctx.init());
    ObIKArbitrator arb;
    ASSERT_EQ(OB_SUCCESS, arb.process(ctx));
    ASSERT_EQ(OB_SUCCESS, arb.output_result(ctx));
    expect_ik_tokens(ctx, {{2, 3, 1, ObIKTokenType::IK_CHINESE_TOKEN},
                          {6, 3, 1, ObIKTokenType::IK_OTHER_CJK_TOKEN},
                          {10, 3, 1, ObIKTokenType::IK_OTHER_CJK_TOKEN}});
  }
}

TEST(FtsHotPath, ik_arbitrator_many_disjoint_chains_keep_order)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  std::string text;
  for (int i = 0; i < 300; ++i) {
    text += "word ";
  }
  ObArenaAllocator alloc;
  TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, alloc, text.data(), text.size(), false);
  ASSERT_EQ(OB_SUCCESS, ctx.init());
  for (int i = 299; i >= 0; --i) {
    ASSERT_EQ(OB_SUCCESS, ctx.add_token(text.data(), i * 5, 4, 4,
                                      ObIKTokenType::IK_ENGLISH_TOKEN));
  }
  ObIKArbitrator arb;
  ASSERT_EQ(OB_SUCCESS, arb.process(ctx));
  ASSERT_EQ(OB_SUCCESS, arb.output_result(ctx));
  ASSERT_EQ(300, ctx.result_list().size());
  int64_t offset = 0;
  for (const auto &token : ctx.result_list()) {
    EXPECT_EQ(offset, token.offset_);
    EXPECT_EQ(4, token.length_);
    EXPECT_EQ(4, token.char_cnt_);
    EXPECT_EQ(ObIKTokenType::IK_ENGLISH_TOKEN, token.type_);
    EXPECT_EQ(text.data(), token.ptr_);
    offset += 5;
  }
}

TEST(FtsHotPath, ik_arbitrator_smart_choice_can_move_chain_start)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const char *text = "甲乙丙丁 戊己";
  const auto chinese = ObIKTokenType::IK_CHINESE_TOKEN;
  ObArenaAllocator alloc;
  TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, alloc, text, strlen(text), true);
  ASSERT_EQ(OB_SUCCESS, ctx.init());
  ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 0, 9, 3, chinese));
  ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 3, 9, 3, chinese));
  ASSERT_EQ(OB_SUCCESS, ctx.add_token(text, 13, 6, 2, chinese));
  ObIKArbitrator arb;
  ASSERT_EQ(OB_SUCCESS, arb.process(ctx));
  ASSERT_EQ(OB_SUCCESS, arb.output_result(ctx));
  // The equal-length overlap chooses the later ending token, leaving a single
  // character before the selected chain and a separate chain after the space.
  expect_ik_tokens(ctx, {{0, 3, 1, chinese}, {3, 9, 3, chinese},
                        {13, 6, 2, chinese}});
}
