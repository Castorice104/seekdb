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
#include "storage/fts/ob_beng_ft_parser.h"
#include "lib/charset/ob_charset.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::storage;
class ParserBenchAllocator : public ObIAllocator
{
public:
  void *alloc(const int64_t size) override { ++calls_; bytes_ += size; return arena_.alloc(size); }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *) override {}
  ObArenaAllocator arena_;
  int64_t calls_ = 0;
  int64_t bytes_ = 0;
};
int main(int argc, char **argv)
{
  if (argc != 3) { std::cerr << "usage: fts_parser_microbench DOCUMENT_FILE ROUNDS\n"; return 2; }
  std::ifstream file(argv[1]);
  if (!file) { return 2; }
  const std::string document((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const int rounds = std::stoi(argv[2]);
  if (document.empty() || rounds <= 0 || ObCharset::init_charset() != OB_SUCCESS) { return 2; }
  ParserBenchAllocator allocator;
  int64_t tokens = 0;
  uint64_t checksum = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < rounds; ++i) {
    {
      ObBEngFTParser parser(allocator);
      ObFTParserParam param;
      param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
      param.fulltext_ = document.data(); param.ft_length_ = document.size(); param.allocator_ = &allocator;
      if (parser.init(&param) != OB_SUCCESS) { return 1; }
      const char *word = nullptr;int64_t length = 0, characters = 0, frequency = 0;
      int ret = OB_SUCCESS;
      while ((ret = parser.get_next_token(word,length,characters,frequency)) == OB_SUCCESS) {
        ++tokens;checksum += length + frequency + static_cast<unsigned char>(word[0]);
      }
      if (ret != OB_ITER_END) { return 1; }
    }
    allocator.arena_.reuse();
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  std::cout << "{\"rounds\":" << rounds << ",\"tokens\":" << tokens << ",\"seconds\":" << seconds
            << ",\"allocations\":" << allocator.calls_ << ",\"allocated_bytes\":" << allocator.bytes_
            << ",\"checksum\":" << checksum << "}\n";
}
