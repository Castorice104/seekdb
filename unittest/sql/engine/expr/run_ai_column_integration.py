#!/usr/bin/env python3
"""AI column transactional acceptance against a real seekdb and local HTTP mock."""
import argparse
import importlib.util
import json
import time
import unittest
from pathlib import Path

import pymysql

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location(
    "column_mock", ROOT / "tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py")
mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock)


class AIColumnIntegration(unittest.TestCase):
    port = 2885
    mock_port = 19126

    @classmethod
    def setUpClass(cls):
        cls.mock_process = mock.start_server(cls.mock_port)
        cls.db = pymysql.connect(host="127.0.0.1", port=cls.port, user="root",
                                 autocommit=True, charset="utf8mb4", read_timeout=30)
        cls.sql("CREATE DATABASE IF NOT EXISTS ai_column_acceptance")
        cls.sql("USE ai_column_acceptance")
        for routine, name in (("DROP_AI_MODEL_ENDPOINT", "acceptance_ep"),
                              ("DROP_AI_MODEL", "acceptance_model")):
            try:
                cls.sql("CALL DBMS_AI_SERVICE." + routine + "(%s)", (name,))
            except pymysql.MySQLError as error:
                if error.args[0] not in (11112, 11118):
                    raise
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                ("acceptance_model", json.dumps({"type": "completion", "model_name": "acceptance-text"})))
        cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                ("acceptance_ep", json.dumps({"ai_model_name": "acceptance_model", "provider": "openai",
                 "request_model_name": "acceptance-text", "access_key": "local-fixture",
                 "url": "http://127.0.0.1:%d/v1/chat/completions" % cls.mock_port})))

    @classmethod
    def sql(cls, text, args=None):
        with cls.db.cursor() as cursor:
            cursor.execute(text, args)
            return cursor.fetchall()

    def setUp(self):
        self.sql("ROLLBACK")
        self.sql("DROP TABLE IF EXISTS docs")
        self.sql("CREATE TABLE docs(id BIGINT PRIMARY KEY, content VARCHAR(200), title VARCHAR(100),"
                 " result VARCHAR(400) AI COLUMN (AI_COMPLETE('acceptance_model',content))"
                 " WITH(sync_mode='async',max_retries=0))")
        Path(mock.state_path(self.mock_port, "events.jsonl")).write_text("")

    def tearDown(self):
        self.sql("ROLLBACK")
        self.sql("DROP TABLE IF EXISTS docs")

    @classmethod
    def tearDownClass(cls):
        try:
            cls.sql("DROP DATABASE ai_column_acceptance")
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('acceptance_ep')")
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('acceptance_model')")
        finally:
            cls.db.close()
            mock.stop_server(cls.mock_port)
            cls.mock_process.wait(timeout=5)

    def wait_value(self, ident, expected, timeout=20):
        deadline = time.monotonic() + timeout
        actual = None
        while time.monotonic() < deadline:
            rows = self.sql("SELECT result FROM docs WHERE id=%s", (ident,))
            actual = rows[0][0] if rows else None
            if actual == expected:
                return
            time.sleep(.1)
        self.assertEqual(actual, expected)

    def test_no_request_before_commit_and_rollback(self):
        self.sql("START TRANSACTION")
        self.sql("INSERT INTO docs(id,content) VALUES(1,'never-committed')")
        time.sleep(1.5)
        self.assertEqual(mock.read_events(self.mock_port), [])
        self.assertEqual(self.sql("SELECT result FROM docs"), ((None,),))
        self.sql("ROLLBACK")
        time.sleep(.5)
        self.assertEqual(mock.read_events(self.mock_port), [])
        self.assertEqual(self.sql("SELECT COUNT(*) FROM docs"), ((0,),))

    def test_autonomous_after_commit_without_next_sql(self):
        self.sql("INSERT INTO docs(id,content) VALUES(1,'autonomous')")
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not mock.read_events(self.mock_port):
            time.sleep(.1)
        self.assertTrue(mock.read_events(self.mock_port), "worker needs to run without another SQL request")
        self.wait_value(1, "mock:autonomous")

    def test_savepoint_restores_result_and_tasks(self):
        self.sql("INSERT INTO docs(id,content) VALUES(1,'original')")
        self.wait_value(1, "mock:original")
        Path(mock.state_path(self.mock_port, "events.jsonl")).write_text("")
        self.sql("START TRANSACTION")
        self.sql("SAVEPOINT before_update")
        self.sql("UPDATE docs SET content='savepoint-rollback' WHERE id=1")
        self.assertEqual(self.sql("SELECT result FROM docs"), ((None,),))
        self.sql("ROLLBACK TO SAVEPOINT before_update")
        self.sql("COMMIT")
        self.assertEqual(self.sql("SELECT result FROM docs"), (("mock:original",),))
        time.sleep(1)
        self.assertEqual(mock.read_events(self.mock_port), [])

    def test_unrelated_and_same_value_update_do_not_call(self):
        self.sql("INSERT INTO docs(id,content) VALUES(1,'stable')")
        self.wait_value(1, "mock:stable")
        Path(mock.state_path(self.mock_port, "events.jsonl")).write_text("")
        self.sql("UPDATE docs SET title='new-title' WHERE id=1")
        self.sql("UPDATE docs SET content=content WHERE id=1")
        time.sleep(1)
        self.assertEqual(self.sql("SELECT result FROM docs"), (("mock:stable",),))
        self.assertEqual(mock.read_events(self.mock_port), [])

    def test_rename_table_while_request_is_running(self):
        self.sql("INSERT INTO docs(id,content) VALUES(1,'slow-old-rename')")
        time.sleep(.2)
        self.sql("RENAME TABLE docs TO renamed_docs")
        try:
            deadline = time.monotonic() + 10
            rows = ()
            while time.monotonic() < deadline:
                rows = self.sql("SELECT result FROM renamed_docs WHERE id=1")
                if rows == (("mock:slow-old-rename",),):
                    break
                time.sleep(.1)
            self.assertEqual(rows, (("mock:slow-old-rename",),))
        finally:
            self.sql("RENAME TABLE renamed_docs TO docs")

    def test_case_change_under_case_insensitive_collation_regenerates(self):
        self.sql("ALTER TABLE docs MODIFY content VARCHAR(200) COLLATE utf8mb4_general_ci")
        self.sql("INSERT INTO docs(id,content) VALUES(1,'hello')")
        self.wait_value(1, "mock:hello")
        self.sql("UPDATE docs SET content='HELLO' WHERE id=1")
        self.wait_value(1, "mock:HELLO", timeout=3)

    def test_retry_cannot_read_unauthorized_subquery(self):
        self.sql("INSERT INTO docs(id,content) VALUES(1,'privilege-check')")
        self.wait_value(1, "mock:privilege-check")
        self.sql("CREATE DATABASE IF NOT EXISTS ai_column_private")
        self.sql("CREATE TABLE IF NOT EXISTS ai_column_private.secret(value VARCHAR(50))")
        self.sql("INSERT INTO ai_column_private.secret VALUES('private')")
        self.sql("DROP USER IF EXISTS 'ai_retry_low'@'%'")
        self.sql("CREATE USER 'ai_retry_low'@'%'")
        self.sql("GRANT SELECT, ALTER ON ai_column_acceptance.* TO 'ai_retry_low'@'%'")
        restricted = pymysql.connect(host="127.0.0.1", port=self.port, user="ai_retry_low",
                                     database="ai_column_acceptance", autocommit=True)
        try:
            with restricted.cursor() as cursor:
                with self.assertRaises(pymysql.MySQLError):
                    cursor.execute("ALTER TABLE docs RETRY AI COLUMN result WHERE EXISTS "
                                   "(SELECT 1 FROM ai_column_private.secret WHERE value='private')")
        finally:
            restricted.close()
            self.sql("DROP USER 'ai_retry_low'@'%'")
            self.sql("DROP DATABASE ai_column_private")

    def test_ai_definition_rejects_stored_routine_execution(self):
        self.sql("DROP FUNCTION IF EXISTS hidden_reader")
        self.sql("CREATE FUNCTION hidden_reader() RETURNS VARCHAR(20) RETURN 'private'")
        try:
            with self.assertRaises(pymysql.MySQLError):
                self.sql("CREATE TABLE unsafe_ai(id INT PRIMARY KEY, result VARCHAR(100) "
                         "AI COLUMN(AI_COMPLETE('acceptance_model', hidden_reader())))")
        finally:
            self.sql("DROP TABLE IF EXISTS unsafe_ai")
            self.sql("DROP FUNCTION hidden_reader")

    def test_explicit_result_assignment_rejected(self):
        with self.assertRaises(pymysql.MySQLError):
            self.sql("INSERT INTO docs(id,content,result) VALUES(1,'body','forged')")
        self.sql("INSERT INTO docs(id,content) VALUES(1,'body')")
        self.wait_value(1, "mock:body")
        with self.assertRaises(pymysql.MySQLError):
            self.sql("UPDATE docs SET result='forged' WHERE id=1")
        self.assertEqual(self.sql("SELECT result FROM docs"), (("mock:body",),))

    def test_batch_beyond_old_buffer_and_statement_rollback(self):
        values = [(n, "batch-%d" % n) for n in range(1, 49)]
        with self.db.cursor() as cursor:
            cursor.executemany("INSERT INTO docs(id,content) VALUES(%s,%s)", values)
        deadline = time.monotonic() + 30
        ready = 0
        while time.monotonic() < deadline:
            ready = self.sql("SELECT COUNT(*) FROM docs WHERE result=CONCAT('mock:',content)")[0][0]
            if ready == 48:
                break
            time.sleep(.1)
        self.assertEqual(ready, 48)
        Path(mock.state_path(self.mock_port, "events.jsonl")).write_text("")
        with self.assertRaises(pymysql.MySQLError):
            self.sql("INSERT INTO docs(id,content) VALUES(101,'statement-rollback'),(1,'duplicate')")
        self.assertEqual(self.sql("SELECT COUNT(*) FROM docs WHERE id=101"), ((0,),))
        time.sleep(.5)
        self.assertEqual(mock.read_events(self.mock_port), [])

    def test_primary_key_update_and_delete_pending(self):
        self.sql("INSERT INTO docs(id,content) VALUES(1,'initial-key')")
        self.wait_value(1, "mock:initial-key")
        self.sql("START TRANSACTION")
        self.sql("UPDATE docs SET id=2 WHERE id=1")
        self.assertEqual(self.sql("SELECT id,result FROM docs"), ((2, None),))
        self.sql("COMMIT")
        self.wait_value(2, "mock:initial-key")
        self.sql("START TRANSACTION")
        self.sql("UPDATE docs SET content='must-delete' WHERE id=2")
        self.sql("DELETE FROM docs WHERE id=2")
        self.sql("COMMIT")
        time.sleep(.5)
        self.assertEqual(self.sql("SELECT COUNT(*) FROM docs"), ((0,),))
        self.assertFalse(any(event['user'] == 'must-delete' for event in mock.read_events(self.mock_port)))

    def test_string_composite_primary_key_and_quoting(self):
        self.sql("DROP TABLE docs")
        self.sql("CREATE TABLE docs(k VARCHAR(100), n INT, content VARCHAR(200),"
                 " result VARCHAR(400) AI COLUMN(AI_COMPLETE('acceptance_model',content)), PRIMARY KEY(k,n))")
        self.sql("INSERT INTO docs(k,n,content) VALUES(%s,2,%s)", ("key'\\;雪", "quoted-body"))
        deadline = time.monotonic() + 15
        rows = ()
        while time.monotonic() < deadline:
            rows = self.sql("SELECT result FROM docs WHERE k=%s AND n=2", ("key'\\;雪",))
            if rows == (("mock:quoted-body",),):
                break
            time.sleep(.1)
        self.assertEqual(rows, (("mock:quoted-body",),))
        self.sql("DELETE FROM docs WHERE k=%s AND n=2", ("key'\\;雪",))
        self.assertEqual(self.sql("SELECT COUNT(*) FROM docs"), ((0,),))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=2885)
    args, remaining = parser.parse_known_args()
    AIColumnIntegration.port = args.port
    unittest.main(argv=[__file__] + remaining, verbosity=2)
