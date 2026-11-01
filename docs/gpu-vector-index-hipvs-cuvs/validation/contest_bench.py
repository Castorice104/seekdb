#!/usr/bin/env python3
"""Contest benchmark harness for the seekdb x AMD hipVS CAGRA contest.

Subcommands
-----------
prepare  Build a 250K-row dataset directory (base/query vectors, exact
         Top-100 ground truth, TSV load files, manifest with hashes).
run      Cold-start seekdb, load the table, time the CREATE VECTOR INDEX DDL
         through the first legal query, then time nq=4096 queries for
         Top-10 and Top-100 over one persistent connection.
compare  Pair a baseline run with a candidate run: per-dataset build/query
         speedups and the contest score model.
report   Render a Markdown report from a results directory.

Design notes:
  * the DDL and SQL text shape follow the contest rules
    (distance=l2, type=hnsw, lib=cuvs);
  * build timing = cold CREATE VECTOR INDEX .. first legal query returns k IDs;
  * one mysql client process executes all nq statements, so connection and
    process setup are excluded from query throughput;
  * recall is measured against exact Top-100 truth recomputed for the actual
    row subset, never against an upstream SIFT ground truth file.
"""

import argparse
import hashlib
import json
import math
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

import numpy as np

DEFAULT_M = 16
DEFAULT_EF_CONSTRUCTION = 200
DEFAULT_EF_SEARCH = 64
READY_TIMEOUT = 240


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def log(msg):
    sys.stderr.write("[bench %s] %s\n" % (time.strftime("%H:%M:%S"), msg))
    sys.stderr.flush()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 22)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def port_free(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        return sock.connect_ex(("127.0.0.1", port)) != 0
    finally:
        sock.close()


def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def vector_literal(vec):
    # "%.9g" round-trips float32 exactly, so the rows the engine actually stores
    # are bit-identical to the vectors the exact ground truth was computed from.
    return "[" + ",".join("%.9g" % float(v) for v in vec) + "]"


def write_tsv(path, vectors):
    with open(path, "w") as fh:
        for row, vec in enumerate(vectors):
            fh.write("%d\t%s\n" % (row + 1, vector_literal(vec)))


def read_fvecs(path, limit=None):
    rows = []
    with open(path, "rb") as fh:
        while limit is None or len(rows) < limit:
            header = fh.read(4)
            if len(header) < 4:
                break
            dim = int(np.frombuffer(header, dtype=np.int32)[0])
            payload = fh.read(dim * 4)
            if len(payload) < dim * 4:
                break
            rows.append(np.frombuffer(payload, dtype=np.float32).copy())
    if not rows:
        raise SystemExit("no vectors parsed from %s" % path)
    return np.vstack(rows)


def exact_topk(base, queries, k, block=64):
    base_norm = np.einsum("ij,ij->i", base, base)
    out = np.empty((queries.shape[0], k), dtype=np.int32)
    for start in range(0, queries.shape[0], block):
        q = queries[start:start + block]
        qn = np.einsum("ij,ij->i", q, q)[:, None]
        dist = base_norm[None, :] - 2.0 * q.dot(base.T) + qn
        idx = np.argpartition(dist, k - 1, axis=1)[:, :k]
        sub = np.take_along_axis(dist, idx, axis=1)
        order = np.argsort(sub, axis=1)
        out[start:start + block] = np.take_along_axis(idx, order, axis=1)
    return out


# --------------------------------------------------------------------------
# prepare
# --------------------------------------------------------------------------
def cmd_prepare(args):
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    if args.dataset == "sift250k":
        if not args.sift_base:
            raise SystemExit("--sift-base is required for --dataset sift250k "
                             "(SIFT1M base.fvecs; the first N rows are used)")
        base = read_fvecs(args.sift_base, limit=args.n)
        dim = base.shape[1]
        if args.sift_queries:
            queries = read_fvecs(args.sift_queries, limit=args.nq)
        else:
            pick = rng.choice(base.shape[0], size=args.nq, replace=False)
            queries = base[pick] + rng.normal(
                scale=0.05, size=(args.nq, dim)).astype(np.float32)
        notes = ("SIFT subset: first %d rows of %s; the official SIFT250K is only "
                 "available on the evaluation platform"
                 % (base.shape[0], os.path.basename(args.sift_base)))
    elif args.dataset == "synthetic":
        dim = args.dim
        base = rng.normal(size=(args.n, dim)).astype(np.float32)
        base /= np.linalg.norm(base, axis=1, keepdims=True)
        pick = np.linspace(0, args.n - 1, args.nq).astype(np.int64)
        queries = base[pick] + rng.normal(
            scale=0.02, size=(args.nq, dim)).astype(np.float32)
        queries /= np.linalg.norm(queries, axis=1, keepdims=True)
        notes = ("proxy dataset, fixed seed %d, %d x %d normalized gaussian; "
                 "cannot replace Hidden250K" % (args.seed, args.n, dim))
    else:
        raise SystemExit("unknown dataset %s" % args.dataset)

    base = np.ascontiguousarray(base, dtype=np.float32)
    queries = np.ascontiguousarray(queries, dtype=np.float32)
    if queries.shape[1] != base.shape[1]:
        raise SystemExit("dimension mismatch")

    base.tofile(os.path.join(out, "base.f32"))
    queries.tofile(os.path.join(out, "query.f32"))
    write_tsv(os.path.join(out, "base.tsv"), base)
    write_tsv(os.path.join(out, "query.tsv"), queries)
    log("computing exact Top-100 truth for %d queries over %d rows"
        % (queries.shape[0], base.shape[0]))
    t0 = time.time()
    truth = exact_topk(base, queries, 100)
    log("truth done in %.1fs" % (time.time() - t0))
    np.save(os.path.join(out, "truth.npy"), truth)

    manifest = {
        "dataset": args.dataset,
        "dim": int(dim),
        "n": int(base.shape[0]),
        "nq": int(queries.shape[0]),
        "seed": args.seed,
        "notes": notes,
        "base_sha256": sha256_file(os.path.join(out, "base.f32")),
        "query_sha256": sha256_file(os.path.join(out, "query.f32")),
        "truth_sha256": sha256_file(os.path.join(out, "truth.npy")),
        "bytes": {"base": int(base.nbytes), "query": int(queries.nbytes)},
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)
    log("prepared %s (n=%d dim=%d nq=%d)"
        % (out, manifest["n"], dim, manifest["nq"]))
    return 0


# --------------------------------------------------------------------------
# run
# --------------------------------------------------------------------------
class Observer(object):
    def __init__(self, args, run_dir):
        self.args = args
        self.run_dir = run_dir
        self.proc = None
        self.port = args.port
        self.bridge_dir = os.path.dirname(os.path.abspath(args.bridge))
        self.vram_samples = []
        self.samples = {}
        self.logpath = os.path.join(run_dir, "observer.log")
        self.logfh = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False

    def sql(self, statement, database=None, timeout=600, local_infile=False):
        cmd = ["mysql", "--protocol=TCP", "-h127.0.0.1", "-P%d" % self.port,
               "-uroot", "-A", "-N", "-B", "--connect-timeout=10"]
        if local_infile:
            cmd.insert(1, "--local-infile=1")
        if database:
            cmd.append(database)
        cmd += ["-e", statement]
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)

    def sql_file(self, path, database=None, timeout=3600):
        cmd = ["mysql", "--protocol=TCP", "-h127.0.0.1", "-P%d" % self.port,
               "-uroot", "-A", "-N", "-B", "--connect-timeout=10"]
        if database:
            cmd.append(database)
        started = time.time()
        with open(path) as fh:
            proc = subprocess.run(cmd, stdin=fh, capture_output=True, text=True,
                                  timeout=timeout)
        elapsed = time.time() - started
        if proc.returncode != 0:
            raise SystemExit("mysql script failed (rc=%d): %s\n%s\n%s"
                             % (proc.returncode, path, proc.stdout[-2000:],
                                proc.stderr[-2000:]))
        return elapsed, proc.stdout

    def start(self):
        if not port_free(self.port):
            raise SystemExit("port %d already in use" % self.port)
        os.makedirs(os.path.join(self.run_dir, "store"), exist_ok=True)
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = "%s:/opt/hipvs/lib:/opt/rocm/lib" % self.bridge_dir
        env.pop("OB_VSAG_USE_CUVS", None)
        if self.args.trace:
            env["OB_VSAG_TRACE"] = "1"
            env["OB_VSAG_TRACE_FILE"] = self.args.trace
        cmd = [self.args.observer, "--nodaemon", "--port", str(self.port),
               "--data-dir", os.path.join(self.run_dir, "store"),
               "--base-dir", self.run_dir,
               "--parameter", "__min_full_resource_pool_memory=2147483648",
               "--parameter", "memory_budget=%s" % self.args.memory_budget,
               "--parameter", "datafile_size=%s" % self.args.datafile_size,
               "--parameter", "cpu_count=%d" % self.args.cpu_count,
               "--parameter", "local_ip=127.0.0.1"]
        log("starting observer on port %d" % self.port)
        self.logfh = open(self.logpath, "w")
        self.proc = subprocess.Popen(cmd, env=env, stdout=self.logfh,
                                     stderr=subprocess.STDOUT,
                                     preexec_fn=os.setsid)
        deadline = time.time() + READY_TIMEOUT
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise SystemExit("observer exited early, see %s" % self.logpath)
            probe = self.sql("select 1", timeout=30)
            if probe.returncode == 0:
                log("observer ready")
                return self
            time.sleep(1.0)
        raise SystemExit("observer did not become ready in %ds" % READY_TIMEOUT)

    def sample(self):
        dev = self.args.vram_sysfs
        value = None
        if dev:
            try:
                with open(os.path.join(dev, "mem_info_vram_used")) as fh:
                    value = int(fh.read().strip())
            except OSError:
                value = None
        if value is not None:
            self.vram_samples.append(value)
        if self.proc is not None:
            try:
                with open("/proc/%d/status" % self.proc.pid) as fh:
                    for line in fh:
                        if line.startswith("VmHWM:"):
                            self.samples["vm_hwm_bytes"] = int(line.split()[1]) * 1024
                        elif line.startswith("VmRSS:"):
                            self.samples["vm_rss_bytes"] = int(line.split()[1]) * 1024
            except OSError:
                pass

    def start_sampler(self, interval=0.25):
        stop_flag = [False]

        def loop():
            while not stop_flag[0]:
                self.sample()
                time.sleep(interval)

        thread = threading.Thread(target=loop, daemon=True)
        thread.start()

        def halt():
            stop_flag[0] = True
            thread.join(timeout=5)

        return halt

    def stop(self):
        if self.proc is None:
            return
        self.sample()
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except OSError:
            pass
        try:
            self.proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except OSError:
                pass
            self.proc.wait(timeout=30)
        if self.logfh is not None:
            self.logfh.close()
        self.proc = None


