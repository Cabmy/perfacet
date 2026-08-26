#!/usr/bin/env bash
# 再生成 Python 客户端桩（python/twigrpc/stubs/*_pb2.py 是 protoc 产物）。
# C++ 生成物由 CMake 构建期经 gen.sh 产出（在 build 目录内），此处只管 Python 侧。
# 多 protoc 环境下用 PROTOC=... 显式指定，避免 PATH 解析到不同版本。
set -euo pipefail
cd "$(dirname "$0")/.."
PROTOC="${PROTOC:-protoc}"
"${PROTOC}" -I proto --python_out=python/twigrpc/stubs \
  proto/rpc.proto proto/math.proto proto/registry.proto
