#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-baseline}"
shift || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
RESULT_ROOT="${RESULT_ROOT:-${SCRIPT_DIR}/perf_results}"
TS="$(date +%Y%m%d_%H%M%S)"

BIN_CANDIDATES=(
  "${BUILD_DIR}/znet/tests/znet_wrk_benchmark"
  "${BUILD_DIR}/znet_wrk_benchmark"
)

BIN=""
for candidate in "${BIN_CANDIDATES[@]}"; do
  if [[ -x "${candidate}" ]]; then
    BIN="${candidate}"
    break
  fi
done

if [[ -z "${BIN}" ]]; then
  echo "error: znet_wrk_benchmark not found. Build first:" >&2
  echo "  cmake -S ${ROOT_DIR} -B ${BUILD_DIR} -G Ninja" >&2
  echo "  cmake --build ${BUILD_DIR} -j --target znet_wrk_benchmark" >&2
  exit 2
fi

mkdir -p "${RESULT_ROOT}"

run_baseline() {
  local out_dir="${RESULT_ROOT}/baseline_${TS}"
  mkdir -p "${out_dir}"

  "${BIN}" "$@" | tee "${out_dir}/benchmark_output.txt"
  echo "baseline output: ${out_dir}/benchmark_output.txt"
}

run_perf() {
  local out_dir="${RESULT_ROOT}/perf_${TS}"
  mkdir -p "${out_dir}"

  perf record -F 99 -g -o "${out_dir}/perf.data" -- "${BIN}" "$@" \
    > "${out_dir}/benchmark_output.txt" 2>&1

  perf report -i "${out_dir}/perf.data" --stdio \
    > "${out_dir}/perf_report.txt" 2>&1 || true

  echo "perf data: ${out_dir}/perf.data"
  echo "perf report: ${out_dir}/perf_report.txt"
}

run_valgrind() {
  local tool="${VALGRIND_TOOL:-cachegrind}"
  local out_dir="${RESULT_ROOT}/valgrind_${TS}"
  local scale_pct="${ZNET_PERF_SCALE_PCT:-25}"
  mkdir -p "${out_dir}"

  ZNET_PERF_SCALE_PCT="${scale_pct}" \
  valgrind --tool="${tool}" \
    --cachegrind-out-file="${out_dir}/cachegrind.out" \
    "${BIN}" "$@" > "${out_dir}/benchmark_output.txt" 2>&1

  echo "valgrind output: ${out_dir}/benchmark_output.txt"
  echo "cachegrind file: ${out_dir}/cachegrind.out"
}

case "${MODE}" in
  baseline)
    run_baseline "$@"
    ;;
  perf)
    run_perf "$@"
    ;;
  valgrind)
    run_valgrind "$@"
    ;;
  *)
    echo "usage: $0 [baseline|perf|valgrind] [benchmark args...]" >&2
    exit 2
    ;;
esac
