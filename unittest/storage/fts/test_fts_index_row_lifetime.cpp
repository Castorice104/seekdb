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
#include <map>
#include <string>
#include "lib/charset/ob_charset.h"
#include "storage/tx/ob_trans_define.h"
#include "sql/das/ob_das_domain_utils.h"
#include "storage/blocksstable/ob_datum_row.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;

TEST(FtsIndexLifetime, row_cache_retains_words_after_input_buffer_is_reused)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  for (const bool word_first : {false, true}) {
    ObFTIndexRowCache cache;
    ASSERT_EQ(OB_SUCCESS, cache.init(word_first, ObString::make_string("ngram.1"), ObString()));
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    int64_t id_value = 42;
    ObDatum id;
    id.ptr_ = reinterpret_cast<const char *>(&id_value);
    id.set_int(id_value);
    std::string document = "数据库全文索引";
    ASSERT_EQ(OB_SUCCESS, cache.segment(meta, id, ObString(document.size(), document.data())));
    // FTS DDL resolves an out-of-row LOB through a temporary allocator before
    // it starts consuming cached rows. Reusing the source is deterministic and
    // tests the same lifetime boundary without relying on allocator poisoning.
    std::fill(document.begin(), document.end(), 'x');
    std::map<std::string, int64_t> actual;
    blocksstable::ObDatumRow *row = nullptr;
    int ret = OB_SUCCESS;
    while ((ret = cache.get_next_row(row)) == OB_SUCCESS) {
      ASSERT_NE(nullptr, row);
      const auto word = row->storage_datums_[word_first ? 0 : 1].get_string();
      actual[std::string(word.ptr(), word.length())] = row->storage_datums_[2].get_int();
      EXPECT_EQ(42, row->storage_datums_[word_first ? 1 : 0].get_int());
      EXPECT_EQ(6, row->storage_datums_[3].get_int());
    }
    EXPECT_EQ(OB_ITER_END, ret);
    const std::map<std::string, int64_t> expected = {
      {"数据",1}, {"据库",1}, {"库全",1}, {"全文",1}, {"文索",1}, {"索引",1}
    };
    EXPECT_EQ(expected, actual);
  }
}

TEST(FtsIndexLifetime, repeated_documents_release_source_and_reuse_cache)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  for (const bool word_first : {false, true}) {
    ObFTIndexRowCache cache;
    ASSERT_EQ(OB_SUCCESS, cache.init(word_first, ObString::make_string("ngram.1"), ObString()));
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    for (int repeat = 1; repeat <= 12; ++repeat) {
      int64_t id_value = repeat;
      ObDatum id;
      id.ptr_ = reinterpret_cast<const char *>(&id_value);
      id.set_int(id_value);
      {
        std::string source;
        for (int n = 0; n < repeat; ++n) { source += "数据库全文索引 "; }
        ASSERT_EQ(OB_SUCCESS, cache.segment(meta, id, ObString(source.size(), source.data())));
      }
      blocksstable::ObDatumRow *row = nullptr;
      std::map<std::string, int64_t> actual;
      int ret;
      while ((ret = cache.get_next_row(row)) == OB_SUCCESS) {
        const auto word = row->storage_datums_[word_first ? 0 : 1].get_string();
        actual[std::string(word.ptr(), word.length())] = row->storage_datums_[2].get_int();
        EXPECT_EQ(repeat, row->storage_datums_[word_first ? 1 : 0].get_int());
        EXPECT_EQ(6 * repeat, row->storage_datums_[3].get_int());
      }
      EXPECT_EQ(OB_ITER_END, ret);
      const std::map<std::string, int64_t> expected = {
        {"数据",repeat}, {"据库",repeat}, {"库全",repeat},
        {"全文",repeat}, {"文索",repeat}, {"索引",repeat}
      };
      EXPECT_EQ(expected, actual);
      EXPECT_EQ(OB_ITER_END, cache.segment(meta, id, ObString()));
      EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
    }
  }
}
