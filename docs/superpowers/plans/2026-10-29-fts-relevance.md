# Fulltext predicate-only relevance optimization

The first official evaluation of 8b0ebc345 yielded 3.831/50 (mean reduction 3.831%). Local historical-CI-baseline scores cannot predict this result. Preserve that result; compare the new code against the submitted binary using the unchanged full benchmark and identical resources.

The optimizer currently forces relevance calculations on. Existing runtime/codegen support no-BM25 operation. Add a conservative AST usage check for plain SELECT/NATURAL_LANGUAGE MATCH without boosts or child statements: all non-WHERE roots must be free of the match reference; every WHERE use must be boolean. Numeric thresholds, projections, order/group/having, boolean search mode, custom boosts, subqueries and uncertain contexts keep scores. Traverse separate expression scopes to avoid root-deduplication hiding a projected match also used as a filter. The existing pushed-down boolean filter projects the hit value when the runtime omits the numeric score.

- [x] Add integration snapshot/plan regression; demonstrate the missing fast plan on the submitted binary.
- [x] Implement and unit-test conservative score-usage checking; wire it into text-retrieval planning.
- [x] Verify codegen/runtime without score, complete row/score result parity, projected scores, thresholds/parameters, ordering, grouped queries, subqueries, joins, transactions, boolean mode, parser options.
- [x] Run unchanged full benchmark before/after and original semantic regressions, accepting legitimate EXPLAIN-only expectation changes only when independently checked.
- [x] Independent review, integrity check, document actual improvements and limitations, commit and push.


Acceptance: native 7/7; 68 baseline-equivalent results in scalar and batch modes; 1,118 prior differential observations; original four FTS mysqltests pass. The only expected-plan changes are four score-elision flags. Full fresh-instance A-B-B-A results and immutable benchmark/source checksums are recorded in docs/performance/fts-relevance-2026-10-29/.

Review: one fresh reviewer found the initially incomplete no-score runtime. Ordinary COUNT and 400-term TaaT regression runs failed before the corresponding fixes and pass afterward. Final review reports no unresolved Blocker or Major finding. No deferred minor findings.

Ruling: expand the runtime fixes beyond the initial optimizer-only plan because dormant score-elision support was incomplete. Keeping only the optimizer change demonstrably crashed; the cost is additional retrieval-path maintenance, covered by scalar/batch, long-query and original regression suites.

Ruling: retain conservative fallback for child statements rather than enlarge eligibility after review. The cost is leaving derived-LIMIT performance largely unchanged; numeric score semantics stay protected.

Performance is modest: the controlled comparison includes build regressions and yields only +1.537% aggregate reduction versus 8b; it is not evidence of full marks or a prediction of the next official result.
