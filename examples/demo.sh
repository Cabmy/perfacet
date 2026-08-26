#!/usr/bin/env bash
# Perfacet demo：剧本 0–6 + 在途去重旁路。Python 一律 uv run（仓库根 pyproject.toml）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${PERFACET_BIN:-$ROOT/build/perfacet}"

if [[ ! -x "$BIN" ]]; then
  echo "缺少 $BIN，请先 cmake --build build -j" >&2
  exit 1
fi

uv sync --quiet

PIDS=()
cleanup() {
  fuser -k 8741/tcp 9001/tcp 9002/tcp 9003/tcp 9004/tcp 9005/tcp 9006/tcp >/dev/null 2>&1 || true
  PIDS=()
}
trap cleanup EXIT

mcp() { uv run python "$ROOT/examples/mcp_call.py" "$@"; }

names_of() {
  echo "$1" | uv run python -c "import json,sys; r=json.load(sys.stdin); print(' '.join(t['name'] for t in r.get('result',{}).get('tools',[])))"
}

wait_port() {
  local p="$1"
  for _ in $(seq 1 50); do
    if (echo >/dev/tcp/127.0.0.1/"$p") 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "端口 $p 未就绪" >&2
  return 1
}

wait_port_free() {
  local p="$1"
  for _ in $(seq 1 50); do
    if ! (echo >/dev/tcp/127.0.0.1/"$p") 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "端口 $p 仍被占用" >&2
  return 1
}

mcp() { uv run python "$ROOT/examples/mcp_call.py" "$@"; }

names_of() {
  echo "$1" | uv run python -c "import json,sys; r=json.load(sys.stdin); print(' '.join(t['name'] for t in r.get('result',{}).get('tools',[])))"
}

wait_port() {
  local p="$1"
  for _ in $(seq 1 50); do
    if (echo >/dev/tcp/127.0.0.1/"$p") 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "端口 $p 未就绪" >&2
  return 1
}

wait_health() {
  for _ in $(seq 1 50); do
    if curl -sf http://127.0.0.1:8741/healthz | grep -q ok; then return 0; fi
    sleep 0.1
  done
  echo "网关未就绪" >&2
  return 1
}

echo "== 0 出厂一档：两 token 的 tools/list 都是全集"
rm -f "$ROOT/grants.jsonl" "$ROOT/audit.jsonl"
uv run python "$ROOT/examples/mock_mcp.py" --port 9001 --tools echo &
PIDS+=($!)
wait_port 9001
"$BIN" serve -c "$ROOT/examples/perfacet.yaml" &
PIDS+=($!)
wait_health
L1="$(mcp --token pf_cursor --method tools/list)"
L2="$(mcp --token pf_claude --method tools/list)"
N1="$(names_of "$L1")"
N2="$(names_of "$L2")"
echo "cursor: $N1"
echo "claude: $N2"
echo "$N1" | grep -q "echo__echo"
test "$N1" = "$N2"
cleanup
wait_port_free 8741
sleep 0.3

echo "== 1 multi-level：intern 看不见 payroll / github"
uv run python "$ROOT/examples/mock_mcp.py" --port 9002 --tools query,explain --delay 12 &
PIDS+=($!)
uv run python "$ROOT/examples/mock_mcp.py" --port 9003 --tools search &
PIDS+=($!)
uv run python "$ROOT/examples/mock_mcp.py" --port 9004 --tools run_payroll &
PIDS+=($!)
uv run python "$ROOT/examples/mock_mcp.py" --port 9005 --tools ping --fail-after 5 &
PIDS+=($!)
uv run python "$ROOT/examples/mock_mcp.py" --port 9006 --tools sleep --delay 30 &
PIDS+=($!)
wait_port 9002
wait_port 9003
wait_port 9004
wait_port 9005
wait_port 9006
"$BIN" serve -c "$ROOT/examples/perfacet.multi-level.yaml" &
PIDS+=($!)
wait_health
LI="$(mcp --token pf_cursor_intern --method tools/list)"
NI="$(names_of "$LI")"
echo "intern list: $NI"
echo "$NI" | grep -q "postgres__query"
if echo "$NI" | grep -q "payroll"; then echo "intern 不应看见 payroll"; exit 1; fi
if echo "$NI" | grep -q "github"; then echo "intern 不应看见 github"; exit 1; fi

