#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-baseline}"
shift || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-zhttp-perf}"
RESULT_ROOT="${RESULT_ROOT:-${SCRIPT_DIR}/perf_results}"
TS="$(date +%Y%m%d_%H%M%S)"

BIN_CANDIDATES=(
  "${BUILD_DIR}/zhttp/tests/zhttp_benchmark"
  "${BUILD_DIR}/zhttp_benchmark"
)

BIN=""
for candidate in "${BIN_CANDIDATES[@]}"; do
  if [[ -x "${candidate}" ]]; then
    BIN="${candidate}"
    break
  fi
done

if [[ -z "${BIN}" ]]; then
  echo "error: zhttp_benchmark not found. Build first:" >&2
  echo "  cmake -S ${ROOT_DIR} -B ${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF" >&2
  echo "  cmake --build ${BUILD_DIR} -j --target zhttp_benchmark" >&2
  exit 2
fi

mkdir -p "${RESULT_ROOT}"

server_threads="${ZHTTP_SERVER_THREADS:-4}"
wrk_threads="${ZHTTP_WRK_THREADS:-4}"
wrk_connections="${ZHTTP_WRK_CONNECTIONS:-256}"
wrk_duration="${ZHTTP_WRK_DURATION:-10s}"
bench_mode="${ZHTTP_BENCH_MODE:-all}"
bench_path="${ZHTTP_BENCH_PATH:-/}"

scale_int() {
  local base="$1"
  local pct="$2"
  local scaled=$(( base * pct / 100 ))
  if (( scaled < 1 )); then
    scaled=1
  fi
  echo "${scaled}"
}

scale_duration() {
  local duration="$1"
  local pct="$2"
  if [[ "${duration}" =~ ^([0-9]+)s$ ]]; then
    local seconds="${BASH_REMATCH[1]}"
    local scaled
    scaled="$(scale_int "${seconds}" "${pct}")"
    echo "${scaled}s"
    return
  fi
  echo "${duration}"
}

build_benchmark_args() {
  local mode="$1"
  local scale_pct="${2:-100}"

  local scaled_wrk_threads="${wrk_threads}"
  local scaled_wrk_connections="${wrk_connections}"
  local scaled_wrk_duration="${wrk_duration}"
  if (( scale_pct != 100 )); then
    scaled_wrk_threads="$(scale_int "${wrk_threads}" "${scale_pct}")"
    scaled_wrk_connections="$(scale_int "${wrk_connections}" "${scale_pct}")"
    scaled_wrk_duration="$(scale_duration "${wrk_duration}" "${scale_pct}")"
  fi

  local -a args=(
    --mode "${mode}"
    --threads "${server_threads}"
    --wrk-threads "${scaled_wrk_threads}"
    --wrk-connections "${scaled_wrk_connections}"
    --wrk-duration "${scaled_wrk_duration}"
    --path "${bench_path}"
  )

  printf '%s\0' "${args[@]}"
}

run_baseline() {
  local out_dir="${RESULT_ROOT}/baseline_${TS}"
  mkdir -p "${out_dir}"

  local -a args=()
  while IFS= read -r -d '' arg; do
    args+=("${arg}")
  done < <(build_benchmark_args "${bench_mode}" 100)

  "${BIN}" "${args[@]}" "$@" | tee "${out_dir}/benchmark_output.txt"
  echo "baseline output: ${out_dir}/benchmark_output.txt"
}

run_perf() {
  local out_dir="${RESULT_ROOT}/perf_${TS}"
  mkdir -p "${out_dir}"

  local -a args=()
  while IFS= read -r -d '' arg; do
    args+=("${arg}")
  done < <(build_benchmark_args "${bench_mode}" 100)

  perf record -F 99 -g -o "${out_dir}/perf.data" -- "${BIN}" "${args[@]}" "$@" \
    > "${out_dir}/benchmark_output.txt" 2>&1

  perf report -i "${out_dir}/perf.data" --stdio \
    > "${out_dir}/perf_report.txt" 2>&1 || true

  echo "perf data: ${out_dir}/perf.data"
  echo "perf report: ${out_dir}/perf_report.txt"
}

run_valgrind() {
  local tool="${VALGRIND_TOOL:-cachegrind}"
  local out_dir="${RESULT_ROOT}/valgrind_${TS}"
  local scale_pct="${ZHTTP_PERF_SCALE_PCT:-25}"
  mkdir -p "${out_dir}"

  local -a args=()
  while IFS= read -r -d '' arg; do
    args+=("${arg}")
  done < <(build_benchmark_args "${bench_mode}" "${scale_pct}")

  if [[ "${tool}" == "cachegrind" ]]; then
    valgrind --tool=cachegrind \
      --trace-children=yes \
      --trace-children-skip=wrk,/usr/bin/wrk \
      --child-silent-after-fork=yes \
      --cache-sim=yes \
      --branch-sim=yes \
      --cachegrind-out-file="${out_dir}/cachegrind.out.%p" \
      "${BIN}" "${args[@]}" "$@" > "${out_dir}/benchmark_output.txt" 2>&1

    local selected_file=""
    selected_file="$(
      find "${out_dir}" -maxdepth 1 -type f -name 'cachegrind.out.*' -printf '%s %p\n' \
        | sort -nr | head -n1 | cut -d' ' -f2-
    )"
    if [[ -n "${selected_file}" ]]; then
      cp "${selected_file}" "${out_dir}/cachegrind.out"
      cg_annotate --auto=yes "${selected_file}" > "${out_dir}/cachegrind_report.txt" 2>&1 || true
      echo "cachegrind selected: ${selected_file}"
      echo "cachegrind report: ${out_dir}/cachegrind_report.txt"
    fi
  else
    valgrind --tool="${tool}" \
      "${BIN}" "${args[@]}" "$@" > "${out_dir}/benchmark_output.txt" 2>&1
  fi

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
