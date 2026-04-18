#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../.." && pwd)"
build_dir="${1:-${repo_root}/build-cov}"

line_target="${ZNET_COVERAGE_LINE_TARGET:-100}"
function_target="${ZNET_COVERAGE_FUNCTION_TARGET:-100}"
decision_target="${ZNET_COVERAGE_DECISION_TARGET:-95}"
branch_target="${ZNET_COVERAGE_BRANCH_TARGET:-80}"

ctest_timeout="${ZNET_COVERAGE_CTEST_TIMEOUT:-120}"
test_regex="${ZNET_COVERAGE_TEST_REGEX:-^(acceptor_unit|address_unit|buffer_unit|server_unit|socket_unit|tcp_connection_unit|tcp_server_unit|tls_context_unit|znet_logger_unit)$}"
seeds=(101 202 303 404 505)

gcovr_common=(
  gcovr
  -j 1
  --root "${repo_root}"
  --filter '^znet/src/'
  --object-directory "${build_dir}"
  --exclude '^.*/(usr|extern|third_party|_deps)/.*'
  --exclude-unreachable-branches
  --exclude-throw-branches
  --exclude-noncode-lines
  --gcov-ignore-errors=all
  --gcov-ignore-parse-errors=negative_hits.warn_once_per_file
)

extract_metric() {
  local key="$1"
  local file="$2"
  grep -Eo "\"${key}\":[[:space:]]*[0-9]+(\\.[0-9]+)?" "${file}" | tail -n1 |
    sed -E 's/.*:[[:space:]]*//'
}

sum_values() {
  local left="$1"
  local right="$2"
  awk -v lhs="${left}" -v rhs="${right}" 'BEGIN { printf "%.4f", lhs + rhs }'
}

avg_values() {
  local total="$1"
  local count="$2"
  awk -v sum="${total}" -v n="${count}" 'BEGIN { printf "%.2f", sum / n }'
}

ge_target() {
  local value="$1"
  local target="$2"
  awk -v v="${value}" -v t="${target}" 'BEGIN { exit(v + 0 >= t + 0 ? 0 : 1) }'
}

line_sum="0"
function_sum="0"
decision_sum="0"
branch_sum="0"

echo "Coverage scope: znet/src (excluding system/third-party paths)"
echo "Tests regex: ${test_regex}"
echo "Seeds: ${seeds[*]}"
echo "Targets: line>=${line_target}, function>=${function_target}, decision>=${decision_target}, branch>=${branch_target}"
echo "CTest timeout: ${ctest_timeout}s"

for seed in "${seeds[@]}"; do
  echo ""
  echo "[seed ${seed}] running tests..."

  find "${build_dir}" -name '*.gcda' -delete
  find "${build_dir}" -name '*.gcov' -delete

  GTEST_SHUFFLE=1 GTEST_RANDOM_SEED="${seed}" \
    ctest --test-dir "${build_dir}" -R "${test_regex}" --schedule-random \
      --timeout "${ctest_timeout}" --output-on-failure

  summary_file="${build_dir}/znet_coverage_summary_seed_${seed}.json"
  find "${build_dir}" -name '*.gcov' -delete
  "${gcovr_common[@]}" --decisions --json-summary "${summary_file}" >/dev/null

  line_value="$(extract_metric line_percent "${summary_file}")"
  function_value="$(extract_metric function_percent "${summary_file}")"
  decision_value="$(extract_metric decision_percent "${summary_file}")"
  branch_value="$(extract_metric branch_percent "${summary_file}")"

  echo "[seed ${seed}] line=${line_value}% function=${function_value}% decision=${decision_value}% branch=${branch_value}%"

  line_sum="$(sum_values "${line_sum}" "${line_value}")"
  function_sum="$(sum_values "${function_sum}" "${function_value}")"
  decision_sum="$(sum_values "${decision_sum}" "${decision_value}")"
  branch_sum="$(sum_values "${branch_sum}" "${branch_value}")"
done

count="${#seeds[@]}"
line_avg="$(avg_values "${line_sum}" "${count}")"
function_avg="$(avg_values "${function_sum}" "${count}")"
decision_avg="$(avg_values "${decision_sum}" "${count}")"
branch_avg="$(avg_values "${branch_sum}" "${count}")"

echo ""
echo "Average (5 runs): line=${line_avg}% function=${function_avg}% decision=${decision_avg}% branch=${branch_avg}%"

failed=0
if ! ge_target "${line_avg}" "${line_target}"; then
  echo "FAIL: line average ${line_avg}% < target ${line_target}%"
  failed=1
fi
if ! ge_target "${function_avg}" "${function_target}"; then
  echo "FAIL: function average ${function_avg}% < target ${function_target}%"
  failed=1
fi
if ! ge_target "${decision_avg}" "${decision_target}"; then
  echo "FAIL: decision average ${decision_avg}% < target ${decision_target}%"
  failed=1
fi
if ! ge_target "${branch_avg}" "${branch_target}"; then
  echo "FAIL: branch average ${branch_avg}% < target ${branch_target}%"
  failed=1
fi

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "PASS: all average coverage targets satisfied."
