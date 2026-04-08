#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN="${BUILD_DIR}/zcoroutine/tests/stack_model_perf"
GTEST_FILTER="${GTEST_FILTER:-StackModelPerfStressTest.CoverSchedulerChannelTimerAndHookIoPerformance}"
MODE="${1:-baseline}"
OUT_ROOT="${ROOT_DIR}/zcoroutine/tests/stress/perf_results"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${OUT_ROOT}/${MODE}_${STAMP}"

mkdir -p "${OUT_DIR}"

if [[ ! -x "${BIN}" ]]; then
  echo "[error] test binary not found: ${BIN}"
  echo "[hint] build first: cmake --build build --target stack_model_perf"
  exit 1
fi

log() {
  echo "[stack-model-profiler] $*"
}

ORIGINAL_PARANOID=""
PARANOID_UPDATED=0
PERF_CMD=(perf)

restore_perf_paranoid() {
  if [[ ${PARANOID_UPDATED} -eq 1 && -n "${ORIGINAL_PARANOID}" ]]; then
    if command -v sudo >/dev/null 2>&1; then
      sudo sysctl -w "kernel.perf_event_paranoid=${ORIGINAL_PARANOID}" >/dev/null || true
      log "restored kernel.perf_event_paranoid=${ORIGINAL_PARANOID}"
    fi
  fi
}

prepare_perf_permissions() {
  if [[ -r /proc/sys/kernel/perf_event_paranoid ]]; then
    ORIGINAL_PARANOID="$(cat /proc/sys/kernel/perf_event_paranoid)"
  fi

  if [[ "${PREFER_SYSCTL:-1}" == "1" ]] && command -v sudo >/dev/null 2>&1; then
    if sudo -n true >/dev/null 2>&1; then
      if sudo sysctl -w kernel.perf_event_paranoid=-1 >/dev/null; then
        PARANOID_UPDATED=1
        log "temporarily set kernel.perf_event_paranoid=-1"
      fi
    fi
  fi

  if [[ ${PARANOID_UPDATED} -eq 0 ]]; then
    if [[ "${USE_SUDO_PERF:-0}" == "1" ]] && command -v sudo >/dev/null 2>&1; then
      PERF_CMD=(sudo perf)
      log "use sudo perf fallback"
    else
      PERF_CMD=(perf)
      log "run perf as current user"
    fi
  fi
}

run_baseline() {
  local out_file="${OUT_DIR}/baseline.log"
  log "running baseline"
  "${BIN}" --gtest_filter="${GTEST_FILTER}" | tee "${out_file}"
}

run_perf() {
  trap restore_perf_paranoid EXIT
  prepare_perf_permissions

  local perf_data="${OUT_DIR}/perf.data"
  local perf_report="${OUT_DIR}/perf_report.txt"
  local perf_stat="${OUT_DIR}/perf_stat.txt"
  local perf_run_log="${OUT_DIR}/perf_run.log"

  export ZCOROUTINE_PERF_SCALE_PCT="${ZCOROUTINE_PERF_SCALE_PCT:-100}"

  log "running perf record"
  "${PERF_CMD[@]}" record -F "${PERF_FREQ:-99}" -g -o "${perf_data}" -- \
    "${BIN}" --gtest_filter="${GTEST_FILTER}" | tee "${perf_run_log}"

  log "running perf stat"
  "${PERF_CMD[@]}" stat -o "${perf_stat}" \
    -e cache-references,cache-misses,cycles,instructions,branches,branch-misses \
    -- "${BIN}" --gtest_filter="${GTEST_FILTER}" >/dev/null 2>&1 || true

  log "generating perf report"
  "${PERF_CMD[@]}" report -i "${perf_data}" --stdio --sort symbol,dso > "${perf_report}" || true

  log "perf artifacts: ${OUT_DIR}"
}

run_valgrind() {
  local cache_out="${OUT_DIR}/cachegrind.out"
  local cache_txt="${OUT_DIR}/cachegrind.txt"
  local call_out="${OUT_DIR}/callgrind.out"
  local call_txt="${OUT_DIR}/callgrind.txt"
  local run_log="${OUT_DIR}/valgrind_run.log"

  export ZCOROUTINE_PERF_SCALE_PCT="${ZCOROUTINE_PERF_SCALE_PCT:-25}"

  log "running cachegrind (scale=${ZCOROUTINE_PERF_SCALE_PCT}%)"
  valgrind --tool=cachegrind \
    --cache-sim=yes --branch-sim=yes \
    --cachegrind-out-file="${cache_out}" \
    "${BIN}" --gtest_filter="${GTEST_FILTER}" | tee "${run_log}"

  cg_annotate "${cache_out}" > "${cache_txt}" || true

  log "running callgrind"
  valgrind --tool=callgrind \
    --dump-instr=yes --collect-jumps=yes \
    --callgrind-out-file="${call_out}" \
    "${BIN}" --gtest_filter="${GTEST_FILTER}" >/dev/null 2>&1 || true

  callgrind_annotate "${call_out}" > "${call_txt}" || true
  log "valgrind artifacts: ${OUT_DIR}"
}

summarize() {
  local summary="${OUT_DIR}/summary.txt"
  {
    echo "mode=${MODE}"
    echo "output_dir=${OUT_DIR}"
    echo "binary=${BIN}"
    echo "gtest_filter=${GTEST_FILTER}"
    echo "scale_pct=${ZCOROUTINE_PERF_SCALE_PCT:-100}"
    echo
    if [[ -f "${OUT_DIR}/baseline.log" ]]; then
      grep "^\[zcoroutine-perf\] scenario=" "${OUT_DIR}/baseline.log" || true
    fi
    if [[ -f "${OUT_DIR}/perf_run.log" ]]; then
      grep "^\[zcoroutine-perf\] scenario=" "${OUT_DIR}/perf_run.log" || true
    fi
    if [[ -f "${OUT_DIR}/valgrind_run.log" ]]; then
      grep "^\[zcoroutine-perf\] scenario=" "${OUT_DIR}/valgrind_run.log" || true
    fi
  } > "${summary}"

  log "summary written: ${summary}"
}

case "${MODE}" in
  baseline)
    run_baseline
    ;;
  perf)
    run_perf
    ;;
  valgrind)
    run_valgrind
    ;;
  *)
    echo "Usage: $0 [baseline|perf|valgrind]"
    exit 1
    ;;
esac

summarize
