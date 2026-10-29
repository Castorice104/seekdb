# Predicate-only fulltext relevance optimization (2026-10-29)

The first official evaluation of `8b0ebc3452505ce9713a0958f72ab70772495b58` was **3.831/50**, with mean time reduction 3.831% and raw performance score 7.662/100. That result supersedes any prediction from the repository's historical baseline. This follow-up is measured against that submitted version; it does not promise a new official score or full marks.

## Bottleneck and change

`EXPLAIN EXTENDED` showed numeric relevance enabled even for `WHERE MATCH(...)` with `COUNT(*)` or an ID-only projection. The active composition retrieval engine initialized document statistics, per-token frequency aggregation and BM25 calculation although SQL consumed only truth values. The optimizer now proves score usage from separate WHERE and non-WHERE expression scopes before omitting those operations. Index postings, tokenization, filtering, document-ID enumeration and table lookup are still performed.

Only ordinary natural-language text-retrieval scans qualify. Numeric predicates/arithmetic, score projections, ordering/grouping/having, boolean search, custom parameters/boosts, child statements, vector-adaptive paths, functional lookups and index merge retain their score contract. Unknown or excessively complex expression trees fall back to scoring. The derived-LIMIT benchmark therefore stays on the original path.

The dormant hit-only runtime was incomplete. Initial regression runs exposed a null aggregate-definition crash and, after the short-query fix, a null aggregate-range crash for 400-term TaaT. These were fixed before committing: aggregate/statistics setup is gated, hit validity vectors remain allocated, DaaT emits a positive hit, and TaaT stores/deduplicates IDs and reuses posting ranges without requiring absent aggregates. Residual MATCH filters keep their established nonpushdown location. The independent review traced initialization, scalar/batch retrieval, projection, rescan and cleanup; see `final-review.md`.

## Controlled performance comparison

Fresh dedicated instances, identical configuration: WSL Ubuntu-22.04, `RelWithDebInfo`, 2 CPUs, 2 GiB memory budget, port 2885. Each run used the unchanged full official benchmark: ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30, SKIP_LOAD=0. Run order: submitted A, candidate B, candidate B, submitted A. Each version's two runs are averaged per metric below; the category formula includes negative improvements.

| Metric | Submitted 8b mean | Candidate mean | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 17.58450 | 18.36800 | -4.456% |
| `build_ik_content_sec` | 12.97900 | 12.69800 | +2.165% |
| `build_beng_en_sec` | 6.06550 | 6.54700 | -7.938% |
| `tokenize_ik_avg_ms` | 0.47380 | 0.46810 | +1.203% |
| `tokenize_beng_avg_ms` | 0.36595 | 0.37025 | -1.175% |
| `query_cn_avg_ms` | 3.77425 | 3.34660 | +11.331% |
| `query_beng_avg_ms` | 5.42065 | 4.86120 | +10.321% |
| `query_mixed_avg_ms` | 4.11030 | 3.70690 | +9.814% |
| `query_limit_avg_ms` | 4.95835 | 4.93080 | +0.556% |

Build category: **-3.410%**; tokenization: **+0.014%**; query: **+8.005%**. Equal-category aggregate reduction: **+1.537%** versus the previously submitted binary. This is a modest local incremental result, not a contest score. The measured build regression is retained even though this follow-up changes the query path. Query hits remain 8001 / 11000 / 7332 / 20 in every full run.

Earlier runs on the reused acceptance instance showed broad timing drift, including unchanged paths. They were not used for the comparison above; their raw records remain under `/home/wyh20/fts-relevance-evidence/`. The controlled reports are committed here. Neither this repeat nor a historical-baseline score establishes performance in the official environment.

Reports have git_dirty=1 because measurement preceded the final commit. Their git_head field names the checked-out base for both binaries; `integrity.json` records the two binary hashes and the exact candidate source hashes, avoiding reliance on that field for binary identity.

## Correctness and reproduction

- Native `fts_hot_path_tests`: **7/7** (existing tokenizer/word-map tests plus score-usage AST checks).
- **68** full SQL observations equal the submitted binary, separately with scalar `ROWSETS_MAX_ROWS=0` and batch size 64; EXPLAIN verifies the selected mode and score-elision boundaries. Includes scores rounded to 12 places, numeric thresholds/parameters, order/group/having, boolean mode, joins, residual MATCH reuse, rollback, empty/NULL text, and 400-term TaaT. Two correlated-query cases preserve baseline error 1210; this does not claim support for that query shape.
- Original **1,118** differential tokenizer/query/transaction/concurrency observations still pass.
- All four original mysqltests pass: `tokenize_function`, `cn_word`, `index_view`, `simple_query`. The only existing expected-output changes are four `calc_relevance=true` to `false` EXPLAIN fields, independently checked against unchanged query results. Their existing trailing spaces are retained to match exact server output.
- The official benchmark generator, SQL, timing harness, scoring script and fixed baseline are byte-identical to the pre-FTS base; no benchmark/workflow tuning was used. See `integrity.json`.

On the dedicated test instance (the integration runner recreates only `fts_relevance_regression`), with PyMySQL available:

```bash
python unittest/storage/fts/run_fts_relevance_integration.py --port 2885 --snapshot /tmp/relevance-before.json
# Switch to candidate server, preserving test settings.
python unittest/storage/fts/run_fts_relevance_integration.py --port 2885 --snapshot /tmp/relevance-after.json --compare /tmp/relevance-before.json --require-fastpath --rowset-size 0
python unittest/storage/fts/run_fts_relevance_integration.py --port 2885 --snapshot /tmp/relevance-batch.json --compare /tmp/relevance-before.json --require-fastpath --rowset-size 64
```

The full benchmark commands and scoring definitions remain in `tools/benchmark/`; retain a new output report per run. Build and tokenization optimization from the prior commit is inherited; this follow-up addresses only predicate-only retrieval.
