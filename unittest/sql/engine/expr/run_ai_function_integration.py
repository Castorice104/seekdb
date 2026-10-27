#!/usr/bin/env python3
"""SQL-engine integration regression against a real seekdb and local mock.

Requires PyMySQL. Run with --port pointing to the freshly built server.
Starts its own fixture on port 8000; refuses to replace an existing listener.
"""
import argparse
import json
import threading
from pathlib import Path
import pymysql
from ai_function_mock import make_server, embedding
from urllib.request import Request, urlopen


class Integration:
    def __init__(self, port, report):
        self.connection = pymysql.connect(host="127.0.0.1", port=port, user="root",
                                         autocommit=True, charset="utf8mb4", read_timeout=90)
        self.cursor = self.connection.cursor()
        self.report = Path(report)
        self.checks = []
        self.run("CREATE DATABASE IF NOT EXISTS ai_enhancement_test")
        self.run("USE ai_enhancement_test")

    def run(self, sql, args=None):
        self.cursor.execute(sql, args)
        return self.cursor.fetchall()

    def scalar(self, sql, args=None):
        return self.run(sql, args)[0][0]

    def check(self, name, action):
        try:
            action()
        except Exception as error:
            self.checks.append({"test": name, "passed": False, "error": str(error)})
            print("FAIL", name, str(error), flush=True)
        else:
            self.checks.append({"test": name, "passed": True})
            print("PASS", name, flush=True)
        self.report.write_text(json.dumps(self.checks, indent=2, ensure_ascii=False), encoding="utf-8")

    def eq(self, actual, expected):
        assert actual == expected, f"actual {str(actual)[:500]} != expected {str(expected)[:500]}"

    def error(self, sql, args=None):
        try:
            self.run(sql, args)
        except pymysql.MySQLError:
            self.eq(self.scalar("SELECT 1+1"), 2)
        else:
            raise AssertionError("expected SQL error")

    def events(self, clear=False):
        request = Request("http://127.0.0.1:8000/events", method="DELETE" if clear else "GET")
        with urlopen(request) as response:
            return json.load(response)

    def model(self, name, kind, path, provider="openai", key="mock-key"):
        for routine, value in (("DROP_AI_MODEL", name), ("DROP_AI_MODEL_ENDPOINT", name + "_ep")):
            try:
                self.run("CALL DBMS_AI_SERVICE." + routine + "(%s)", (value,))
            except pymysql.MySQLError as e:
                if e.args[0] not in (11118,11112):
                    raise
        self.run("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                 (name, json.dumps({"type": kind, "model_name": "fixture-" + kind})))
        self.run("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                 (name + "_ep", json.dumps({"ai_model_name": name, "scope": "all",
                  "url": "http://127.0.0.1:8000" + path, "access_key": key, "provider": provider})))

    def endpoint(self, name, **changes):
        self.run("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)",
                 (name + "_ep", json.dumps(changes)))

    def public_suite(self, file):
        # The checked-in suites use SQL plus connection/warning/error/query-log directives.
        sql = ""
        allowed = None
        quote = None
        escaped = False
        for line in Path(file).read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not sql.strip() and (not stripped or stripped.startswith("#") or stripped.startswith("--") or stripped.startswith("connection ")):
                if stripped.startswith("--error "):
                    allowed = {int(x) for x in stripped[len("--error "):].split(",")}
                continue
            for char in line + "\n":
                sql += char
                if escaped:
                    escaped = False
                elif quote:
                    if char == "\\":
                        escaped = True
                    elif char == quote:
                        quote = None
                elif char in ("'", '"', "`"):
                    quote = char
                elif char == ";":
                    try:
                        rows = self.run(sql)
                    except pymysql.MySQLError as error:
                        if allowed is None or error.args[0] not in allowed:
                            raise AssertionError(f"{file}: {sql}: {error}") from error
                    else:
                        if allowed is not None and 0 not in allowed:
                            raise AssertionError(f"expected error {allowed}: {sql}")
                        if rows and self.cursor.description:
                            names = [c[0] for c in self.cursor.description]
                            for name, value in zip(names, rows[0]):
                                if name.endswith("_ok"):
                                    self.eq(value, 1)
                                if name in ("embed_dimension", "requested_dimension"):
                                    self.eq(value, 768)
                    sql, allowed = "", None
        assert not sql.strip(), "unterminated SQL"

    def core(self):
        self.model("reg_complete", "completion", "/v1/chat/completions")
        self.model("reg_embed", "dense_embedding", "/v1/embeddings")
        self.model("reg_rerank", "rerank", "/v1/rerank", "siliconflow")
        self.model("reg_ollama", "completion", "/api/generate", "oLlAmA", "")
        self.model("reg_ollama_embed", "dense_embedding", "/api/embed", "OLLAMA", "")

    def prompt(self):
        value = json.loads(self.scalar("SELECT AI_PROMPT('{0}/{0}/{1}',JSON_ARRAY(1,true),JSON_OBJECT('k',2))"))
        self.eq(value, {"template": "{0}/{0}/{1}", "args": [[1,True], {"k":2}]})
        self.events(True)
        self.scalar("SELECT AI_COMPLETE('reg_complete',AI_PROMPT('{0}/{0}/{1}',%s,AI_PROMPT('nested {0}',%s)),NULL)", ("你好\n{1}", "world"))
        self.eq(self.events()[-1]["body"]["messages"][-1]["content"], "你好\n{1}/你好\n{1}/nested world")
        for token in ("{99}", "{999999999999999999999999999999999}"):
            self.error("SELECT AI_COMPLETE('reg_complete',AI_PROMPT(%s,'x'))", (token,))

    def completion(self):
        self.events(True)
        self.scalar("SELECT AI_COMPLETE('reg_complete','hello',JSON_OBJECT('temperature',0.2,'max_tokens',16))")
        body = self.events()[-1]["body"]
        self.eq(body["temperature"], 0.2)
        self.eq(body["max_tokens"], 16)
        self.scalar("SELECT AI_COMPLETE('reg_complete','hello',NULL)")
        self.error("SELECT AI_COMPLETE('reg_complete','hello','[]')")
        self.error("SELECT AI_COMPLETE('reg_complete','hello','invalid-json')")

    def lob(self):
        self.run("DROP TABLE IF EXISTS reg_lob")
        self.run("CREATE TABLE reg_lob(t LONGTEXT,a LONGTEXT)")
        value = "数据库\n\"\\" * 12000
        self.run("INSERT INTO reg_lob VALUES(%s,%s)", ("{0}/{0}",value))
        obj = json.loads(self.scalar("SELECT AI_PROMPT(t,a) FROM reg_lob"))
        self.eq(obj["args"], [value])
        self.events(True)
        self.scalar("SELECT AI_COMPLETE('reg_complete',AI_PROMPT(t,a)) FROM reg_lob")
        self.eq(self.events()[-1]["body"]["messages"][-1]["content"], value + "/" + value)

    def embed_scalar(self):
        self.eq(json.loads(self.scalar("SELECT AI_EMBED('reg_embed','scalar')")), embedding("scalar",768))
        self.eq(json.loads(self.scalar("SELECT AI_EMBED('reg_embed','scalar',7)")), embedding("scalar",7))
        self.eq(self.scalar("SELECT AI_EMBED('reg_embed',NULL)"), None)
        for dimension in ("0", "-1", "1.2", "NULL"):
            self.error("SELECT AI_EMBED('reg_embed','scalar'," + dimension + ")")

    def batch(self):
        self.run("DROP TABLE IF EXISTS reg_batch")
        self.run("CREATE TABLE reg_batch(id INT PRIMARY KEY,content TEXT,dim INT)")
        values = [(i,None if i%7 == 0 else 'row-'+str(i),3+i%3) for i in range(1100)]
        self.cursor.executemany("INSERT INTO reg_batch VALUES(%s,%s,%s)",values)
        self.events(True)
        rows = self.run("SELECT id,AI_EMBED('reg_embed',content,4) FROM reg_batch ORDER BY id")
        self.eq(len(rows),len(values))
        for (i, vector), (_, content, _) in zip(rows,values):
            self.eq(json.loads(vector) if vector is not None else None, embedding(content,4) if content is not None else None)
        events = [e for e in self.events() if e["path"].endswith("embeddings")]
        sizes = [len(e["body"]["input"]) for e in events]
        assert sizes and max(sizes)>1, f"no actual batched requests: {sizes[:20]}"
        assert len(events)>1, "multiple batches were not exercised"
        self.eq(sum(sizes),sum(c is not None for _,c,_ in values))
        self.events(True)
        rows = self.run("SELECT id,AI_EMBED('reg_embed',content,dim) FROM reg_batch WHERE id>=101 AND id<151 ORDER BY id LIMIT 17")
        self.eq([r[0] for r in rows],list(range(101,118)))
        for i, vector in rows:
            content=values[i][1]
            self.eq(json.loads(vector) if vector is not None else None,embedding(content,3+i%3) if content is not None else None)
        print("batch request sizes:",sizes,flush=True)

    def batch_failure(self):
        self.events(True)
        self.endpoint("reg_embed",url="http://127.0.0.1:8000/fault/late-error/v1/embeddings")
        try:
            self.error("SELECT id,AI_EMBED('reg_embed',content,dim) FROM reg_batch ORDER BY id")
            events = self.events()
            assert any(e["status"] == 200 for e in events), "expected an earlier successful group"
            failed = [e for e in events if e["status"] == 400]
            assert failed and len(failed[-1]["body"]["input"]) > 1, "failure did not exercise batch evaluation"
        finally:
            self.endpoint("reg_embed",url="http://127.0.0.1:8000/v1/embeddings")
        rows = self.run("SELECT id,AI_EMBED('reg_embed',content,dim) FROM reg_batch WHERE id<20 ORDER BY id")
        self.eq([r[0] for r in rows],list(range(20)))
        for i,vector in rows:
            self.eq(json.loads(vector) if vector is not None else None,
                    embedding('row-'+str(i),3+i%3) if i%7 else None)

    def ollama(self):
        self.endpoint("reg_ollama",provider="Ollama")
        self.eq(self.scalar("SELECT access_key IS NULL FROM oceanbase.__all_ai_model_endpoint WHERE endpoint_name='reg_ollama_ep'"),0)
        self.events(True)
        self.scalar("SELECT AI_COMPLETE('reg_ollama','hello','{\"temperature\":0.2,\"max_tokens\":16}')")
        event=self.events()[-1]
        self.eq(event["body"]["stream"],False)
        self.eq(event["body"]["options"]["num_predict"],16)
        self.eq(event["authorization"],None)
        self.eq(json.loads(self.scalar("SELECT AI_EMBED('reg_ollama_embed','hello',5)")),embedding("hello",5))

    def rerank(self):
        docs=[{"id":"doc-"+str(i),"text":"apple "*(i%3)} for i in range(45)]
        result=json.loads(self.scalar("SELECT AI_RERANK('reg_rerank','apple',%s,'text')",(json.dumps(docs),)))
        expected=sorted(range(len(docs)),key=lambda i:-(i%3))
        self.eq([r["id"] for r in result],[docs[i]["id"] for i in expected])
        strings=[d["text"] for d in docs]
        result=json.loads(self.scalar("SELECT AI_RERANK('reg_rerank','apple',%s,NULL)",(json.dumps(strings),)))
        self.eq([r["index"] for r in result],expected)

    def faults(self,model,path,sql,modes):
        try:
            for mode in modes:
                self.endpoint(model,url="http://127.0.0.1:8000/fault/"+mode+path)
                self.error(sql)
        finally:
            self.endpoint(model,url="http://127.0.0.1:8000"+path)

    def cleanup(self):
        for name in ("reg_complete","reg_embed","reg_rerank","reg_ollama","reg_ollama_embed"):
            self.run("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)",(name,))
            self.run("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)",(name+"_ep",))
        for name in ("reg_lob","reg_batch"):
            self.run("DROP TABLE IF EXISTS "+name)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",type=int,default=2883)
    parser.add_argument("--report",default="ai-function-integration-results.json")
    parser.add_argument("--baseline",action="store_true")
    args=parser.parse_args()
    server=make_server()
    thread=threading.Thread(target=server.serve_forever,daemon=True)
    thread.start()
    suite=Integration(args.port,args.report)
    root=Path(__file__).resolve().parents[4]
    try:
        print("server:",suite.scalar("SELECT VERSION()"),flush=True)
        suite.check("public enhancement suite",lambda:suite.public_suite(root/"tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test"))
        suite.check("public prompt suite",lambda:suite.public_suite(root/"tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test"))
        suite.check("model endpoint setup",suite.core)
        if not args.baseline:
            for name in ("prompt","completion","lob","embed_scalar","batch","batch_failure","ollama","rerank"):
                suite.check(name,getattr(suite,name))
            suite.check("completion malformed response",lambda:suite.faults("reg_complete","/v1/chat/completions","SELECT AI_COMPLETE('reg_complete','x')",("empty","null-content","bad-json","non-object","http-error")))
            suite.check("embedding malformed response",lambda:suite.faults("reg_embed","/v1/embeddings","SELECT AI_EMBED('reg_embed','x',4)",("wrong-dimension","string-element","null-element","nonfinite","empty-vector","not-vector","duplicate-index","bad-index","missing-index","missing","bad-json","non-object","http-error")))
            suite.check("rerank malformed response",lambda:suite.faults("reg_rerank","/v1/rerank","SELECT AI_RERANK('reg_rerank','x','[\"x\",\"y\"]',NULL)",("bad-index","duplicate-index","missing","string-score","nonfinite","bad-json","non-object","http-error")))
            suite.check("cleanup",suite.cleanup)
    finally:
        suite.connection.close()
        server.shutdown()
        server.server_close()
    return int(any(not item["passed"] for item in suite.checks))


if __name__ == "__main__":
    raise SystemExit(main())
