#!/usr/bin/env bash
# GPU-window runbook for the seekdb x AMD cuVS CAGRA contest.
#
# Steps are separate so a slow or failing step can be inspected without
# redoing the previous ones. W is a scratch dir, e.g. /work/contest.
#
#   contest_gpu.sh probe                  # environment report
#   contest_gpu.sh bridges  W             # baseline (v1) + candidate (v2) bridge .so
#   contest_gpu.sh obs      W name ref    # build seekdb at git ref -> W/obs-name/seekdb
#   contest_gpu.sh round    W ds tag obs bridge [ENV=VAL ...]
#   contest_gpu.sh sweep    W ds          # the measurement matrix
#   contest_gpu.sh report   W
#
# Datasets come from: python3 contest_bench.py prepare ...
set -uo pipefail

BENCH_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$BENCH_DIR/../../.." && pwd)
BASE_COMMIT=634888d9b037153fa143d2c913ba4b25503e1d62
JOBS=64

die() { echo "[contest_gpu][ERROR] $*" >&2; exit 2; }
say() { echo "[contest_gpu] $*"; }

cmd_probe() {
  say "host=$(uname -n) nproc=$(nproc)"
  free -g | sed 's/^/    /'
  say "rocm:  $(ls -d /opt/rocm* 2>/dev/null | tr '\n' ' ')"
  say "hipvs: $(ls -d /opt/hipvs 2>/dev/null)"
  rocminfo 2>/dev/null | grep -m3 -E 'Marketing Name|gfx' | sed 's/^/    /'
  rocm-smi --showmeminfo vram 2>/dev/null | head -8 | sed 's/^/    /'
  say "datasets: $(ls /work/datasets 2>/dev/null | head -5 | tr '\n' ' ')"
  say "bench:    $(ls /work/bench 2>/dev/null | head -5 | tr '\n' ' ')"
  say "mysql:    $(command -v mysql || echo missing)"
  say "python:   $(python3 -c 'import numpy,sys;print(sys.version.split()[0], numpy.__version__)' 2>/dev/null || echo 'no numpy')"
  say "repo:     $REPO"
  say "head:     $(git -C "$REPO" rev-parse --short HEAD) on $(git -C "$REPO" rev-parse --abbrev-ref HEAD)"
  if git -C "$REPO" merge-base --is-ancestor "$BASE_COMMIT" HEAD; then
    say "base:     $BASE_COMMIT is an ancestor of HEAD (submission rule satisfied)"
  else
    say "base:     ANCESTOR CHECK FAILED"
  fi
}

cmd_bridges() {
  local W=$1
  mkdir -p "$W/bridge" "$W/src"
  command -v hipcc >/dev/null 2>&1 || die "hipcc missing"
  say "building baseline bridge (v1) from $BASE_COMMIT"
  rm -rf "$W/src/bridge-v1"
  mkdir -p "$W/src/bridge-v1"
  git -C "$REPO" archive "$BASE_COMMIT" tools/hipvs_bridge | tar -x -C "$W/src/bridge-v1" || die "git archive failed"
  ( cd "$W/src/bridge-v1" && bash tools/hipvs_bridge/build.sh "$W/bridge-v1.so" ) || die "baseline bridge build failed"
  say "building candidate bridge (v2) from HEAD"
  bash "$REPO/tools/hipvs_bridge/build.sh" "$W/bridge-v2.so" || die "candidate bridge build failed"
  ls -l "$W"/bridge-v1.so "$W"/bridge-v2.so | sed 's/^/    /'
  # The observer records this absolute path at link time, so every round
  # installs the chosen variant here.
  cp "$W/bridge-v2.so" "$W/bridge/libseekdb_cuvs_bridge.so"
}

cmd_obs() {
  local W=$1 name=$2 ref=$3 src bin
  src="$W/src/obs-$name"
  [ -f "$W/bridge/libseekdb_cuvs_bridge.so" ] || die "run 'bridges' first"
  command -v cmake >/dev/null 2>&1 || die "cmake missing"
  say "building observer '$name' from $ref"
  rm -rf "$src"
  git -C "$REPO" worktree add --detach "$src" "$ref" >/dev/null || die "worktree add failed"
  ( cd "$src" && ./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF -DCUVS_BRIDGE_LIB="$W/bridge/libseekdb_cuvs_bridge.so" --make -j"$JOBS" ) 2>&1 | tail -25
  bin="$src/build_release/src/observer/seekdb"
  [ -x "$bin" ] || die "observer binary missing: $bin"
  mkdir -p "$W/obs-$name"
  cp "$bin" "$W/obs-$name/seekdb"
  say "observer '$name' -> $W/obs-$name/seekdb ($(stat -c%s "$bin") bytes)"
}

cmd_round() {
  local W=$1 ds=$2 tag=$3 obs=$4 bridge=$5 bin so
  shift 5
  bin="$W/obs-$obs/seekdb"
  so="$W/bridge-$bridge.so"
  [ -x "$bin" ] || die "no observer: $bin"
  [ -f "$so" ] || die "no bridge: $so"
  cp "$so" "$W/bridge/libseekdb_cuvs_bridge.so"
  say "round $tag: observer=$obs bridge=$bridge env=$*"
  # Tunables are env-driven, so a value exported by an earlier round must not
  # leak into this one (otherwise the trailing base2 drift round would silently
  # run with the last swept niter).
  unset SEEKDB_CUVS_NITER SEEKDB_CUVS_GRAPH_DEGREE SEEKDB_CUVS_INTERMEDIATE_DEGREE \
        SEEKDB_CUVS_ITOPK SEEKDB_CUVS_SEARCH_WIDTH
  if [ "$#" -gt 0 ]; then export "$@"; fi
  unset MYSQL_PWD
  python3 "$BENCH_DIR/contest_bench.py" run --dataset-dir "$ds" \
    --observer "$bin" --bridge "$W/bridge/libseekdb_cuvs_bridge.so" \
    --tag "$tag" --out "$W/results" --work-dir "$W/run-$tag" \
    --datafile-size 20G --cpu-count "$(nproc)" || die "round $tag failed"
}

cmd_sweep() {
  local W=$1 ds=$2
  say "sweep over $ds"
  cmd_round "$W" "$ds" base    base v1
  cmd_round "$W" "$ds" reuse   base v2
  cmd_round "$W" "$ds" worker  cand v1
  cmd_round "$W" "$ds" cand    cand v2
  cmd_round "$W" "$ds" n60     cand v2 SEEKDB_CUVS_NITER=60
  cmd_round "$W" "$ds" n40     cand v2 SEEKDB_CUVS_NITER=40
  cmd_round "$W" "$ds" n20     cand v2 SEEKDB_CUVS_NITER=20
  cmd_round "$W" "$ds" base2   base v1
}

cmd_report() {
  local W=$1
  python3 "$BENCH_DIR/contest_bench.py" report --results "$W/results" --out "$W/report.md" || die "report failed"
  say "report -> $W/report.md"
}

main() {
  local step
  if [ "$#" -lt 1 ]; then sed -n '3,16p' "$0"; exit 2; fi
  step=$1
  shift
  case "$step" in
    probe)   cmd_probe ;;
    bridges) cmd_bridges "$1" ;;
    obs)     cmd_obs "$1" "$2" "$3" ;;
    round)   cmd_round "$@" ;;
    sweep)   cmd_sweep "$1" "$2" ;;
    report)  cmd_report "$1" ;;
    *) sed -n '3,16p' "$0"; exit 2 ;;
  esac
}
main "$@"
