#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j
"${BUILD_DIR}/stgs_benchmark_decode" --frames 200000 --payload-size 64 --decoder-threads 4
"${BUILD_DIR}/stgs_benchmark_decode" --frames 200000 --payload-size 256 --decoder-threads 4
"${BUILD_DIR}/stgs_benchmark_decode" --frames 200000 --payload-size 1024 --decoder-threads 4
