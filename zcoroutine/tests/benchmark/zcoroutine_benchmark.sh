#!/usr/bin/env bash
# zcoroutine_benchmark.sh
# - 分别在独立栈/共享栈模式下：
#   1) perf record + perf report 导出热点前30函数
#   2) valgrind(cachegrind) 导出 L1(D1mr) 与 LLC/LL(DLmr) miss 前30函数
# - 结果输出到本目录 report/ 下

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

MODE="both"                 # normal|shared|both
PORT=9000
THREADS=4
PERF_SECONDS=12
WARMUP_MS=500
WRK_THREADS=4
WRK_CONNECTIONS=200
WRK_DURATION="15s"
PATH_REQ="/"

# 可重复：--wrk-arg --latency / --wrk-arg -R / --wrk-arg 1000
WRK_ARGS=()

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --mode [normal|shared|both]    Default: both
  --port N                       Default: 9000
  --threads N                    Default: 4
  --perf-seconds N               Default: 12
  --warmup-ms N                  Default: 500

  --wrk-threads N                Default: 4
  --wrk-connections N            Default: 200
  --wrk-duration STR             Default: 15s
  --path STR                     Default: /
  --wrk-arg ARG                  Extra arg forwarded to wrk (repeatable)

Env:
  ZCOROUTINE_BENCH_BIN            Override benchmark binary path

Output:
  ${SCRIPT_DIR}/report/<timestamp>/{normal,shared}/...
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode) MODE="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --threads) THREADS="$2"; shift 2;;
    --perf-seconds) PERF_SECONDS="$2"; shift 2;;
    --warmup-ms) WARMUP_MS="$2"; shift 2;;

    --wrk-threads) WRK_THREADS="$2"; shift 2;;
    --wrk-connections) WRK_CONNECTIONS="$2"; shift 2;;
    --wrk-duration) WRK_DURATION="$2"; shift 2;;
    --path) PATH_REQ="$2"; shift 2;;
    --wrk-arg) WRK_ARGS+=("$2"); shift 2;;

    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1"; usage; exit 2;;
  esac
done

pick_bin() {
  if [[ -n "${ZCOROUTINE_BENCH_BIN:-}" ]]; then
    echo "$ZCOROUTINE_BENCH_BIN"; return
  fi
  if [[ -x "./tests/zcoroutine_benchmark" ]]; then
    echo "./tests/zcoroutine_benchmark"; return
  fi
  if [[ -x "./zcoroutine_benchmark" ]]; then
    echo "./zcoroutine_benchmark"; return
  fi
  if [[ -x "${SCRIPT_DIR}/../../../../build/zcoroutine/tests/zcoroutine_benchmark" ]]; then
    echo "${SCRIPT_DIR}/../../../../build/zcoroutine/tests/zcoroutine_benchmark"; return
  fi
  echo ""; return
}

BIN=$(pick_bin)
if [[ -z "$BIN" ]]; then
  echo "Cannot find zcoroutine_benchmark binary." >&2
  echo "Try building first, or set ZCOROUTINE_BENCH_BIN." >&2
  exit 1
fi

mkdir -p "${SCRIPT_DIR}/report"
ROOT_OUT="${SCRIPT_DIR}/report/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$ROOT_OUT"

echo "Benchmark bin: $BIN"
echo "Report dir: $ROOT_OUT"