def build_query_script(path, queries, k, approximate=True):
    with open(path, "w") as fh:
        suffix = " approximate" if approximate else ""
        for vec in queries:
            fh.write("select group_concat(id order by id) from (select id from items "
                     "order by l2_distance(v,%s)%s limit %d) t;\n"
                     % (vector_literal(vec), suffix, k))
    return path


def parse_id_lists(stdout, nq):
    rows = [line.strip() for line in stdout.splitlines() if line.strip()]
    lists = []
    for row in rows:
        if row.upper() == "NULL":
            lists.append([])
            continue
        lists.append([int(x) for x in row.split(",") if x])
    if len(lists) != nq:
        raise SystemExit("expected %d result rows, got %d" % (nq, len(lists)))
    return lists


def cmd_run(args):
    ds = os.path.abspath(args.dataset_dir)
    manifest = json.load(open(os.path.join(ds, "manifest.json")))
    n, dim, nq = manifest["n"], manifest["dim"], manifest["nq"]
    queries = np.fromfile(os.path.join(ds, "query.f32"), dtype=np.float32).reshape(nq, dim)
    truth = np.load(os.path.join(ds, "truth.npy"))
    topks = [int(x) for x in args.topk.split(",") if x.strip()]
    outdir = os.path.abspath(args.out)
    os.makedirs(outdir, exist_ok=True)
    work = os.path.abspath(args.work_dir) if args.work_dir else os.path.join(outdir, "work-" + args.tag)
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    db = "bench_%s" % args.tag.replace("-", "_")
    ddl = ("create vector index idx on items(v) with (distance=l2,type=hnsw,lib=%s,"
           "m=%d,ef_construction=%d,ef_search=%d);"
           % (args.lib, args.m, args.ef_construction, args.ef_search))
    result = {
        "tag": args.tag,
        "lib": args.lib,
        "observer": os.path.abspath(args.observer),
        "bridge": os.path.abspath(args.bridge),
        "observer_sha256": sha256_file(args.observer),
        "bridge_sha256": sha256_file(args.bridge),
        "dataset": manifest,
        "m": args.m,
        "ef_construction": args.ef_construction,
        "ef_search": args.ef_search,
        "topk": topks,
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "host": os.uname().nodename,
        "nproc": os.cpu_count(),
    }
    if args.note:
        result["note"] = args.note

    observer = Observer(args, os.path.join(work, "run"))
    halt = None
    try:
        observer.start()
        halt = observer.start_sampler()

        load_start = time.time()
        stmt = ("create database if not exists %s; use %s; drop table if exists items;"
                "create table items(id bigint primary key, v vector(%d));"
                % (db, db, dim))
        r = observer.sql(stmt, timeout=300)
        if r.returncode != 0:
            raise SystemExit("failed to create table: %s" % r.stderr[-2000:])
        tsv = os.path.join(ds, "base.tsv").replace("'", "\\'")
        sess = ("set session ob_query_timeout=1000000000; set session ob_trx_timeout=1000000000; ")
        tsv_sql = "load data local infile '%s' into table items fields "
        tsv_sql += "terminated by '\\t' lines terminated by '\\n' (id,v);"
        r = observer.sql(sess + tsv_sql % tsv, database=db, timeout=3600, local_infile=True)
        if r.returncode != 0:
            raise SystemExit("load data failed: %s" % r.stderr[-2000:])
        count = observer.sql("select count(*) from items;", database=db).stdout.strip()
        if int(count) != n:
            raise SystemExit("loaded %s rows, expected %d" % (count, n))
        result["load_seconds"] = round(time.time() - load_start, 3)
        log("loaded %d rows in %.1fs" % (n, result["load_seconds"]))

        # build timing: cold DDL until the first legal query completes
        script = os.path.join(work, "first-query.sql")
        build_query_script(script, queries[:1], topks[0])
        post = os.path.join(work, "after-ddl.sql")
        with open(post, "w") as fh:
            fh.write(ddl + "\n")
            fh.write(open(script).read())
        t0 = time.time()
        elapsed_ddl, out_ddl = observer.sql_file(post, database=db, timeout=7200)
        build_seconds = time.time() - t0
        first_ids = parse_id_lists(out_ddl, 1)[0]
        if len(first_ids) != topks[0]:
            raise SystemExit("first query returned %d ids, expected %d"
                             % (len(first_ids), topks[0]))
        result["build_seconds"] = round(build_seconds, 3)
        result["build_ddl_seconds"] = round(elapsed_ddl, 3)
        log("build (DDL -> first legal query) = %.2fs" % build_seconds)

        if args.warmup > 0:
            warm_script = os.path.join(work, "warmup.sql")
            build_query_script(warm_script, queries[:args.warmup], topks[-1])
            observer.sql_file(warm_script, database=db)

        per_topk = {}
        for k in topks:
            script = os.path.join(work, "query-k%d.sql" % k)
            build_query_script(script, queries, k)
            elapsed, stdout = observer.sql_file(script, database=db, timeout=7200)
            lists = parse_id_lists(stdout, nq)
            hits = 0
            integrity_errors = 0
            empty = 0
            for i, ids in enumerate(lists):
                if len(ids) != k or len(set(ids)) != k or min(ids) < 1 or max(ids) > n:
                    integrity_errors += 1
                    if not ids:
                        empty += 1
                    continue
                got = [x - 1 for x in ids]
                want = set(int(x) for x in truth[i, :k])
                hits += sum(1 for g in got if g in want)
            recall = hits / float(nq * k)
            per_topk[str(k)] = {
                "queries": nq,
                "seconds": round(elapsed, 3),
                "qps": round(nq / elapsed, 2),
                "ms_per_query": round(elapsed * 1000.0 / nq, 3),
                "recall": round(recall, 6),
                "integrity_errors": integrity_errors,
                "empty_results": empty,
            }
            log("top-%d: %.1f qps, recall=%.4f, integrity_errors=%d"
                % (k, nq / elapsed, recall, integrity_errors))
        result["per_topk"] = per_topk
        if len(topks) == 2:
            qps = [per_topk[str(k)]["qps"] for k in topks]
            result["qps_geomean"] = round(math.sqrt(qps[0] * qps[1]), 3)

        if args.latency_probe > 0:
            overheads = []
            for _ in range(args.latency_probe):
                t = time.time()
                observer.sql("select 1;", database=db)
                overheads.append(time.time() - t)
            overhead = float(np.median(overheads))
            lat = []
            step = max(1, nq // args.latency_probe)
            for i in range(0, nq, step):
                probe_script = os.path.join(work, "probe.sql")
                build_query_script(probe_script, queries[i:i + 1], topks[0])
                t = time.time()
                observer.sql_file(probe_script, database=db)
                lat.append((time.time() - t) - overhead)
            arr = np.array(lat)
            result["latency_probe"] = {
                "samples": len(lat),
                "client_startup_overhead_ms": round(overhead * 1000.0, 3),
                "median_ms": round(float(np.median(arr)) * 1000.0, 3),
                "p95_ms": round(float(np.percentile(arr, 95)) * 1000.0, 3),
                "note": "diagnostic only: one mysql client process per query, "
                        "measured startup overhead subtracted",
            }
    finally:
        if halt is not None:
            halt()
        observer.stop()
        result["vm_hwm_mb"] = round(observer.samples.get("vm_hwm_bytes", 0) / 1048576.0, 1)
        result["vm_rss_mb"] = round(observer.samples.get("vm_rss_bytes", 0) / 1048576.0, 1)
        if observer.vram_samples:
            result["vram_peak_mb"] = round(max(observer.vram_samples) / 1048576.0, 1)
        try:
            tail = open(observer.logpath, errors="replace").read()[-4000:]
            result["observer_log_tail"] = tail
            result["observer_log_markers"] = {
                token: tail.count(token)
                for token in ("cuvs_build", "cuvs_serve", "serialized GPU CAGRA snapshot",
                              "deserialized GPU CAGRA snapshot", "lazily built VSAG fallback")
            }
        except OSError:
            pass
        result["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        path = os.path.join(outdir, args.tag + ".json")
        with open(path, "w") as fh:
            json.dump(result, fh, indent=2, sort_keys=True)
        log("wrote %s" % path)
    return 0


# --------------------------------------------------------------------------
# compare / report
# --------------------------------------------------------------------------
def load_result(path):
    with open(path) as fh:
        return json.load(fh)


def dataset_speedups(base, cand):
    out = {}
    if "build_seconds" in base and "build_seconds" in cand:
        out["build_speedup"] = round(base["build_seconds"] / cand["build_seconds"], 4)
    for k, metrics in cand.get("per_topk", {}).items():
        b = base.get("per_topk", {}).get(k)
        if not b:
            continue
        out["qps_speedup_top%s" % k] = round(metrics["qps"] / b["qps"], 4)
        out["recall_delta_top%s" % k] = round(metrics["recall"] - b["recall"], 6)
        out["candidate_recall_top%s" % k] = metrics["recall"]
        out["baseline_recall_top%s" % k] = b["recall"]
    if "vram_peak_mb" in base and "vram_peak_mb" in cand:
        out["vram_ratio"] = round(cand["vram_peak_mb"] / base["vram_peak_mb"], 4)
    return out


def score(build_speedup, query_speedup):
    b = math.log(build_speedup) / math.log(3.0) if build_speedup > 0 else 0.0
    q = math.log(query_speedup) / math.log(1.5) if query_speedup > 0 else 0.0
    return round(4.0 + 14.0 * min(1.0, max(0.0, b)) + 2.0 * min(1.0, max(0.0, q)), 4)


def cmd_compare(args):
    base = load_result(args.baseline)
    cand = load_result(args.candidate)
    per = dataset_speedups(base, cand)
    payload = {
        "baseline": {"tag": base.get("tag"), "dataset": base.get("dataset", {}).get("dataset")},
        "candidate": {"tag": cand.get("tag"), "dataset": cand.get("dataset", {}).get("dataset")},
        "dataset_speedups": per,
    }
    # The contest score is built from B = Bsift^0.4 * Bhidden^0.6 (similarly Q),
    # so the two datasets are combined by the caller, not here. --combine turns
    # one dataset pair into the per-dataset B and Q (build speedup, geometric mean
    # of the Top-10/Top-100 QPS speedups).
    if args.combine:
        qs = [v for k, v in per.items() if k.startswith("qps_speedup_top")]
        bs = per.get("build_speedup")
        if qs and bs:
            query_geo = math.exp(sum(math.log(x) for x in qs) / len(qs))
            payload["combined"] = {"build_speedup": bs, "query_speedup": round(query_geo, 4)}
            payload["projected_score"] = score(bs, query_geo)
    # Hard gates from the contest rules: the candidate must stay within 0.005 of
    # the same-round baseline recall and above the absolute floor, must not lose
    # more than 10% of the baseline QPS on either top-k, and must keep VRAM within
    # 1.25x the baseline. The absolute floor is 0.95/0.85 on SIFT250K and
    # 0.90/0.85 on the hidden set; a proxy dataset uses the hidden floors.
    dataset_name = base.get("dataset", {}).get("dataset")
    floor10, floor100 = (0.95, 0.85) if dataset_name == "sift250k" else (0.90, 0.85)
    gates = []
    for key, value in per.items():
        if key.startswith("recall_delta_top"):
            gates.append({"gate": "%s drop<=0.005" % key, "value": value,
                          "pass": value >= -0.005})
        elif key == "candidate_recall_top100":
            gates.append({"gate": "%s >= %.2f" % (key, floor100), "value": value,
                          "pass": value >= floor100})
        elif key == "candidate_recall_top10":
            gates.append({"gate": "%s >= %.2f" % (key, floor10), "value": value,
                          "pass": value >= floor10})
        elif key.startswith("qps_speedup_top"):
            gates.append({"gate": "%s >= 0.90" % key, "value": value,
                          "pass": value >= 0.90})
        elif key == "vram_ratio":
            gates.append({"gate": "vram <= 1.25x baseline", "value": value,
                          "pass": value <= 1.25})
    payload["gates"] = gates
    payload["gates_pass"] = all(g["pass"] for g in gates) if gates else None
    text = json.dumps(payload, indent=2, sort_keys=True)
    print(text)
    if args.json:
        with open(args.json, "w") as fh:
            fh.write(text)
    return 0


def cmd_report(args):
    results = os.path.abspath(args.results)
    files = sorted(f for f in os.listdir(results) if f.endswith(".json"))
    if not files:
        raise SystemExit("no result json in %s" % results)
    rows = []
    for name in files:
        try:
            data = load_result(os.path.join(results, name))
        except ValueError:
            continue
        if "per_topk" not in data:
            continue
        rows.append(data)
    lines = ["# GPU vector index benchmark", ""]
    lines.append("| tag | lib | build s | top-k | QPS | ms/query | recall | integrity err | VRAM MB | RSS MB |")
    lines.append("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for data in rows:
        for k, m in sorted(data["per_topk"].items(), key=lambda kv: int(kv[0])):
            lines.append("| %s | %s | %.2f | %s | %.1f | %.2f | %.4f | %d | %s | %s |" % (
                data.get("tag"), data.get("lib"), data.get("build_seconds", 0), k,
                m["qps"], m["ms_per_query"], m["recall"], m["integrity_errors"],
                data.get("vram_peak_mb", "-"), data.get("vm_hwm_mb", "-")))
    lines.append("")
    for data in rows:
        ds = data.get("dataset", {})
        lines.append("- %s: dataset=%s n=%s dim=%s nq=%s base_sha256=%s" % (
            data.get("tag"), ds.get("dataset"), ds.get("n"), ds.get("dim"),
            ds.get("nq"), (ds.get("base_sha256") or "")[:16]))
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(text)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="GPU vector index contest benchmark")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("prepare", help="build a dataset directory")
    p.add_argument("--dataset", choices=["sift250k", "synthetic"], required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--n", type=int, default=250000)
    p.add_argument("--nq", type=int, default=4096)
    p.add_argument("--dim", type=int, default=256)
    p.add_argument("--seed", type=int, default=1314)
    p.add_argument("--sift-base")
    p.add_argument("--sift-queries")
    p.set_defaults(func=cmd_prepare)

    p = sub.add_parser("run", help="run one benchmark round")
    p.add_argument("--dataset-dir", required=True)
    p.add_argument("--observer", required=True)
    p.add_argument("--bridge", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--work-dir")
    p.add_argument("--port", type=int, default=0)
    p.add_argument("--lib", default="cuvs")
    p.add_argument("--m", type=int, default=DEFAULT_M)
    p.add_argument("--ef-construction", type=int, default=DEFAULT_EF_CONSTRUCTION)
    p.add_argument("--ef-search", type=int, default=DEFAULT_EF_SEARCH)
    p.add_argument("--topk", default="10,100")
    p.add_argument("--warmup", type=int, default=32)
    p.add_argument("--latency-probe", type=int, default=0)
    p.add_argument("--memory-budget", default="48G")
    p.add_argument("--datafile-size", default="64G")
    p.add_argument("--cpu-count", type=int, default=24)
    p.add_argument("--trace")
    p.add_argument("--vram-sysfs", default="/sys/class/drm/card0/device")
    p.add_argument("--note")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("compare", help="compare candidate against baseline")
    p.add_argument("--baseline", required=True)
    p.add_argument("--candidate", required=True)
    p.add_argument("--combine", action="store_true",
                   help="also report per-dataset B and Q and the projected score")
    p.add_argument("--json")
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("report", help="render a markdown report")
    p.add_argument("--results", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_report)

    args = parser.parse_args(argv)
    if args.command == "run" and not args.port:
        args.port = free_port()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