echo "== 2 intern 猜 payroll__x 与拼错同错"
E1="$(mcp --token pf_cursor_intern --method tools/call --name payroll__x)"
E2="$(mcp --token pf_cursor_intern --method tools/call --name nosuch__x)"
T1="$(echo "$E1" | uv run python -c "import json,sys; print(json.load(sys.stdin)['result']['content'][0]['text'])")"
T2="$(echo "$E2" | uv run python -c "import json,sys; print(json.load(sys.stdin)['result']['content'][0]['text'])")"
echo "payroll: $T1"
echo "typo:    $T2"
test "$T1" = "$T2"

echo "== 3 Grant：申请 → 批准 → sleep 0.2 → list 出现 github"
EV="$(mcp --token pf_cursor_intern --method tools/call --name perfacet__request_elevation --args '{"bump_to":"engineer"}')"
GID="$(echo "$EV" | uv run python -c "import json,sys; t=json.load(sys.stdin)['result']['content'][0]['text']; print(json.loads(t)['grantId'])")"
echo "grantId=$GID"
"$BIN" grant approve -c "$ROOT/examples/perfacet.multi-level.yaml" --id "$GID"
sleep 0.2
L3="$(mcp --token pf_cursor_intern --method tools/list)"
N3="$(names_of "$L3")"
echo "after grant: $N3"
echo "$N3" | grep -q "github__search"
TTL="$(echo "$L3" | uv run python -c "import json,sys; print(int(json.load(sys.stdin)['result']['ttlMs']))")"
# 有 Grant 时 ttlMs = min(list_ttl_ms, 剩余)；剩余 15min 时仍为 5000
test "$TTL" -le 5000
echo "ttlMs=$TTL"

echo "== 4 四个身份打 postgres__query（max=3），第四个 Throttled"
mcp --token pf_cursor_intern --method tools/call --name postgres__query >/tmp/pf_q1.json &
mcp --token pf_claude_eng --method tools/call --name postgres__query >/tmp/pf_q2.json &
mcp --token pf_researcher --method tools/call --name postgres__query >/tmp/pf_q3.json &
sleep 0.4
R4="$(mcp --token pf_intern_bot --method tools/call --name postgres__query)"
echo "$R4" | uv run python -c "import json,sys; r=json.load(sys.stdin); assert r['result']['isError'] is True; print(r['result']['content'][0]['text'])"

echo "== 5 flaky：list 仍在，call Unavailable；payroll 仍 unknown"
for i in $(seq 1 8); do
  mcp --token pf_cursor_intern --method tools/call --name flaky__ping --args "{\"n\":$i}" >/dev/null || true
done
LF="$(mcp --token pf_cursor_intern --method tools/list)"
echo "$LF" | grep -q "flaky__ping"
CF="$(mcp --token pf_cursor_intern --method tools/call --name flaky__ping --args '{"n":99}')"
echo "$CF" | uv run python -c "import json,sys; r=json.load(sys.stdin); t=r['result']['content'][0]['text']; assert r['result']['isError'] is True; print(t)"
if echo "$LF" | grep -q payroll; then echo "intern list 泄漏 payroll"; exit 1; fi

echo "== 6 github__search 审计对得上 trace_id"
GOUT="$(mcp --token pf_claude_eng --method tools/call --name github__search)"
echo "$GOUT" | uv run python -c "import json,sys; r=json.load(sys.stdin); assert r.get('result',{}).get('isError', False) is False"
sleep 0.3
"$BIN" status -c "$ROOT/examples/perfacet.multi-level.yaml" | uv run python -c "import json,sys; j=json.load(sys.stdin); print(j['observe']); assert 'inflight_hit' in j['observe']"

echo "== 旁路：slow 不声明 tasks → -32021"
SLOW="$(mcp --token pf_researcher --method tools/call --name slow__sleep)"
echo "$SLOW" | uv run python -c "import json,sys; r=json.load(sys.stdin); assert r['error']['code']==-32021; print('got -32021')"

echo "== 旁路：在途去重（slow 仍在途，同 params 再打）"
H2="$(mcp --token pf_researcher --method tools/call --name slow__sleep)"
echo "$H2" | uv run python -c "import json,sys; r=json.load(sys.stdin); t=r.get('result',{}).get('content',[{}])[0].get('text',''); assert r.get('result',{}).get('isError') is True; assert 'if_' in t; print(t[:240])"
sleep 0.3
grep -q 'inflight_hit' audit.jsonl
"$BIN" status -c "$ROOT/examples/perfacet.multi-level.yaml"

echo "demo.sh 0-6 完成"
