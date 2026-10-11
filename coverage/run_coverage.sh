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
  GCOV        Override gcov reader command (for example: "llvm-cov gcov")
  BUILD_JOBS  Number of concurrent build jobs (default: 4)
  CTEST_JOBS  Number of concurrent test programs (default: 4)
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
# Keep report layout deterministic and avoid stale flat reports from older runs.
find "${REPORT_ROOT}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=ON \
        -DZLYNX_ENABLE_COVERAGE=ON
fi

cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS:-4}"

test_exit=0
test_status_file="${REPORT_ROOT}/test-status.txt"
if [[ ${RUN_TESTS} -eq 1 ]]; then
    find "${BUILD_DIR}" -name '*.gcda' -delete
    if ctest --test-dir "${BUILD_DIR}" --output-on-failure -j"${CTEST_JOBS}"; then
        echo "initial_ctest_exit=0" >"${test_status_file}"
    else
        test_exit=$?
        echo "initial_ctest_exit=${test_exit}" >"${test_status_file}"
        echo "Initial ctest run failed, rerunning failed tests sequentially..."
        if ctest --test-dir "${BUILD_DIR}" --output-on-failure --rerun-failed -j1; then
            echo "rerun_ctest_exit=0" >>"${test_status_file}"
            echo "flaky=true" >>"${test_status_file}"
            echo "Rerun passed; the initial failure is retained (flaky tests)." >&2
        else
            rerun_exit=$?
            echo "rerun_ctest_exit=${rerun_exit}" >>"${test_status_file}"
            echo "flaky=false" >>"${test_status_file}"
            echo "Rerun also failed; generating coverage for diagnosis." >&2
        fi
    fi
else
    echo "tests=skipped" >"${test_status_file}"
fi

compiler="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n1)"
if [[ -z "${GCOV:-}" ]]; then
    if "${compiler}" --version | head -n1 | grep -qi clang; then
        llvm_cov="$("${compiler}" -print-prog-name=llvm-cov)"
        if ! command -v "${llvm_cov}" >/dev/null; then
            echo "Matching llvm-cov is unavailable; set GCOV explicitly." >&2
            exit 1
        fi
        GCOV="${llvm_cov} gcov"
    else
        GCOV="$("${compiler}" -print-prog-name=gcov)"
    fi
fi
{
    echo "generated_utc=$(date -u +%FT%TZ)"
    echo "source_commit=$(git -C "${ROOT_DIR}" rev-parse HEAD)"
    echo "source_status:"
    git -C "${ROOT_DIR}" status --short
    echo "compiler=${compiler}"
    "${compiler}" --version | head -n1
    gcovr --version
    echo "gcov_command=${GCOV}"
    echo "source_filter=<module>/(src|include)/"
    echo "excluded: tests, third-party, unreachable branches, throw branches, noncode lines"
    echo "zco/znet: logging expansion lines; znet: SSL_ERROR_ branch fanout"
} >"${REPORT_ROOT}/metadata.txt"

modules=(zlog zmalloc zco znet zhttp)

gcovr_common=(
    gcovr
    -j 1
    -r "${ROOT_DIR}" "${BUILD_DIR}"
    --gcov-executable "${GCOV}"
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
    module_extra_args=()
    module_source_filter="${module}/(src|include)/"

    case "${module}" in
    zco)
        # Ignore logging macro expansion branches.
        module_extra_args+=("--exclude-lines-by-pattern" ".*ZCO_LOG_.*")
        ;;
    znet)
        # Ignore logging macro expansion branches.
        module_extra_args+=("--exclude-lines-by-pattern" ".*ZNET_LOG_.*")
        # Ignore OpenSSL state-machine branch fanout from third-party codes.
        module_extra_args+=("--exclude-branches-by-pattern" ".*SSL_ERROR_.*")
        ;;
    esac

    "${gcovr_common[@]}" \
        "${module_extra_args[@]}" \
        --filter "${module_source_filter}" \
        --decisions \
        --json "${module_dir}/coverage.json" \
        --json-summary "${module_dir}/summary.json" \
        --txt-summary >"${summary_file}"

    gcovr -r "${ROOT_DIR}" --json-add-tracefile "${module_dir}/coverage.json" \
        --txt --txt-metric branch --sort uncovered-percent >"${branch_file}"
    gcovr -r "${ROOT_DIR}" --json-add-tracefile "${module_dir}/coverage.json" \
        --decisions --html-details "${html_file}" \
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
exit "${test_exit}"
