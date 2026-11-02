#!/usr/bin/env bash
# Local smoke test: C++ tests, STGF replay and output comparison.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build}"

for binary in stgs_tests stgs_ground_station stgs_satellite_simulator; do
    if [[ ! -x "${BUILD_DIR}/${binary}" ]]; then
        echo "Missing ${BUILD_DIR}/${binary}; build the project first." >&2
        exit 2
    fi
done

workdir="$(mktemp -d)"
trap 'rm -rf "${workdir}"' EXIT

printf '%s\n' '[1/4] C++ unit and loopback integration tests'
"${BUILD_DIR}/stgs_tests" | tail -1

printf '%s\n' '[2/4] Generate 1000 source frames (STGF)'
"${BUILD_DIR}/stgs_satellite_simulator" --output-file "${workdir}/demo.stgf" --count 1000 --rate 0 --seed 424242 --no-color >"${workdir}/simulator.log" 2>&1

printf '%s\n' '[3/4] Replay with 1 and 8 decoder workers'
"${BUILD_DIR}/stgs_ground_station" --replay "${workdir}/demo.stgf" --decoder-threads 1 --output "${workdir}/one.csv" --no-color >"${workdir}/one.log" 2>&1
"${BUILD_DIR}/stgs_ground_station" --replay "${workdir}/demo.stgf" --decoder-threads 8 --output "${workdir}/eight.csv" --no-color >"${workdir}/eight.log" 2>&1
cmp "${workdir}/one.csv" "${workdir}/eight.csv"
grep 'summary received=1000 decoded=1000 rejected=0 written=1000' "${workdir}/eight.log" | sed -E 's/ output=.*$//'

printf '%s\n' '[4/4] Deterministic CSV comparison: PASS'
if command -v python3 >/dev/null 2>&1; then
    "${BUILD_DIR}/stgs_ground_station" --replay "${workdir}/demo.stgf" --decoder-threads 4 --output "${workdir}/four.json" --no-color >"${workdir}/four.log" 2>&1
    python3 -m json.tool "${workdir}/four.json" >/dev/null
    echo 'JSON parse: PASS'
else
    echo 'JSON parse: SKIPPED (optional python3 not available)'
fi
