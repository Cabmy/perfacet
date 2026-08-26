#!/usr/bin/env bash
# 生成 gRPC C++ 桩代码（用 anaconda 工具链：protoc 29.3 + grpc_cpp_plugin 1.71）
set -euo pipefail
cd "$(dirname "$0")"
GRPC_ROOT="${GRPC_ROOT:-/home/cabmy/anaconda3}"
mkdir -p gen
"$GRPC_ROOT/bin/protoc" -I . --cpp_out=gen --grpc_out=gen \
  --plugin=protoc-gen-grpc="$GRPC_ROOT/bin/grpc_cpp_plugin" echo.proto
echo "generated: $(ls gen/)"
