#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-cov-all}"
REPORT_ROOT="${REPORT_ROOT:-${ROOT_DIR}/coverage/reports}"
RUN_TESTS=1
CTEST_JOBS="${CTEST_JOBS:-4}"

usage() {
    cat <<'EOF'
Usage:
  coverage/run_coverage.sh [--no-test] [--build-dir <dir>] [--report-dir <dir>]

Environment variables:
  BUILD_DIR   Override coverage build directory (default: ./build-cov-all)
  REPORT_ROOT Override report output directory (default: ./coverage/reports)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --no-test)
        RUN_TESTS=0
        shift
        ;;
    --build-dir)
        BUILD_DIR="$2"
        shift 2
        ;;
    --report-dir)
        REPORT_ROOT="$2"
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

mkdir -p "${REPORT_ROOT}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_TESTS=ON \
        -DENABLE_COVERAGE=ON
fi

cmake --build "${BUILD_DIR}" -j"$(nproc)"

if [[ ${RUN_TESTS} -eq 1 ]]; then
    find "${BUILD_DIR}" -name '*.gcda' -delete
    if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure -j"${CTEST_JOBS}"; then
        echo "Initial ctest run failed, rerunning failed tests sequentially..."
        ctest --test-dir "${BUILD_DIR}" --output-on-failure --rerun-failed -j1
    fi
fi

modules=(zlog zmalloc zco znet zhttp)

gcovr_common=(
    gcovr
    -j 1
    -r "${ROOT_DIR}" "${BUILD_DIR}"
    --merge-lines
    --exclude '/usr/.*'
    --exclude '.*/(third_party|3rdparty|external|_deps)/.*'
    --exclude '.*CMakeFiles/.*'
    --exclude-unreachable-branches
    --exclude-throw-branches
    --exclude-noncode-lines
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file
)

summary_table="${REPORT_ROOT}/summary.md"
{
    echo "| module | lines | functions | branches | decisions | summary | html |"
    echo "|---|---:|---:|---:|---:|---|---|"
} >"${summary_table}"

for module in "${modules[@]}"; do
    module_dir="${REPORT_ROOT}/${module}"
    mkdir -p "${module_dir}"

    summary_file="${module_dir}/summary.txt"
    branch_file="${module_dir}/branches.txt"
    html_file="${module_dir}/index.html"

    "${gcovr_common[@]}" \
        --filter "${module}/src" \
        --decisions \
        --txt-summary >"${summary_file}"

    "${gcovr_common[@]}" \
        --filter "${module}/src" \
        --txt \
        --txt-metric branch \
        --sort uncovered-percent >"${branch_file}"

    "${gcovr_common[@]}" \
        --filter "${module}/src" \
        --decisions \
        --html-details "${html_file}" \
        --html-title "${module} coverage" >/dev/null

    line_cov="$(grep '^lines:' "${summary_file}" | awk '{print $2}')"
    func_cov="$(grep '^functions:' "${summary_file}" | awk '{print $2}')"
    branch_cov="$(grep '^branches:' "${summary_file}" | awk '{print $2}')"
    decision_cov="$(grep '^decisions:' "${summary_file}" | awk '{print $2}')"

    {
        echo "| ${module} | ${line_cov} | ${func_cov} | ${branch_cov} | ${decision_cov} | ${summary_file} | ${html_file} |"
    } >>"${summary_table}"
done

echo "Coverage reports generated under: ${REPORT_ROOT}"
echo "Summary table: ${summary_table}"
