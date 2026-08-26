#!/usr/bin/env bash
# 分层与语义约束。P1 CMake 每层独立 target，pipeline 不链 frontend（见根 CMakeLists）。
# 失效条件：下游自带鉴权头落地时，改为禁止 agent 侧 token、允许 backend 侧下游凭据。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0

grep_src() {
  grep -nE "$1" "$ROOT"/include/perfacet "$ROOT"/src -r --include='*.h' --include='*.cpp' || true
}

deny_include() {
  local pattern="$1"
  local scope="$2"
  local hits
  hits="$(grep -nE "$pattern" $scope -r --include='*.h' --include='*.cpp' 2>/dev/null || true)"
  if [[ -n "$hits" ]]; then
    echo "分层失败: $pattern"
    echo "$hits"
    fail=1
  fi
}

deny_include '#include.*"perfacet/frontend' "$ROOT/include/perfacet/ir $ROOT/src/ir"
deny_include '#include.*netlib/' "$ROOT/include/perfacet/ir $ROOT/src/ir"
deny_include '#include.*httplib' "$ROOT/include/perfacet/ir $ROOT/src/ir"
deny_include '#include.*yaml-cpp' "$ROOT/include/perfacet/ir $ROOT/src/ir"
deny_include '#include.*"perfacet/frontend' "$ROOT/include/perfacet/pipeline $ROOT/src/pipeline"
deny_include '#include.*"perfacet/policy' "$ROOT/include/perfacet/backend $ROOT/src/backend"
deny_include '#include.*"perfacet/frontend' "$ROOT/include/perfacet/backend $ROOT/src/backend"

if grep -nE '\bwho\.' "$ROOT"/src/backend/HttpMcpBackend.cpp >/dev/null 2>&1; then
  echo "Backend 禁止读 Principal (who.)"
  grep -nE '\bwho\.' "$ROOT"/src/backend/HttpMcpBackend.cpp || true
  fail=1
fi

# agent token：只允许 frontend/ 与 cli/；行内 PERFACET_LAYER_ALLOW 豁免
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  if echo "$line" | grep -q 'PERFACET_LAYER_ALLOW'; then
    continue
  fi
  echo "token 规则失败（只允许 frontend/ cli/）: $line"
  fail=1
done < <(grep -nE 'Authorization|Bearer |authenticate\(' "$ROOT"/include/perfacet "$ROOT"/src -r --include='*.h' --include='*.cpp' \
  | grep -v '/frontend/' | grep -v '/cli/' || true)

# 业务档位名不得出现在产品 C++ 中
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  echo "业务档位名出现在 src/include: $line"
  fail=1
done < <(grep -nE '\bintern\b|\bengineer\b|\bfinance-admin\b' "$ROOT"/include/perfacet "$ROOT"/src -r --include='*.h' --include='*.cpp' || true)

if [[ "$fail" -ne 0 ]]; then
  echo "check_layers: FAIL"
  exit 1
fi
echo "check_layers: OK"
exit 0
