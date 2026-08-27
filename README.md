# Perfacet

多 Agent 共用一条 MCP 入口，**一个身份，一个切面**。

已有 MCP server 登记 HTTP 地址即可接入。不同机器上的 Agent 连同一条 Streamable HTTP；看见的是按身份裁过的目录，不是同一份权限。网关不拉起、不保活上游——它管的是谁能看见什么、现在能不能再开一把。

对 Agent 来说，它就是一个普通的 MCP `2026-07-28` server：一条 URL + Bearer。

## 特色

**按身份切面。** 同一张目录、同一条 URL。出厂一档，全员看见全集；加档位只改 YAML，C++ 零改。不够格的工具对低权限不可见；标了 `secret` 的上游，宕机也不会暴露给不该知道的人。

**Grant 提权。** Agent 申请、人用 CLI 审批、到期自动收回。不改配置、不重启。

**共享工具当资源管。** 按工具、按身份限并发，超限 FIFO 排队。多 Agent 抢同一把查询时，第四个等，而不是一起打穿上游。

**重试不放大。** 同一身份、同一参数的在途调用合流或一次确认。Agent 超时后再打，不会把还在跑的慢查询叠成两条。

**长调用拆连接。** 上游还没完时按 MCP Tasks 升格；客户端不会 Tasks，就明确拒绝，而不是干等到超时。

**一条调用链可对账。** OpenTelemetry trace、审计 JSONL、`perfacet status` 计数共用 `trace_id`。探活与熔断挡住坏调用，切面里的目录仍是 last-known-good。

## Agent

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

客户端必须支持 MCP `2026-07-28`。stdio 上游在网关外用 `mcp-proxy` 转成 HTTP 再登记。

## 运行

```bash
uv sync
cmake -B build && cmake --build build -j
./build/perfacet serve -c examples/perfacet.yaml
```

加档、提权、限流见 `examples/perfacet.multi-level.yaml`：

```bash
./build/perfacet grant approve -c examples/perfacet.multi-level.yaml --id g_...
./build/perfacet status -c examples/perfacet.multi-level.yaml
bash examples/demo.sh
```