run_one() {
  local stack_mode="$1"   # independent|shared
  local tag="$2"          # normal|shared
  local out_dir="$ROOT_OUT/$tag"
  mkdir -p "$out_dir"

  local common_args=(
    --port "$PORT"
    --threads "$THREADS"
    --stack "$stack_mode"
    --warmup-ms "$WARMUP_MS"
    --wrk-threads "$WRK_THREADS"
    --wrk-connections "$WRK_CONNECTIONS"
    --wrk-duration "$WRK_DURATION"
    --path "$PATH_REQ"
  )

  local wrk_passthrough=()
  for a in "${WRK_ARGS[@]}"; do
    wrk_passthrough+=(--wrk-arg "$a")
  done

  echo ""
  echo "=============================="
  echo "Mode: $tag (stack=$stack_mode)"
  echo "=============================="

  # 1) perf + wrk
  echo "[1] Run benchmark (background)"
  ("$BIN" "${common_args[@]}" "${wrk_passthrough[@]}" >"$out_dir/bench.log" 2>&1) &
  BENCH_PID=$!
  echo "$BENCH_PID" >"$out_dir/bench.pid"

  # 等待 server + wrk 启动（wrk 在子进程里，这里只确保主进程活着）
  sleep 1
  if ! kill -0 "$BENCH_PID" 2>/dev/null; then
    echo "Benchmark process exited early." >&2
    tail -200 "$out_dir/bench.log" || true
    return 1
  fi

  echo "[2] perf record (${PERF_SECONDS}s)"
  perf record -F 99 -g -p "$BENCH_PID" -o "$out_dir/perf.data" -- sleep "$PERF_SECONDS" \
    2>"$out_dir/perf_record.err" || true

  echo "[3] perf stat (${PERF_SECONDS}s)"
  perf stat -e cache-references,cache-misses,instructions,cycles,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses \
    -p "$BENCH_PID" sleep "$PERF_SECONDS" 2>"$out_dir/perf_stat.txt" || true

  echo "[4] Wait benchmark finish"
  wait "$BENCH_PID" 2>/dev/null || true

  echo "[5] perf report + top30"
  perf report -i "$out_dir/perf.data" --stdio --no-children --sort symbol,dso \
    >"$out_dir/perf_report.txt" 2>/dev/null || true

  awk '/^[[:space:]]*[0-9]+\.[0-9]+%/{p=$1; gsub("%","",p); print p"\t"$0}' "$out_dir/perf_report.txt" \
    | sort -nr -k1,1 | head -30 | cut -f2- >"$out_dir/perf_top30.txt" || true

  # 2) cachegrind (trace-children=no 保证 wrk 子进程不被插桩)
  echo "[6] valgrind cachegrind (may be slow)"
  valgrind --tool=cachegrind --cache-sim=yes --branch-sim=no --trace-children=no \
    --cachegrind-out-file="$out_dir/cachegrind.out" \
    "$BIN" "${common_args[@]}" "${wrk_passthrough[@]}" \
    >"$out_dir/cachegrind_run.log" 2>&1 || true

  echo "[7] cg_annotate top30 (L1=D1mr, LLC=DLmr)"
  cg_annotate --show=D1mr --sort=D1mr --show-percs=no --threshold=0 "$out_dir/cachegrind.out" \
    >"$out_dir/cachegrind_D1mr.txt" || true
  awk 'BEGIN{in_tbl=0} /^-- File:function summary/{in_tbl=1; next} in_tbl && $1 ~ /^[0-9,]+$/{print}' "$out_dir/cachegrind_D1mr.txt" \
    | head -30 >"$out_dir/cache_miss_L1_top30.txt" || true

  cg_annotate --show=DLmr --sort=DLmr --show-percs=no --threshold=0 "$out_dir/cachegrind.out" \
    >"$out_dir/cachegrind_DLmr.txt" || true
  awk 'BEGIN{in_tbl=0} /^-- File:function summary/{in_tbl=1; next} in_tbl && $1 ~ /^[0-9,]+$/{print}' "$out_dir/cachegrind_DLmr.txt" \
    | head -30 >"$out_dir/cache_miss_LLC_top30.txt" || true

  echo "Done: $out_dir"
  echo "  - perf_top30.txt"
  echo "  - cache_miss_L1_top30.txt"
  echo "  - cache_miss_LLC_top30.txt"
}

case "$MODE" in
  normal)
    run_one independent normal
    ;;
  shared)
    run_one shared shared
    ;;
  both)
    run_one independent normal
    sleep 2
    run_one shared shared
    ;;
  *)
    echo "Invalid --mode: $MODE" >&2
    usage
    exit 2
    ;;
esac

echo "\nAll reports in: $ROOT_OUT"
