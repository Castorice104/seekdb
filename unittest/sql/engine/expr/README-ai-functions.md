# AI function regression tests

These tests exercise the SQL engine and provider adapters. The Python HTTP fixture
is deterministic and never calls a real model. Its default embedding dimension is
768 for the public competition suite; production code does not assume this value.

## Build and unit tests (Linux)

```sh
./build.sh release --init       # only if dependencies are not initialized
./build.sh release --make -j2
cmake --build build_release --target ai_function_tests -j2
(cd build_release && ctest -V -R '^ai_function_tests$')
```

The focused CMake target is excluded from the default production build and links
against the same implementation as seekdb. The 19 tests cover prompt type checking,
JSON/nested/repeated/invalid placeholders, provider selection and native Ollama
requests, optional credentials, embedding shape/element/index validation, and
OpenAI/Dashscope response reordering.

## SQL integration tests

Start the newly built binary in an isolated data directory (adjust the memory and
disk limits for your machine):

```sh
mkdir -p /tmp/seekdb-ai-regression
build_release/src/observer/seekdb --nodaemon \
  --base-dir /tmp/seekdb-ai-regression --port 2883 \
  --parameter memory_limit=2G --parameter datafile_size=2G \
  --parameter log_disk_size=2G --parameter cpu_count=2
```

Wait until the server accepts SQL connections. In another terminal, with PyMySQL
installed in your Python environment:

```sh
python3 unittest/sql/engine/expr/run_ai_function_integration.py \
  --port 2883 --report /tmp/ai-function-integration-results.json
```

The runner starts and stops its own mock on `0.0.0.0:8000`. Port 8000 must be free.
It creates a dedicated `ai_enhancement_test` database and test-only model/endpoint
names; use an isolated instance. Checks include the two public SQL suites, >64KB
LOB input, native JSON and NULL config, explicit/default dimensions, 1100 rows
with NULL/filter/LIMIT and varying dimensions, global stable rerank indices and
original IDs, keyless Ollama before/after endpoint alteration, malformed JSON,
invalid dimensions/indices/scores, HTTP errors, and failure of a later batch group.
After a failed group, a succeeding table scan must still map every result correctly.
The request log also verifies that real multi-input HTTP requests were sent.

This runner executes SQL, not a Python implementation of the database functions.
The mock is a local regression fixture, not the competition organizer's service.

## Native mysqltest

Alternatively start only the mock:

```sh
python3 unittest/sql/engine/expr/ai_function_mock.py --port 8000
```

Create an empty test database, then run each public suite with your `mysqltest`
client using `--host=127.0.0.1 --port=2883 --user=root --database=YOUR_TEST_DB` and
redirect the chosen `.test` file into stdin. The public suites are
`tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test`
and `ai_prompt.test`.

The added `ai_embed_rerank_regression.test` has deterministic expected output at
`tools/deploy/mysql_test/test_suite/ai_function/r/ai_embed_rerank_regression.result`;
pass that file through `--result-file`. It covers varying models and dimensions,
NULL propagation, LOBs, filtered LIMIT, stable ties and an uneven rerank batch.
Do not run the standalone mock and the integration runner simultaneously.
