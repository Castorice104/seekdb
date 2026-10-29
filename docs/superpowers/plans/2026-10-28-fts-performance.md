# Fulltext hot path implementation plan

Goal: reduce fulltext indexing and TOKENIZE CPU/allocation overhead while preserving every token, frequency, collation, parser option, MATCH result and transaction rule.

Approach: optimize per-document work rather than changing shared external sorting or storage algorithms. Inspection shows all word-map instances are local variables in TOKENIZE, generate_fulltext_word_rows, optimizer term extraction, and the current/legacy text retrieval iterators; global stopword dictionaries use a different, unchanged type. IK repeatedly decodes a character for each classification predicate and fetches already available character metadata in every processor. English normalization copies/allocates every token. CPU samples show charset/lowercase, allocator, dictionary and latch work; raw profile and symbolized samples are retained outside the repository.

Constraints: leave all six official benchmark files and timing/scoring logic unchanged. Keep the original a6ac70f0f binary. Run before/after at 20000/500/3000/200/3/30/SKIP_LOAD=0 on the same instance resources. CI score on this machine is informational, not proof of official performance.

- [x] Run unmodified official baseline; capture exact commit/binary/config/hit counts.
- [x] Add focused native semantic tests before edits; run on the baseline (3/3). Capture 1118 differential observations; provision local mysqltest admin account and connection variables; original FTS cases 4/4.
- [x] Decode IK characters once and pass existing char metadata to processors without changing processor order.
- [x] Remove synchronization from strictly local per-document word maps and aggregate with one hash lookup, preserving existing initial-frequency semantics.
- [x] Reuse English normalization scratch capacity, retaining charset callbacks and token lifetime.
- [x] Build production and focused tests; run original FTS regression suites plus mixed Unicode/options/long-document/concurrent differential coverage and transactional DML.
- [x] Run unchanged complete after benchmark; compare categories against local before and separately calculate official fixed-baseline score.
- [x] Independent review and integrity checks pass; validation report includes six raw reports.
Delivery commit, remote verification, and final branch/SHA are recorded in the task evidence and final response.
