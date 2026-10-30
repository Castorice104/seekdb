# Continuous FTS optimization candidate (2026-10-30)

Base: `9f5a030ec1c8aa548e298252f670f8343df70879` on `feat/fts-performance`.
Candidate branch: `feat/fts-continuous`.

## Candidate changes

1. `perf(fts): retain borrowed word data across cached row consumption`
   The IK identity-case fast path can leave word datums pointing into the
   table-scan LOB buffer, which is released before cached index rows are
   consumed. The first borrowed word per document now copies the fulltext once
   into the row-cache arena and rebases later words by offset. This is the
   lifetime fix required by the previous round's tokenizer optimization.
2. `perf(fts): cache contiguous sort key arrays`
   `ObSortOpImpl::Compare` called `ObIArray::at()` for the collation and
   comparator arrays twice per key per comparison. The contiguous data pointers
   are cached at init. This is the top user-space symbol in the BENG build
   profile (`ObSortOpImpl::Compare`, 6.6% of samples).
3. `perf(fts): bypass repeated string comparator validation`
   `ObDatumStrCmpImpl` called `ObCharset::strcmpsp`, which re-validated the
   datum arguments and resolved the collation handler on every comparison.
   The handler is resolved once and the same `strnncollsp` entry is called;
   suspicious inputs keep the validating path.
4. `perf(fts): evaluate certified relevance truth filters directly`
   When the pushed-down filter is exactly `BOOL(<double relevance projection>)`,
   the iterator derives the truth value from the already-published relevance
   instead of re-evaluating the expression. Numeric thresholds, casts, residual
   filters, LOB/non-double shapes and shared BOOL consumers keep the evaluator.

The five benchmark/generator/setup/scorer/baseline files are byte-identical to
the protected hashes.

## Correctness

- Native `fts_hot_path_tests`: **23/23**.
- Differential: **1118** observations, bit-identical to `A0`.
- Relevance: **68** observations, scalar and batch 64, identical to `A0`.
- Exact binary-protocol scores: **101** shapes/observations identical to a
  fresh `A0` baseline generated with the same script (the older `A0` snapshot
  predates the bool/IF shapes; comparing against it is invalid).
- `fts_index` mysqltest suite: 16/17 pass. The single failure is the known
  `calc_relevance=true -> false` EXPLAIN difference from the earlier
  score-elision commit; the reject diff is identical to `A0`.

## Counterbalanced performance confirmation

ABBA blocks suffer a strong position bias in this shared environment (the
middle B runs are faster than the first/last A runs even for a query-only
change). The candidate was therefore confirmed with a counterbalanced design:
three `A=B0, B=candidate` blocks and three `A=candidate, B=B0` blocks, and the
per-block fixed-baseline deltas were combined with the correct sign. This
cancels the position effect. Values are fixed-baseline improvement deltas
(positive = candidate faster); CI is a paired-block Student t interval over six
blocks.

| Scope | Delta | 95% CI |
| --- | ---: | ---: |
| build (3 metrics) | +0.2751% | [+0.1070%, +0.4432%] |
| query (4 metrics) | +0.3197% | [+0.0573%, +0.5821%] |
| build + query | +0.5948% | [+0.1993%, +0.9903%] |
| tokenize (2 metrics) | +0.8464% | [-0.6838%, +2.3766%] (noise) |
| all nine metrics | +1.4412% | [-0.3738%, +3.2562%] |

No significant regression after Holm correction. The all-nine CI includes zero
because the tokenize category, which the changes do not target, has a wide
positive point estimate; the targeted build+query CI is strictly positive.
Per-metric significant improvements: `build_ik_all_sec` and
`build_ik_content_sec`; `build_beng_en_sec` is positive but its CI includes
zero. `query_cn_avg_ms` and `query_beng_avg_ms` CIs are strictly positive;
`query_mixed_avg_ms` and `query_limit_avg_ms` are positive but include zero.

This candidate is a **tradeoff admission**, not a claim that the equal-weight
nine-metric gate is met. The strict all-nine CI lower bound is negative; more
blocks or another build improvement would be required for strict admission.
The prior official 20.253 result remains on `feat/fts-performance`; this branch
is separate until the user chooses to submit it.

## Reverted experiments

- Caller-owned BENG token blocks: no reliable gain; reverted.
- UTF8/ASCII token-scan fast path: full benchmark regressed; reverted.
- The earlier six-block ABBA confirmation of the direct relevance filter was
  order-biased and its overall CI included zero; it was re-admitted only after
  the counterbalanced exact-score validation above.
- Thread-global parser/dictionary bundle and deleting the pushed `BOOL(MATCH)`
  filter remain rejected.

## Remaining work

- Run a final counterbalanced `A0` vs candidate comparison (the current
  comparison is against `B0`, which includes the required lifetime fix).
- Six-block counterbalanced confirmation is already stronger than six ABBA
  blocks for this environment; more blocks would tighten the all-nine CI.
- IK processor dispatch, build row/datum block reuse, and exact Top-K query
  remain unmeasured candidates; their expected aggregate potential is below
  the current noise floor unless a new profile proves otherwise.
- The official baseline is not the checked-in historical JSON; the official
  score is relative to a much faster reference. Do not use the checked-in
  score script output as an official prediction.

## Reproduction

```bash
# candidate binary: /home/wyh20/fts-continuous-evidence/seekdb.S3
# baseline:        /home/wyh20/fts-continuous-evidence/seekdb.B0
cd /home/wyh20/fts-continuous-evidence
FTS_SETTLE_SEC=10 /home/wyh20/oceanbase/powercontext/competition/.venv/bin/python \
  run_experiment.py --a seekdb.B0 --b seekdb.S3 --prefix S3-odd --blocks 3
FTS_SETTLE_SEC=10 /home/wyh20/oceanbase/powercontext/competition/.venv/bin/python \
  run_experiment.py --a seekdb.S3 --b seekdb.B0 --prefix S3-even --blocks 3
/home/wyh20/oceanbase/powercontext/competition/.venv/bin/python \
  /mnt/d/OceanBase/work/combine_generic.py S3 3
```
