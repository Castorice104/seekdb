# AI Function Enhancement Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans or superpowers:subagent-driven-development to implement these tasks; keep file ownership disjoint.

**Goal:** Implement all six competition AI SQL enhancements with real engine behavior and deterministic local mock verification.
**Architecture:** Preserve the existing expression / provider / HTTP layers. Add batch evaluation at the SQL expression layer and validate provider outputs before publishing any row results.
**Tech Stack:** C++17, seekdb SQL engine, libcurl, native JSON, CMake, gtest, mysqltest, Python mock HTTP service.
**Spec:** User-provided 2026 competition six-subtopic requirements; public regression tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test.

## Global Constraints
- No model-name, input, or response-specific implementation shortcuts.
- Preserve ordinary SQL and existing AI functions. No real model calls.
- Errors must not crash, leak, or misalign results.
- Submit the private origin repository branch and commit, not the prior public Fork PR.

## Review Focus
- Malformed or incomplete JSON responses must fail before partial results escape.
- LOB values must use real payload rather than locator bytes.
- Batch NULL/skip/evaluated rows must keep original row mapping.
- Duplicate/out-of-range response indices and nonfinite vector values must fail.
- Prompt replacements must not reinterpret placeholder-like text inserted by an argument.

## Task 1: Prompt and completion expressions
Files: src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.{cpp,h}, ob_expr_ai_complete.{cpp,h}; unittest/sql/engine/expr/ob_expr_ai_prompt_test.cpp.
- [x] Add regressions for JSON arrays/objects, repeated and illegal placeholders, nesting, long text, Unicode and escaping.
- [x] Implement type support and safe LOB extraction; accept NULL optional completion config and validate config objects.
- [x] Coordinate shared prompt utility changes with Task 2 owner; do not edit that owner's files.
- [x] Run focused tests and review invalid-input semantics.

## Task 2: Provider protocols and response validation
Files: src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.{cpp,h}, ob_ai_func_client.{cpp,h}, src/share/ai_service/ob_ai_service_struct.cpp, src/observer/ai_service/ob_ai_service_executor.cpp; unittest/sql/engine/expr/test_ob_openai_utils.cpp.
Interfaces: retain ObAIFuncModel::call_dense_embedding_vector_v2(contents, config, results), with one ordered JSON vector per input. Config dimensions optional, positive integer when specified.
- [x] Add protocol/response regression tests before changing behavior.
- [x] Implement case-insensitive Ollama routing, optional key, native generate/embed and config mappings.
- [x] Validate nonempty completion, embedding numeric vectors/count/dimension/indices; restore response order.
- [x] Implement robust prompt formatting requested by Task 1.
- [x] Review HTTP resource cleanup, network and malformed JSON error handling.

## Task 3: Embedding batch expressions and rerank
Files: src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.{cpp,h}, ob_expr_ai_rerank.{cpp,h}; focused regression tests in unittest/sql/engine/expr.
- [x] Add tests for dimension/NULL semantics and batch row mapping; rerank objects/strings/index stability.
- [x] Implement real eval_batch_func_ using existing vector_v2 provider call, grouping compatible model/dimension values, honoring skip/evaluated rows and NULL propagation.
- [x] Validate rerank batches, global indices and stable scores without losing original object IDs.
- [x] Run focused tests and review errors without partial outputs.

## Task 4: Integration, build, verification and delivery
Files: unittest/sql/engine/expr/ai_function_mock.py and established regression test directories as needed.
- [x] Configure/build release and relevant unit tests using WSL installed dependencies.
- [x] Run local deterministic Mock HTTP service; run public enhancement SQL and targeted negative/batch cases against the rebuilt server.
- [x] Review integrated diff for regressions, run git diff --check and relevant tests.
- [x] Record actual commands and outcomes, commit and push private origin branch; report exact commit/branch and limitations.

## Verification record (2026-10-27)

- Release production build completed successfully.
- Focused CTest target passed all 19 gtests.
- Real mysqltest passed ai_function_enhancement, ai_prompt, and the new expected-output regression.
- Integration runner passed all 15 groups, including 1100 rows in five requests (219,219,220,219,65 non-NULL inputs), malformed responses and later-group failure recovery.
- Review found Dashscope text_index reordering and missing batch-failure execution coverage. Added two tests, observed both fail, fixed indexed decoding, then verified green; added real later-group failure/recovery test.
- Runtime uncovered nullable credential persistence after ALTER Ollama endpoint; normalized keyless writes and nullable reads, then verified the public suite and explicit storage assertion.
- Official competition scoring is external and has not been submitted or claimed here.

- Implementation commit `3ee4b31c5b4e1b6944f8d26f13a3f5f6cb0d134a` was pushed to private origin branch `feat/ai-function-enhancement`; remote SHA was verified. Subsequent documentation-only commits do not change the verified implementation.
