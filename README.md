# Perfacet

**Multi-Agent Tool Gateway**（C++17，MCP `2026-07-28`）：一个身份，一个切面。

已有 MCP server 登记 HTTP endpoint 即可接入。不同机器上的 agent 连同一条 Streamable HTTP 入口。共享的是目录投影，不是权限——权限层始终在。

- 产品范围：[`perfacet_project_scope(1).md`](perfacet_project_scope(1).md)
- 开发规格：[`SPEC.md`](SPEC.md)

本仓库 **不实现** 旧版 MCP。epoll 是工程便利，不是产品定义。

## 构建

C++ 依赖由 CMake FetchContent 拉进 `third_party/_fetch`（llhttp / nlohmann / yaml-cpp / cpp-httplib / CLI11 / doctest）。Python mock **只**用仓库根目录的 [uv](https://docs.astral.sh/uv/) 环境，不要另起 venv 或 Docker。

```bash
uv sync
cmake -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

可执行文件：`build/perfacet`。

```bash
./build/perfacet serve -c examples/perfacet.yaml
./build/perfacet grant approve -c examples/perfacet.multi-level.yaml --id g_...
./build/perfacet status -c examples/perfacet.multi-level.yaml
```

演示剧本（需本机 `curl`）：

```bash
bash examples/demo.sh
```

## 对 agent

客户端必须会 `2026-07-28`。不会该版本的不接。

```json
{
  "mcpServers": {
    "perfacet": {
      "type": "http",
      "url": "http://127.0.0.1:8741/mcp",
      "headers": { "Authorization": "Bearer pf_cursor" }
    }
  }
}
```
