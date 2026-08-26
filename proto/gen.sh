#!/usr/bin/env bash
# usage: gen.sh <src_dir> <build_dir> <name> [protoc]
# protoc 缺省取 PATH（手动重生成用）；CMake 调用时显式传入与 libprotobuf
# 同源的绝对路径，避免 conda/系统双 protoc 导致生成物与运行时不匹配。
set -euo pipefail
SRC_DIR="$1"
BUILD_DIR="$2"
NAME="$3"
PROTOC="${4:-protoc}"
mkdir -p "${BUILD_DIR}/gen"
"${PROTOC}" -I "${SRC_DIR}/proto" --cpp_out="${BUILD_DIR}/gen" "${SRC_DIR}/proto/${NAME}.proto"
