# Fulltext hot-path optimization (round 4, 2026-10-30)

Base: `feat/fts-performance`, parent `97b23f410`. This round keeps the previous
round's tokenizer work and adds four localized hot-path changes. It does not
change the benchmark, SQL, scoring, parser semantics or index format.

## Changes

1. **IK range lookup** (`src/storage/fts/dict/ob_ft_range_dict.{h,cpp}`)
   `find_first_char_range` used to scan every dictionary range and call
   `ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, ...)` twice per range for every
   character. The main IK dictionary has 5-6 ranges, so a single CJK character
   paid 10-20 charset-validated string comparisons before the DAT lookup. The
   new code binary-searches the ordered ranges with the exact binary collation
   comparison (memcmp + length) and keeps a per-dictionary last-hit range cache.
   Other collations keep the original lookup.
2. **DAT character-code cache** (`ob_ft_dat_dict.{h,cpp}`)
   `ObFTDATReader::match_with_hit` performs an `ObArrayHashMap::find` for every
   character and every live hit, which hashes the 1-4 byte key and takes a 64-bit
   modulo per probe. Each reader now has a small direct-mapped cache of
   single-character -> trie code (positive and negative). The DAT block and
   `ObArrayHashMap` remain unchanged; a miss falls back to the original find.
3. **Case-normalization identity path** (`src/storage/fts/ob_fts_stop_word.cpp`)
   `ObAddWord::casedown_word` copied and decoded/re-encoded every token even when
   lowercasing was the identity (all CJK tokens). It now checks the exact
   `ObUnicaseInfo` table used by `ob_casedn_utf8mb4`; if no code point changes,
   the word keeps pointing into the document and no allocation/copy happens.
   Otherwise the original `ObCharset::tolower` path is used unchanged.
4. **IK arbitration array** (`src/storage/fts/ik/ob_ik_arbitrator.{h,cpp}`)
   The per-document chain map was replaced by an ordered array. Chains are
   produced in non-overlapping, strictly increasing offset order, so the output
   walk is a single pointer; the old hash-map create/insert/lookup per document
   is gone.
5. **Sort string fast path** (`src/sql/engine/sort/ob_sort_op_impl.{h,cpp}`)
   FTS index build sorts long runs of repeated word keys. The comparator now
   receives a 64-bit mask of non-LOB string keys from the sort spec. When both
   datums are non-null, same length and byte-identical, it returns equality
   without the collation walk. All other keys, NULLs, LOB keys, directions and
   tie order use the existing comparator.

## Correctness

- Native `fts_hot_path_tests`: **16/16** pass, including the new
  `casedown_identity_fast_path_matches_charset_tolower`, the IK arbitration
  cases, and the string-sort comparator cases.
- `run_fts_differential.py` against the pre-change snapshot: **PASS 1118**
  observations with SHA-256
  `80fbfc3134be918fd9a2e631835f72e4d2473b165e0aa6c7b6c6f3380d738aee`
  (bit-identical to the baseline).
- `run_fts_relevance_integration.py` against the pre-change snapshot:
  **PASS 68** results, plan checks true. The two correlated queries keep the
  baseline error 1210.
- The four benchmark hit counts remain 8001 / 11000 / 7332 / 20.
- The five benchmark/scoring/baseline files are byte-identical to the protected
  hashes (see `integrity.json`).

## Performance

Measurements were taken on the same WSL instance and server parameters
(`memory_budget=2G`, `cpu_count=2`, `datafile_size=2G`, `log_disk_size=2G`,
port 2885). The host was shared with other long-running seekdb and application
processes, so run-to-run variance is large. The cleanest controlled experiment
is a fresh-instance build A-B-B-A: each run starts from the same clean instance
template, loads the same 20000 documents, and times the three ALTER statements.
All values are seconds.

| Metric | base mean (A+A') | candidate mean (B+B') | reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 9.030 | 8.120 | 10.1% |
| `build_ik_content_sec` | 6.474 | 5.778 | 10.8% |
| `build_beng_en_sec` | 3.407 | 3.200 | 6.1% |
| build category | - | - | **9.0%** |

Tokenize and MATCH medians from six alternating skip-load runs on the same
already-built instance were inside the noise band (IK `tokenize_ik_avg_ms`
0.31 vs 0.29 ms, BENG 0.21 vs 0.23 ms; the client loop `select1_avg_ms` was
0.16 vs 0.17 ms). The changes remove per-character comparisons and per-token
allocations, but the local metric is dominated by client/statement overhead.
On the official environment the same work is a larger fraction of the measured
0.07 ms, but that is not claimed here. Query code is unchanged in this round.

A later full A-B-B-A and a second fresh-instance build A-B-B-A were dominated
by host load (for example an unchanged baseline BENG build moved between 3.2 s
and 7.4 s), so they are not used to claim a different number. All raw reports
are kept outside the repository.

## Experiments measured and reverted

- Removing the pushed-down `BOOL(MATCH)` filter for score-elided natural
  language COUNT queries made the three COUNT queries 1.5-1.7x slower on the
  same data (the scan without the pushed filter takes a slower per-row path),
  so the change was reverted.
- Pinning the IK dictionary bundle and reusing the parse helper across
  documents and tokenize calls did not produce a measurable gain in the
  controlled comparisons, while adding lifetime/thread-local complexity; it was
  reverted from the final tree.

## Reproduction

```bash
export MYSQL='mysql -h127.0.0.1 -P2885 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2885 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-round4.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"

# correctness
python unittest/storage/fts/run_fts_differential.py --port 2885 \
  --snapshot /tmp/diff-after.json --compare /path/to/differential-before.json
python unittest/storage/fts/run_fts_relevance_integration.py --port 2885 \
  --snapshot /tmp/rel-after.json --compare /path/to/relevance-before.json \
  --require-fastpath --rowset-size 0
```

The score script uses the historical local baseline and reports 100/100 for
both binaries on this machine; it is not evidence of the official score.
