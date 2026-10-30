# FTS continuous optimization implementation plan

Approved specification: the user-approved continuous FTS optimization plan in this chat (2026-10-30).
Goal: improve the nine unchanged FTS benchmark metrics while preserving all observable tokenizer, query, transaction, index-format and error semantics.
Architecture: isolated experiments based on 9f5a030ec1c8aa548e298252f670f8343df70879; immutable A0 binary, bounded per-document scratch, existing parser/collation rules and active composition retrieval engine.
Execution: inline implementation with a final independent whole-branch review.

## Global Constraints
- No changes to the five official benchmark/generator/SQL/scorer/baseline files or evaluator/CI timing and resources.
- RelWithDebInfo; cpu_count=2, memory_budget=2G; identical instance, storage and client settings.
- Full parameters 20000/500/3000/200/3/30, SKIP_LOAD=0; new report per complete run.
- Two A/A blocks; candidate screening ABBA; freeze candidate then six independent ABBA confirmation blocks without optional stopping; final independent six-block A0 comparison.
- Fixed baseline denominators and weights build 1/9 each, tokenize 1/6 each, query 1/12 each. Preserve negative improvements.
- Before official baseline is verified, admit only reliable composite improvement without statistically significant regression in any of the nine metrics; retain tradeoff experiments separately.
- No public SQL, JSON, token order, word-frequency/representative, score/tie, index format or serialization change. No benchmark-specific or result caching.
- Restore the original branch untouched; preserve the pre-existing untracked historical plan.

## Review Focus
- Borrowed normalized token buffers must survive every consumer and allocator reuse.
- JSON token array ordering depends on the original hash geometry.
- Multibyte/invalid input and collation equality must retain exact behavior.
- DDL cancellation/retry and snapshot/dictionary changes must not share stale data.
- Scalar/vectorized query, residual filters, numeric scores and implicit LIMIT order must agree exactly.

### Task 1: Establish immutable baseline and experiment protocol
- [ ] Preserve binary/source/config/protected-file hashes and independent build cache.
- [ ] Run current native tests, capture fresh differential and relevance snapshots, including scalar and batch 64.
- [ ] Run two A/A blocks and separately profile each construction/tokenization/query phase with valid samples.
- [ ] Keep raw reports, per-run identity, environment measurements and failures; do not exclude candidate regressions.

### Task 2: BENG token hot path experiment
- [ ] Add complete character/normalization/lifetime coverage and a resource test that fails before optimization.
- [ ] Test charset-equivalent ASCII scanning/normalization and document-owned stable block storage.
- [ ] Run native, differential, relevance and full ABBA screening; retain only if correctness and performance gates pass.

### Task 3: IK and TOKENIZE invocation experiments
- [ ] Test replacing four fixed linked processors by fixed-order dispatch.
- [ ] Profile remaining character/dictionary/arbitration work; only target demonstrated costs.
- [ ] Test execution-owned options/map/JSON scratch reuse, preserving original map geometry and invalidation.
- [ ] Independently validate each experiment and withdraw unconfirmed complexity.

### Task 4: FTS build row expansion experiment
- [ ] Add row-content/lifetime/empty-document tests before implementation.
- [ ] Test row/datum block allocation, bounded map reuse and precomputed output-expression mapping.
- [ ] Test actual FTS batch row generation if profile supports it; preserve all row and doc-ID contracts.
- [ ] Validate FTS suite, DML/rollback/restart and performance before acceptance.

### Task 5: Sort, spill and DDL pipeline experiments
- [ ] Profile sort vs repeated tokenization vs scheduler waits separately.
- [ ] Where material, test collation-correct cached comparison keys/copy reduction, bounded workarea/spills.
- [ ] Only if justified, test same-DDL snapshot/version-bound shared token streams or avoidable wakeup waits; retain persistence/check/publication barriers.
- [ ] Cover shared sort null/LOB/padding/direction/collation/spill and DDL failure/cancel/retry before acceptance.

### Task 6: Active retrieval and exact LIMIT experiments
- [ ] Follow active composition -> DaaT/TaaT, accounting for existing batch buffers and small mergers.
- [ ] Test one-term bypass, batch merge/projection and proven covering-column/lookup elimination.
- [ ] Test exact bounded Top-K while preserving score and tie order; no unsafe early stopping or score elision.
- [ ] Validate scalar/batch, 400 terms, filters, parameters, full IDs and unrounded binary-protocol scores.

### Task 7: Confirmation, convergence and delivery
- [ ] Freeze each viable candidate and run independent six-block ABBA confirmation with fixed statistical analysis.
- [ ] Re-profile accepted candidates and account for remaining combinable opportunities; three distinct failed hypotheses alone do not prove convergence.
- [ ] Declare constrained convergence only when remaining aggregate potential is below 0.5 percentage points and measurement resolution; otherwise explicitly report measurement/compatibility limits.
- [ ] Clean build, all affected correctness suites and independent six-block final A0 comparison.
- [ ] Independent whole-branch review, protected hashes, experiment ledger and reproducible artifact delivery.
- [ ] After local convergence, evaluate the final commit officially; reopen conflicting evidence and resolve tradeoff candidates using the actual official baseline/results.
