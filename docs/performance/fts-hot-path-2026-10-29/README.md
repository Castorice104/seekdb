# Fulltext hot-path optimization and local validation

Base commit: `a6ac70f0f2db634213ca5ceadce6e43ce1449952`.

The change reduces per-document synchronization, hashing, character decoding, and scratch allocation. It preserves parser properties, tokens and representative spelling, word frequencies, MATCH scores and returned rows, and transactional visibility.

## Bottleneck evidence and changes

Inspection found a fresh word map initialized and cleared with bucket locks in every document parse, plus a second hash lookup and replacement-node allocation for every duplicate token. All five map owners (TOKENIZE, DAS index-row expansion, optimizer query-token extraction, and current/legacy retrieval iterators) are local variables consumed synchronously. Their map now uses the existing `NoPthreadDefendMode`; global dictionary and stopword maps retain their synchronization. Empty scratch maps are not redundantly cleared. One-pass insert/update preserves the original initial count and latest spelling even when two byte strings compare equal under the selected collation.

IK classification decoded the same code point once for each classification predicate; it now decodes once and retains the exact predicate order and error results. Processor dispatch consumes the character metadata already available in the caller, in the original processor order.

English normalization reused an arena and allocated a new lowercase buffer for every token. It now retains scratch capacity until growth or document reuse/reset, while using the same charset `casedn` callbacks and in-place/out-of-place rules as `ObCharset::tolower`. The existing downstream token copy still establishes the document lifetime.

A 567-sample user-space CPU profile confirmed allocator, latch, charset/lowercase, dictionary, and retrieval work; it is a broad sample, not a precise attribution of total latency to each change. Raw samples and symbolized output are retained in local evidence. No shared sorter, storage format, benchmark SQL/data/timers/scoring baseline, or model behavior was changed.

## Reproduction and measurements

Environment: WSL2 Ubuntu 22.04, RelWithDebInfo build, the same database base directory and server parameters for both binaries: `memory_budget=2G`, `cpu_count=2`, `datafile_size=2G`, `datafile_maxsize=4G`, `log_disk_size=2G`, MySQL port 2885. The original binary was saved before source changes. Each complete run used 20000 documents, batch 500, tokenize rounds 3000, query rounds 200, samples 3, warmup 30, and SKIP_LOAD=0, with the official script unchanged. Separate fresh report files were used.

All six raw reports are included. They record the base SHA plus a dirty working tree because measurement preceded the final commit; `validated-source-sha256.json` identifies the tested source files in this commit. The initial before/after pair was collected on October 28; after an overnight instance shutdown, the primary same-day October 29 comparison used B-A-A-B order (`after-repeat2`, `before-repeat2`, `before-repeat3`, `after-repeat3`). The table compares the arithmetic means of the two same-day complete runs per binary. Values are seconds for build metrics and milliseconds for tokenize/query metrics. Machine load and timing variability limit extrapolation to CI.

| Metric | Before mean | After mean | Time reduction |
|---|---:|---:|---:|
| `build_ik_all_sec` | 9.543000 | 8.859000 | +7.168% |
| `build_ik_content_sec` | 7.575500 | 6.712000 | +11.399% |
| `build_beng_en_sec` | 3.968000 | 3.547500 | +10.597% |
| `tokenize_ik_avg_ms` | 0.288150 | 0.224900 | +21.950% |
| `tokenize_beng_avg_ms` | 0.189950 | 0.164700 | +13.293% |
| `query_cn_avg_ms` | 1.936350 | 1.915050 | +1.100% |
| `query_beng_avg_ms` | 2.836400 | 2.849450 | -0.460% |
| `query_mixed_avg_ms` | 2.437900 | 2.213000 | +9.225% |
| `query_limit_avg_ms` | 2.766850 | 2.199650 | +20.500% |

Category improvements: build **9.721%**, tokenize **17.622%**, query **7.591%**. Equal category mean: **11.645%**. The English query's **-0.460%** reduction is a measured regression and remains included in the arithmetic. This is below the 50% reduction target for a full performance score.

The unchanged official scoring script accepts every same-day report and produces 100/100 against its fixed CI baseline for BOTH the original and optimized binaries on this machine. That result is therefore not evidence that this patch earned a full official score; the local net improvement above is the useful comparison. Official competition results require its uniform environment.

All benchmark query hit counts match: Chinese 8001, English 11000, mixed 7332, limited query 20. These counts supplement, rather than replace, complete semantic validation.

## Correctness validation

- Production `seekdb` build completed successfully.
- Four new native cases cover Unicode classes and unsupported charset behavior, word frequencies and map reuse, collation-equal latest spelling, English normalization with a 10000-byte word, growth, and repeated reuse. Baseline semantic tests were run before implementation; final native tests are 4/4.
- Original mysqltests `tokenize_function`, `cn_word`, `index_view`, and `simple_query` pass against their existing expected outputs, before and after. The local harness supplied host/port variables and the test admin account; it did not change their SQL assertions or expected outputs.
- `unittest/storage/fts/run_fts_differential.py` records 1118 complete observations across fixed/random multilingual corpus, all/default output modes, IK smart/max-word modes, BENG/space/ngram, invalid/unsupported input, full MATCH row/score results, natural/boolean queries, LIMIT, updates, deletion, rollback, committed updates, and six concurrent independent sessions. Baseline/after snapshots are identical (SHA-256 `80fbfc3134be918fd9a2e631835f72e4d2473b165e0aa6c7b6c6f3380d738aee`). Error-code observations preserve baseline behavior; they are not claimed as successful charset support.
- All three CTest targets pass. Separate prior-feature regressions (8 AI-column mysqltests, 12 AI-column integration tests, AI-function checks, and file SQL) pass as additional compatibility checks; they do not contribute to this fulltext question's performance or correctness score.
- A fresh independent static review found no actionable Blocker/Major defect; its scope and limits are recorded in `review.md`.
- SHA-256 and `git show` comparisons verify all five benchmark source/data-generation/setup/scoring/baseline files are byte-for-byte unchanged from the base commit. No workflow changes were made.

Run differential coverage using a Python environment with PyMySQL:

```bash
python unittest/storage/fts/run_fts_differential.py --port 2885 --snapshot /tmp/fts-before.json
# Restart using the other binary with identical configuration, then:
python unittest/storage/fts/run_fts_differential.py --port 2885 --snapshot /tmp/fts-after.json --compare /tmp/fts-before.json
```

Run the official benchmark with the parameters in the question and a fresh output file. The differential fixture recreates only its dedicated `fts_hot_path_differential` database; the benchmark recreates `fts_large_bench`. Use a dedicated local test instance.
