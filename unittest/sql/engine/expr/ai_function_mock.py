#!/usr/bin/env python3
"""Deterministic AI protocol fixture. Never connects to an external model.

Run: python3 ai_function_mock.py --port 8000
GET /events returns received request bodies; DELETE /events clears them.
Faults are selected through /fault/<mode>/<normal API path>.
"""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


def embedding(text, dimension):
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return [(digest[i % len(digest)] - 128) / 128.0 for i in range(dimension)]


def response_for(path, body):
    parts = path.strip("/").split("/")
    fault = parts[1] if len(parts) > 2 and parts[0] == "fault" else ""
    if fault == "http-error":
        return 400, {"error": {"message": "injected transport failure"}}
    if fault == "bad-json":
        return 200, b"{invalid json"
    if fault == "non-object":
        return 200, []
    if path.endswith("/chat/completions"):
        messages = body.get("messages")
        if not isinstance(messages, list) or not messages:
            return 400, {"error": "messages must be a nonempty array"}
        prompt = messages[-1].get("content")
        if not isinstance(prompt, str):
            return 400, {"error": "content must be a string"}
        content = "mock:" + prompt
        if fault == "empty":
            content = ""
        if fault == "null-content":
            content = None
        return 200, {"choices": [{"message": {"role": "assistant", "content": content}}]}
    if path.endswith("/api/generate"):
        if body.get("stream") is not False or not isinstance(body.get("prompt"), str):
            return 400, {"error": "expected native nonstream generate"}
        if "temperature" in body or "max_tokens" in body:
            return 400, {"error": "generation options belong in options"}
        return 200, {"response": "" if fault == "empty" else "mock:" + body["prompt"], "done": True}
    if path.endswith("/embeddings") or path.endswith("/api/embed"):
        inputs = body.get("input")
        if isinstance(inputs, str):
            inputs = [inputs]
        if not isinstance(inputs, list) or not inputs or not all(isinstance(x, str) for x in inputs):
            return 400, {"error": "input must contain strings"}
        dimension = body.get("dimensions", 768)
        if fault == "late-error" and dimension == 5:
            return 400, {"error": "injected failure in later dimension group"}
        if not isinstance(dimension, int) or isinstance(dimension, bool) or not 0 < dimension <= 4096:
            return 400, {"error": "invalid dimensions"}
        vectors = [embedding(text, dimension) for text in inputs]
        if fault == "wrong-dimension":
            vectors[0].append(0.0)
        elif fault == "string-element":
            vectors[0][0] = "not-a-number"
        elif fault == "null-element":
            vectors[0][0] = None
        elif fault == "nonfinite":
            vectors[0][0] = float("inf")
        elif fault == "empty-vector":
            vectors[0] = []
        elif fault == "not-vector":
            vectors[0] = 1
        if path.endswith("/api/embed"):
            response = {"embeddings": vectors}
            if fault == "missing":
                response["embeddings"] = vectors[:-1]
        else:
            data = [{"index": i, "embedding": vector} for i, vector in enumerate(vectors)]
            if fault == "duplicate-index":
                data.append(dict(data[0]))
            elif fault == "bad-index":
                data[0]["index"] = len(data)
            elif fault == "missing-index":
                data[0].pop("index")
            elif fault == "missing":
                data.pop()
            response = {"data": list(reversed(data))}
        if fault == "nonfinite":
            return 200, json.dumps(response).replace("Infinity", "1e309").encode()
        return 200, response
    if path.endswith("/rerank"):
        docs = body.get("documents")
        query = body.get("query")
        if not isinstance(docs, list) or not all(isinstance(x, str) for x in docs) or not isinstance(query, str):
            return 400, {"error": "invalid rerank input"}
        results = [{"index": i, "relevance_score": float(doc.count(query))} for i, doc in enumerate(docs)]
        if fault == "bad-index" and results:
            results[0]["index"] = len(docs)
        elif fault == "duplicate-index" and len(results) > 1:
            results[1]["index"] = results[0]["index"]
        elif fault == "missing":
            results = results[:-1]
        elif fault == "string-score" and results:
            results[0]["relevance_score"] = "bad"
        elif fault == "nonfinite" and results:
            results[0]["relevance_score"] = float("inf")
        response = {"results": list(reversed(results))}
        if fault == "nonfinite":
            return 200, json.dumps(response).replace("Infinity", "1e309").encode()
        return 200, response
    return 404, {"error": "unsupported fixture path"}


def make_server(host="0.0.0.0", port=8000):
    events = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def respond(self, status, data):
            payload = data if isinstance(data, bytes) else json.dumps(data, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            with lock:
                self.respond(200, list(events))

        def do_DELETE(self):
            with lock:
                events.clear()
            self.respond(200, {"ok": True})

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                status, response = response_for(urlsplit(self.path).path, body)
            except (ValueError, TypeError, KeyError):
                status, response, body = 400, {"error": "malformed request"}, None
            with lock:
                events.append({"path": self.path, "body": body, "status": status,
                               "authorization": self.headers.get("Authorization")})
            self.respond(status, response)

    return ThreadingHTTPServer((host, port), Handler)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    with make_server(port=args.port) as server:
        server.serve_forever()
