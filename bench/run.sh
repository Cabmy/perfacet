#!/usr/bin/env bash
# 附加延迟看 in-process（total − upstream）；RPS 看本机 HTTP + mock。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${PERFACET_BIN:-$ROOT/build/perfacet}"
BENCH="${PERFACET_BENCH_BIN:-$ROOT/build/bench/perfacet_bench}"
if [[ ! -x "$BIN" ]]; then
  echo "缺少 $BIN" >&2
  exit 1
fi
if [[ ! -x "$BENCH" ]]; then
  echo "缺少 $BENCH，请 cmake --build build -j" >&2
  exit 1
fi

echo "== in-process 附加延迟"
"$BENCH"

uv sync --quiet
cleanup() {
  fuser -k 8741/tcp 9001/tcp >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup

echo "== HTTP RPS（mock echo）"
uv run python "$ROOT/examples/mock_mcp.py" --port 9001 --tools echo &
for _ in $(seq 1 50); do
  if (echo >/dev/tcp/127.0.0.1/9001) 2>/dev/null; then break; fi
  sleep 0.1
done
"$BIN" serve -c "$ROOT/examples/perfacet.yaml" &
for _ in $(seq 1 50); do
  if curl -sf http://127.0.0.1:8741/healthz | grep -q ok; then break; fi
  sleep 0.1
done
uv run python "$ROOT/bench/http_bench.py" -n 8000 -c 32
