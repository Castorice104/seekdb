/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <string>
#include "lib/charset/ob_charset.h"
#include "sql/engine/sort/ob_sort_op_impl.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
using StoredRow = ObChunkDatumStore::StoredRow;

StoredRow *make_row(ObArenaAllocator &allocator, const ObDatum &key, int64_t tie)
{
  void *storage = allocator.alloc(sizeof(StoredRow) + 2 * sizeof(ObDatum) + sizeof(int64_t));
  StoredRow *row = new (storage) StoredRow();
  row->cnt_ = 2;
  new (&row->cells()[0]) ObDatum(key);
  new (&row->cells()[1]) ObDatum();
  row->cells()[1].ptr_ = static_cast<const char *>(storage) + sizeof(StoredRow) + 2 * sizeof(ObDatum);
  row->cells()[1].set_int(tie);
  return row;
}

ObDatumCmpFuncType observed_cmp = nullptr;
int comparison_calls = 0;
int count_comparison(const ObDatum &left, const ObDatum &right, int &cmp,
                     const ObDatumAccessContext *context)
{
  ++comparison_calls;
  return observed_cmp(left, right, cmp, context);
}

void add_key(ObSEArray<ObSortFieldCollation, 4> &collations,
             ObSEArray<ObSortCmpFunc, 4> &functions,
             ObObjType type, ObCollationType collation, bool ascending,
             ObCmpNullPos null_pos, uint32_t field = 0, bool lob_header = false)
{
  ASSERT_EQ(OB_SUCCESS, collations.push_back(
      ObSortFieldCollation(field, collation, ascending, null_pos)));
  ObSortCmpFunc function;
  function.cmp_func_ = ObDatumFuncs::get_nullsafe_cmp_func(
      type, type, null_pos, collation, SCALE_UNKNOWN_YET, lob_header);
  ASSERT_NE(nullptr, function.cmp_func_);
  ASSERT_EQ(OB_SUCCESS, functions.push_back(function));
}
} // namespace

// Count the real comparator through a forwarding probe installed after init:
// identical raw strings must reach later keys without decoding collation again.
TEST(SortStringHotPath, identical_bytes_skip_collation_and_keep_tie_order)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  for (auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_BINARY}) {
    ObArenaAllocator allocator;
    ObExecContext context(allocator);
    ObSEArray<ObSortFieldCollation, 4> collations;
    ObSEArray<ObSortCmpFunc, 4> functions;
    add_key(collations, functions, ObVarcharType, collation, true, NULL_FIRST);
    add_key(collations, functions, ObIntType, CS_TYPE_BINARY, false, NULL_LAST, 1);
    ObSortOpImpl::Compare compare;
    ASSERT_EQ(OB_SUCCESS, compare.init(&collations, &functions, &context, false, 1ULL << 0));
    observed_cmp = functions.at(0).cmp_func_;
    functions.at(0).cmp_func_ = count_comparison;
    for (const std::string &text : {std::string(), std::string("same term"),
                                  std::string("a\0b", 3), std::string("\xff\xfe", 2)}) {
      std::string copy = text;
      ObDatum left, right;
      left.set_string(text.data(), text.size());
      right.set_string(copy.data(), copy.size());
      auto *l = make_row(allocator, left, 2);
      auto *r = make_row(allocator, right, 1);
      comparison_calls = 0;
      EXPECT_TRUE(compare(l, r));
      EXPECT_FALSE(compare(r, l));
      EXPECT_EQ(0, comparison_calls);
      EXPECT_EQ(OB_SUCCESS, compare.get_error_code());
    }
  }
}

TEST(SortStringHotPath, collation_nulls_and_direction_preserved)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  struct Sample { const char *left; const char *right; int sign; };
  const Sample samples[] = {
    {"Alpha", "ALPHA", 0}, {"a", "a ", 0}, {"a", "b", -1},
    {"b", "a", 1}, {"", " ", 0}, {"é", "e", 0}, {"中", "国", -1},
  };
  for (bool ascending : {false, true}) {
    for (auto null_pos : {NULL_FIRST, NULL_LAST}) {
      ObArenaAllocator allocator;
      ObExecContext context(allocator);
      ObSEArray<ObSortFieldCollation, 4> collations;
      ObSEArray<ObSortCmpFunc, 4> functions;
      add_key(collations, functions, ObVarcharType, CS_TYPE_UTF8MB4_GENERAL_CI,
              ascending, null_pos);
      ObSortOpImpl::Compare compare;
      ASSERT_EQ(OB_SUCCESS, compare.init(&collations, &functions, &context, false, 1ULL << 0));
      for (const auto &sample : samples) {
        ObDatum left, right;
        left.set_string(ObString::make_string(sample.left));
        right.set_string(ObString::make_string(sample.right));
        auto *l = make_row(allocator, left, 0);
        auto *r = make_row(allocator, right, 0);
        EXPECT_EQ(ascending ? sample.sign < 0 : sample.sign > 0, compare(l, r));
      }
      ObDatum null, empty;
      null.set_null();
      empty.set_string("", 0);
      auto *n = make_row(allocator, null, 0);
      auto *e = make_row(allocator, empty, 0);
      EXPECT_FALSE(compare(n, n));
      EXPECT_EQ(ascending == (null_pos == NULL_FIRST), compare(n, e));
      EXPECT_EQ(OB_SUCCESS, compare.get_error_code());
    }
  }
}

TEST(SortStringHotPath, lob_comparator_is_not_bypassed)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  ObArenaAllocator allocator;
  ObExecContext context(allocator);
  ObSEArray<ObSortFieldCollation, 4> collations;
  ObSEArray<ObSortCmpFunc, 4> functions;
  add_key(collations, functions, ObLongTextType, CS_TYPE_UTF8MB4_GENERAL_CI,
          true, NULL_FIRST, 0, true);
  ObSortOpImpl::Compare compare;
  ASSERT_EQ(OB_SUCCESS, compare.init(&collations, &functions, &context, false, 0));
  // A valid nonnull in-row LOB must still reach its registered comparator.
  observed_cmp = functions.at(0).cmp_func_;
  functions.at(0).cmp_func_ = count_comparison;
  void *buffer = allocator.alloc(sizeof(ObLobCommon) + 4);
  auto *lob = new (buffer) ObLobCommon();
  MEMCPY(lob->buffer_, "same", 4);
  ObDatum value;
  value.set_string(static_cast<const char *>(buffer), sizeof(ObLobCommon) + 4);
  auto *row = make_row(allocator, value, 0);
  comparison_calls = 0;
  EXPECT_FALSE(compare(row, row));
  EXPECT_EQ(1, comparison_calls);
  EXPECT_EQ(OB_SUCCESS, compare.get_error_code());
}
