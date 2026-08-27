# Perfacet 开发规格说明书（M1）

> 状态：可执行规格，不是设计随笔。实现以本文为准；产品判断以 `perfacet_project_scope(1).md` 为准。两处冲突时，以范围文档的产品不变量为准，以本文的接口/文件/错误码为准。
> 制定日期：2026-08-26
> 协议：**只实现 MCP `2026-07-28`**。不实现、不探测、不降级任何更早版本。
> 兼容性：**零向后兼容。** 旧 TwigRPC / agent 运行时 / mcpd 计划全部作废。

本文回答：一个工程师从空树开始，按哪些文件、哪些类型、哪条请求路径、哪组测试，在三周内把 `examples/demo.sh` 做绿。

---

## 0. 硬约束（写进 CI，不靠自觉）

1. **只认 `2026-07-28`。** 客户端 `MCP-Protocol-Version` 或 `_meta.io.modelcontextprotocol/protocolVersion` 不是该值 → HTTP 400 + `-32022`。缺头、缺 `_meta` 字段同样拒绝。禁止把缺头当成 `2025-03-26`。
2. **无 session。** 禁止出现 `Mcp-Session-Id`、`initialize`、`notifications/initialized`、`Last-Event-ID`。收到这些头或方法 → 当未知方法/未知头处理，不实现兼容分支。
3. **无 stdio 前端、无进程内 Stdio backend。** stdio MCP 必须在网关外用 `mcp-proxy` 转 HTTP 再登记。
4. **C++ 源码不出现用户业务档位名**（`intern` / `engineer` / `finance-admin` / `default` 只允许出现在 YAML、测试夹具、审计 `levelName` 字符串、示例）。判定只用 `ir::Rank`。
5. **只有 `frontend/` 碰 agent HTTP 与 agent token。** `ir/` 不 include 上层，不认 HTTP。
6. **虚接口 M1 恰好 5 个：** `Backend`、`Policy`、`Governor`、`Health`、`Tracer`。禁止再抽 `IdentityStore` / `GrantStore` / `TaskStore` / `Circuit` / `AuditLog` / `Executor` / `Promoter` / interceptor。
7. **IO loop 禁止：** `stat`/`open` grants 或 audit；`POST` OTLP；yaml-cpp 解析；cpp-httplib 调用。这些进 worker，完成必须 `EventLoop::queueInLoop` 回 loop 再碰 `Call` / `responded_`。
8. **本仓库 RPC 不是主语。** 禁止链 protobuf、禁止 `TWIGRPC_*` 环境变量、禁止 Prometheus/`/metrics`。
9. **单实例。** 不设计多副本、不把 `Permit` RAII 说成已为分布式预留。

---

## 1. 产品一句话（实现口径）

Perfacet 是 **Multi-Agent Tool Gateway**：agent 连一条 Streamable HTTP 入口；已有 MCP server 登记 HTTP endpoint；按身份切面；对共享工具做并发治理。

| 主语 | 看见的东西 |
|---|---|
| Agent | 普通 `2026-07-28` MCP server。`mcp.json` 一条 URL + Bearer。 |
| 上游 MCP | 普通 MCP client。网关不拉起、不重启、不沙箱。 |
| 管理员 | YAML + CLI Grant + `perfacet status` / `GET /upstreams`。 |

网关自身 **SPOF、单进程**。epoll 是工程便利，不是产品定义。

---

## 2. 仓库清洗（已执行，禁止回潮）

清洗原则：范围文档 §6.2「只借 EventLoop / TcpServer / Buffer / Endpoint，以及 TaskTree 的取消与 deadline、GOAWAY 排空」；**不能直接复用的旧树整棵删除**，语义写进新模块，不留适配层。

### 2.1 删除（禁止再引入）

| 路径 | 为什么删 |
|---|---|
| `rpc/` | 自研 24B 帧、codec、ConnPool、Balancer、Registry、Heartbeat、Collector、`/metrics`。产品主语是 MCP，不是 RPC。 |
| `proto/` | protobuf 契约。M1 JSON-RPC + nlohmann。 |
| `python/twigrpc/` | 旧 RPC Python 客户端。 |
| `agent/` | 旧 Runtime / IPolicy / IRetry / PolicyChain / AdmissionPolicy。与本产品 Policy/Governor 同名不同物。 |
| `registryd/` | 注册中心进程。 |
| `deploy/` | Prometheus / Grafana / docker-compose。范围明确不做。 |
| `examples/*.cpp`、旧 math/kv/agent_demo | RPC 示例。 |
| `benchmark/`（grpc_echo 对照等） | 旧 RPC 压测。bench 目录按 §16 重建。 |
| `archive/` | mcpd / TwigRPC 研究稿。会污染判断。 |
| `.env.example`、`.dockerignore` | `TWIGRPC_*` 配置与旧镜像。 |
| `netlib/TcpClient.{h,cpp}` | 范围：**不用 TcpClient 打上游**。上游用 cpp-httplib。 |

### 2.2 保留并约束用法

只保留 `netlib/` 作为 **agent 侧字节流服务器** 底座。禁止 netlib 出现 MCP / Rank / token 概念。

| 类型 | M1 用法 | 禁止 |
|---|---|---|
| `EventLoop` | 唯一 IO loop：accept、llhttp 读、定时器（probe / cooldown / promote）、`queueInLoop` | loop 上文件 I/O、httplib、OTLP POST |
| `TcpServer` | 听 `listen:`；`ioThreads=0`（**单 loop**，见 §5.1） | 为 MCP 再开 sub-reactor 分片业务状态 |
| `TcpConnection` + `Buffer` | HTTP/1.1 字节入缓冲；llhttp 消费 | 自研 HTTP 状态机 |
| `Endpoint` + `parseHostPort` | 解析 `host:port` | 自注册 advertise IP（`localIpFor` 本产品不用） |
| `ThreadPool` | worker 池：httplib、OTLP、audit append、grant `stat` | handler 直接在 IO 线程打上游 |
| `Acceptor` / `Socket` / `Channel` / `Epoll` | TcpServer 内部 | 业务直接操作 epoll |
| `Timer` / `runAfter` / `cancel` | promote、probe、circuit cooldown、queue wait | Grant TTL 判定（TTL 是读时 `now < expiresAt`） |

**从旧 TaskTree 迁过来的语义（不保留源码）：**

- deadline **只收紧**：`Request.deadlineMs = min(客户端超时, 网关上限)`；子操作不得放宽。
- 取消是协作的：`Cancelled` = 网关放弃等待，**不保证**远端终止（不变量 12）。
- `~Call()` 是唯一收尾点（对应旧 `complete`：无在途才摘除；这里是 Permit + InFlight + 定时器 + span 一起走）。

**从旧 GOAWAY 迁过来的语义（不保留帧类型）：**

```
stop():
  1. 拒新请求（HTTP 503 + JSON-RPC 应用错误，或直接断 accept）
  2. 队列中的 Governor wait → Throttled 出队
  3. 在途 Call 等到上限 drain_timeout_ms（默认 3000）
  4. 超时强制 ~Call（Cancelled），内存 task 随进程消失
```

HTTP 没有 GOAWAY 帧。对 agent 的信号是：**停 accept + 在途连接写完响应后 shutdown**。不要发明私有停机头。

### 2.3 符号清洗

全仓库禁止残留：`twigrpc`、`TWIGRPC_`、`Catch2` 测试开关、`IRetry`、`IPolicy`（旧 agent）、`MsgType::GOAWAY`、`Mcp-Session-Id` 实现。`netlib` 注释里若仍写 RpcClient，改为「上层」。

---

## 3. 协议落地（只跟 `2026-07-28`）

权威：

- Streamable HTTP：https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- Tasks 扩展 SEP-2663（码号以 **现行 schema `-32021`** 为准，不以 overview 旧稿 `-32003` 为准）
- 扩展名：`io.modelcontextprotocol/tasks`

### 3.1 传输

| 项 | M1 行为 |
|---|---|
| 入口 | 唯一 `POST /mcp` |
| 短调用 | `Content-Type: application/json` 一次 JSON-RPC 响应 |
| SSE | **仅当** 该请求必须流（M1 默认不流）。不做 `Last-Event-ID`，不做独立 GET 流 |
| 通知 POST | 核心协议 Streamable HTTP 无 client→server notification。收到 JSON-RPC notification → HTTP 400 |
| TLS | 不做 |
| Origin | 非浏览器 agent 常无 Origin。M1：无 Origin 放行；有 Origin 则必须在 yaml `http.origin_allowlist`（缺省 `["*"]` 仅用于内网演示）。DNS rebinding 不是本项目主语 |

客户端 `Accept` 必须含 `application/json`（规范还要求声明 `text/event-stream`）。缺 `application/json` → 406。

### 3.2 强制请求头（路由用头，不靠先解析 JSON 才知道方法）

| Header | 来源 | 何时必须 |
|---|---|---|
| `MCP-Protocol-Version` | 必须 = `2026-07-28`，且等于 body `_meta.io.modelcontextprotocol/protocolVersion` | 每个 POST |
| `Mcp-Method` | = JSON-RPC `method` | 每个请求 |
| `Mcp-Name` | `tools/call` → `params.name`；`tasks/*` → `params.taskId` | 见表 |
| `Authorization` | `Bearer <token>` | 每个 POST /mcp（`GET /healthz` 除外） |
| `traceparent` | W3C；可缺 | 可选 |

`Mcp-Name` 对 `tasks/get` / `tasks/cancel`：**等于 `taskId`**（范围文档）。与 `tools/call` 的工具名不是同一字段。

校验顺序（Frontend，失败不进 Pipeline）：

1. 缺 `MCP-Protocol-Version` 或值 ≠ `2026-07-28` → `-32022` HTTP 400，`data.supported` = `["2026-07-28"]`
2. 头与 body 版本不一致 → `-32020` HeaderMismatch HTTP 400
3. 缺 `Mcp-Method`，或 ≠ body `method` → `-32020` HTTP 400
4. 方法需要 `Mcp-Name` 时：缺、或与 body 对应字段不一致 → `-32020` HTTP 400
5. 未知方法 → `-32601` HTTP **404**（用 JSON-RPC 体区分「不是旧 SSE 端点的裸 404」）

`Mcp-Name` 若含非 ASCII，按规范 Base64 sentinel 解码后再与 body 比。M1 工具名约定 ASCII + `__`。

### 3.3 `_meta`（每请求自描述）

JSON-RPC request 的 `params._meta` **必须**含：

```json
{
  "io.modelcontextprotocol/protocolVersion": "2026-07-28",
  "io.modelcontextprotocol/clientCapabilities": {}
}
```

`clientInfo` 可选，进 span `client.name`。缺 `protocolVersion` 或 `clientCapabilities` → `-32602` HTTP 400。

Tasks 能力 **只**看：

```json
"io.modelcontextprotocol/clientCapabilities": {
  "extensions": {
    "io.modelcontextprotocol/tasks": {}
  }
}
```

`ClientCaps::tasks == true` 当且仅当该 key 存在。禁止用旧版 `params.task` 标志、禁止用 session 记忆「上次声明过」。不变量 19：只判本请求 `Request.caps.tasks`。

网关私有确认令牌（**不是** MRTR）：

```json
"_meta": {
  "perfacet/confirm": "if_<inflightId>"
}
```

造 `BackendCall` 时 **剥掉** `perfacet/confirm`，不转发上游、不进 tool schema。

### 3.4 本网关实现的方法

| method | M1 | 备注 |
|---|---|---|
| `server/discover` | 做 | 广告 `tools` + `extensions["io.modelcontextprotocol/tasks"]` |
| `tools/list` | 做 | 必填 `ttlMs` + `cacheScope: "private"` |
| `tools/call` | 做 | 切面内走 Pipeline；内置 `perfacet__*` 分流 |
| `tasks/get` | 做 | `Mcp-Name` = taskId；别人的 id → 失败 |
| `tasks/cancel` | 做 | 能转发上游则转发；否则标 Cancelled 停等 |
| `tasks/update` | **404 / -32601** | 整条不做 MRTR / `input_required` |
| `tasks/list` / `tasks/result` | **404 / -32601** | 规范已删 / 禁止阻塞等结果 |
| `initialize` 及一切 `2025-*` 方法 | **404 / -32601** | 不提供兼容说明超出版本列表以外的迁徙路径 |
| `resources/*` `prompts/*` `subscriptions/listen` | **404 / -32601** | M1 不做 |

`server/discover` 结果形状（最小）：

```json
{
  "protocolVersion": "2026-07-28",
  "capabilities": {
    "tools": { "listChanged": false },
    "extensions": {
      "io.modelcontextprotocol/tasks": {}
    }
  },
  "serverInfo": { "name": "perfacet", "version": "0.1.0" },
  "ttlMs": 60000,
  "cacheScope": "private"
}
```

`listChanged: false`：M1 不推 `notifications/tools/list_changed`。切面变化靠 `ttlMs`（Grant 剩余）让客户端重新 list。

### 3.5 `tools/list` 结果

```json
{
  "tools": [ { "name": "postgres__query", "description": "...", "inputSchema": {} } ],
  "ttlMs": 5000,
  "cacheScope": "private"
}
```

- `name` 对 agent 永远是 `ToolKey::str()`，即 `backend__tool`。
- `ttlMs = min(默认 list_ttl_ms, 该身份最短未过期 Grant 剩余 ms)`。无 Grant 用 yaml `tasks` 无关的默认，建议 **5000**。
- `cacheScope` **恒** `"private"`（不变量 5）。禁止 `"public"`。
- ToolIndex 冷（首轮 probe 未成功写入过）→ **不得**对合规客户端返回可长期缓存的空列表：listen 前等首轮 probe；若仍空，`ttlMs` 必须为 **0**（不变量 20）。

内置工具也出现在 list 里，规则见 §9。

### 3.6 `tools/call` 与 Tasks

同步成功：标准 `CallToolResult`（`content` / `isError`），`resultType` 缺省按规范 `"complete"`。

升格（已发出上游、promote 到点、且 `caps.tasks`）：

```json
{
  "resultType": "task",
  "taskId": "<id>",
  "status": "working",
  "createdAt": "<ISO-8601>",
  "lastUpdatedAt": "<ISO-8601>",
  "ttlMs": 3600000,
  "pollIntervalMs": 500
}
```

**SEP-2663 MUST：** 返回该对象前，`tasks/get` 用同一 `taskId` 必须能立刻命中（`MemTaskStore` insert 先于写 HTTP）。M1 知情缺口：进程内、不 fsync。面试讲 MUST 与边界，不假装 WAL。

未声明 tasks 且必须靠句柄才能结束本次 HTTP → **立刻** `-32021`，HTTP **400**：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32021,
    "message": "Missing required client capability",
    "data": {
      "requiredCapabilities": {
        "extensions": {
          "io.modelcontextprotocol/tasks": {}
        }
      }
    }
  }
}
```

禁止干等到客户端 timeout。禁止给未声明客户端任何 `taskId`。在途命中 **复用已有句柄** 不算「给新句柄」（不变量 19）。

任务状态机 M1：`working → completed | failed | cancelled`。永不进入 `input_required`。收到 `tasks/update` 当未知方法。

`tasks/get` 终态：`completed` 带原 `CallToolResult`；`failed` 带 JSON-RPC `error` 对象；`cancelled` 无 result。

### 3.7 错误码（实现表）

| 场景 | HTTP | JSON-RPC code | FailureClass |
|---|---|---|---|
| 无/未知 Bearer | 401 | 无 body 或 `-32000` 应用错误；**禁止写 token** | — |
| 版本不支持 / 缺版本 | 400 | `-32022` | Protocol |
| HeaderMismatch | 400 | `-32020` | Protocol |
| 缺 `_meta` 必填 | 400 | `-32602` | Protocol |
| 未知 method | 404 | `-32601` | Protocol |
| 未声明 tasks 且必须句柄 | 400 | `-32021` | Capability |
| Policy Deny（非 secret） | 200 | `CallToolResult.isError=true` 或 `-32603`；对外文案不得泄漏其它 backend | Authz |
| Policy Deny + secret 不够格 | 200 | **与拼错工具同一形状**（unknown） | Authz |
| Governor Reject / 排队超时 | 200 | `CallToolResult.isError=true` | Throttled |
| 在途去重未声明 tasks | 200 | `CallToolResult.isError=true` | Throttled |
| Circuit OPEN / Health DOWN | 200 | `CallToolResult.isError=true` | Unavailable |
| 上游超时 | 200 | isError | Timeout |
| 网关放弃等待 | 200 | isError | Cancelled |
| 停机拒新 | 503 | 应用错误 | Unavailable |

JSON-RPC `id` 必须回显请求 id。Header 阶段失败若 body 未解析出 id，用 `null`。

---

## 4. 架构与线程

### 4.1 分层

```
Agent  --Bearer + 可选 traceparent-->  POST /mcp
  Frontend     HTTP ↔ ir::Request     YamlIdentityStore + JsonlGrantStore
  Pipeline     authorize → inflight → admit → circuit → call → promote
  Backend      HttpMcpBackend(ir::BackendCall)  --httplib worker-->  MCP HTTP
控制面不在热路径编排：Catalog / ToolIndex / ProbeHealth / IndexRefresher
```

请求顺序是 Pipeline 里的六步，不是文档约定。Deny 不进 Governor、不进 InFlight（不变量 8）。

**可见性只由 Rank 决定，与 Health / Circuit 无关。** DOWN/OPEN 时切面内仍列出 last-known-good；call 才 Unavailable。secret 且不在切面的 server，list 与错误码都不得让低权限知道它存在。

### 4.2 线程（M1 钉死）

| 角色 | 数量 | 职责 |
|---|---|---|
| IO loop | **1** | llhttp、定时器、Pipeline 状态机、`responded_` 判定 |
| Worker 池 | `workers`（yaml，默认 4） | httplib client、OTLP POST、audit append、grant 文件 refresh |

**单 IO loop。** 旧 TcpServer 默认 `ioThreads=2` 是 RPC 多 reactor。MCP 网关的 Call/InFlight/Governor 队列是进程内共享状态，分到多 IO 线程会被迫加锁或投递。M1：`TcpServer(mainLoop, addr, /*ioThreads=*/0)` —— 若现实现 `0` 表示「只用 mainLoop、不建 subLoop」，按此改 netlib 适配；禁止为图省事维持 2 个 subLoop 再在连接间共享无锁假设。

worker 完成 **必须** `loop->queueInLoop(...)` 再读 `Call::responded_`。禁止 worker 直接写 HTTP 响应或改 Permit。

### 4.3 启动顺序

1. 加载 YAML，fail-closed（§8）。失败则进程退出码 ≠ 0，日志带名字。
2. `Catalog.add` 每个 backend → `HttpMcpBackend` + 已解析 `BackendMeta`。
3. 起 worker 池与 Health 定时器，**listen 前**跑完首轮 probe（同步等在 loop+worker 上）。
4. IndexRefresher 成功则写 ToolIndex；失败不抹（冷启动可能仍空：此时不得对客户端宣称长 ttl 空列表 —— 见不变量 20；可拒绝 listen 或 ttlMs=0，M1 选择：**至少完成一轮 probe 再 `TcpServer::start`**，即使全 DOWN 也 listen，list 用 last-known-good 或空+ttlMs=0）。
5. 校验已成功 list 的 backend 的 `governor.tools`；从未成功的标 `unvalidated`，**不阻塞 listen**。
6. accept。

### 4.4 停机

见 §2.2 GOAWAY 语义。`drain_timeout_ms` 放 yaml `listen` 旁，默认 3000。已转发的 cancel 仍是放弃等待。`MemTaskStore` 随进程消失。

---

## 5. IR（`include/perfacet/ir/`）

`ir` 是唯一跨层货币。禁止在 IR 留没人读的字段。禁止 IR include `frontend` / httplib / yaml-cpp。

```cpp
namespace perfacet::ir {

using Json = nlohmann::json;
using Rank = uint16_t;

struct ToolKey {
    std::string backend;
    std::string tool;
    static std::optional<ToolKey> parse(std::string_view);
    std::string str() const; // backend + "__" + tool
};

struct ClientCaps {
    bool tasks = false;
    bool has(std::string_view ext) const; // M1 只认 tasks 扩展名
};

struct Principal {
    std::string agentId;
    Rank        level = 0;
    Rank        grantBump = 0;
    bool        hasLevel = false; // 默认 false；仅 authenticate 成功才 true
    bool        admin = false;
    std::string levelName;        // 仅 span / 审计
};

struct BackendMeta {
    Rank level = 0;
    bool secret = false;
    std::vector<std::string> idempotentTools;
};

struct TraceContext {
    std::string traceId, spanId, parentSpanId;
};

enum class FailureClass {
    Ok, Cancelled, Timeout, Unavailable, Throttled,
    Protocol, Capability, Upstream, Authz, Internal
};

struct Request {
    std::string  method;      // Mcp-Method
    std::string  name;        // tools/call: ToolKey::str(); tasks/*: taskId
    std::string  upstreamId;  // JSON-RPC id 的字符串化
    Json         params;
    Json         meta;        // 可含 perfacet/confirm
    Principal    who;
    TraceContext trace;
    uint64_t     deadlineMs;  // 绝对 Unix ms
    ClientCaps   caps;
};

struct BackendCall {
    std::string  method, name; // name 是上游工具名（不含 backend__ 前缀）
    Json         params, meta; // 已剥 perfacet/confirm
    uint64_t     deadlineMs;
    TraceContext trace;
    // P1 起禁止带 Principal。M1 工期紧可暂传 Request，但 Backend 实现仍不得读 who
};

struct Response {
    std::string   upstreamId;
    Json          body;       // JSON-RPC result 或 error 对象
    bool          isError = false;
    FailureClass  klass = FailureClass::Ok;
    uint64_t      gatewayMs = 0;
    uint64_t      upstreamMs = 0;
};

FailureClass classify(const Response&, std::error_code);

inline Rank effectiveRank(const Principal& w) {
    return w.level > w.grantBump ? w.level : w.grantBump;
}

} // namespace perfacet::ir
```

### 5.1 `ToolKey::parse`

- 第一个 `"__"` 切开：`parse("a__b__c")` → `{backend: "a", tool: "b__c"}`。
- 无 `"__"` → `nullopt`（内置 `perfacet__*` 除外，见 §9：内置也用同一 parse，backend=`perfacet`）。
- backend 名加载期禁止含 `"__"`，违反则拒绝启动。

### 5.2 `classify`

输入：上游 HTTP 状态、JSON-RPC error.code、超时/取消 errno。输出 `FailureClass`。

| 条件 | klass |
|---|---|
| 成功 JSON-RPC result | Ok |
| 网关自己的 -32021 | Capability（Upstream 路径不应出现） |
| 连接失败 / 5xx / OPEN / DOWN | Unavailable |
| 读超时 | Timeout |
| JSON 不是 JSON-RPC | Protocol |
| 其它上游 error | Upstream |

**Backend 禁止私自 `retry=true`。** 是否重试只由 `RetryPolicy` 读 klass + yaml + `idempotent_tools` + 在途状态。

### 5.3 可见性

```
visible(who, mcp) = who.hasLevel && effectiveRank(who) >= mcp.level
```

热路径零字符串比较档位名。未知 level 名在加载期已拒绝，请求期类型上不出现。

---

## 6. 模块规格

路径均相对仓库根。头文件在 `include/perfacet/...`，实现在 `src/...`，与头同名。

### 6.1 `policy/` —— Taxonomy / YamlConfig / YamlIdentityStore / JsonlGrantStore / RankPolicy

**`Taxonomy`（具体类）**

- 加载 `access.levels` 数组，下标即 `Rank`（0..n-1）。全序链，不是格。
- `std::optional<Rank> parse(std::string_view name) const`
- 面试口径：finance-admin 排在 github 后面会顺带看见 github；升级偏序 = DAG，Principal/BackendMeta/Policy/测试全换。

**`YamlConfig`（具体类）**

一次加载整份 yaml。校验失败抛带名字的错误，main 打印后退出。加载规则：

1. `admin: true` 且省略 `level` → 该 agent `hasLevel=false`（优先，与档数无关）
2. `levels` 长度为 1 且省略 `level` → Rank 0，`hasLevel=true`
3. `levels` 长度 > 1 且省略 `level` → **拒绝启动**（纯 admin 除外，已由 1 覆盖）
4. 写了 `level` → 必须在 `levels` 内，否则拒绝启动
5. backend 省略 `level`：一档 → Rank 0；多档 → 拒绝启动
6. `secret` 缺省 false；`idempotent_tools` 缺省空
7. backend `name` 唯一、非空、不含 `"__"`

**`YamlIdentityStore`（具体类，M1 不抽接口）**

```cpp
class YamlIdentityStore {
public:
    explicit YamlIdentityStore(const YamlConfig&);
    // 失败：nullopt。禁止打日志写 token。
    std::optional<ir::Principal> authenticate(std::string_view bearer) const;
};
```

token → `{agentId, level, hasLevel, admin, levelName}`。`grantBump` 此处为 0，由 GrantStore 填。

**`JsonlGrantStore`（具体类）**

文件格式每行一个 JSON 对象（append-only）：

```json
{"id":"g_...","agent":"cursor","bump_to":"engineer","rank":1,"status":"pending|approved|denied","expiresAt":0,"ts_ms":0}
```

- CLI `approve` 写 `status=approved` 与 `expiresAt = now + elevation.ttl_ms`。
- worker 每 `grant_refresh_ms`（默认 100）`stat` + 若 mtime 变则解析，原子 `shared_ptr<const GrantTable>` swap。
- **loop 上** `Rank effectiveBump(agentId, nowMs) const`：只读快照 + `now < expiresAt`。禁止 syscall。
- 过期消失不依赖 reload（不变量 6）。
- `bump_to` 必须在 levels 且 Rank ≤ `elevation.max_level`；否则申请失败（工具 isError），不写文件。
- 纯 admin（`hasLevel==false`）调用 `perfacet__request_elevation` 失败（不变量 21）。

**`RankPolicy` : `Policy`**

```cpp
class Policy {
public:
    virtual ~Policy() = default;
    virtual Decision authorizeCall(const ir::Principal&, const ir::ToolKey&) = 0;
    virtual std::function<bool(const ir::ToolKey&)>
        visibilityFilter(const ir::Principal&) = 0;
};
enum class Decision { Allow, Deny, Unknown }; // Unknown = secret 不够格的对外形状
```

规则链（list 与 call 同一套；list 用 filter 一次编译）：

1. 内置 `perfacet__*`：不参与 Rank。见 §9。
2. 业务工具 + `hasLevel==false` → Deny
3. 业务工具 + `effectiveRank < mcp.level` → Deny；若 `secret` → Unknown
4. 否则 Allow

出厂一档仍走 Rank，没有旁路。Policy 答「有没有这把工具」；Governor 答「现在能不能再开一把」。

### 6.2 `catalog/` —— Catalog / ToolIndex / FacetView / IndexRefresher

**`Catalog`**

`name → {Backend*, BackendMeta}`。不带 Principal。热路径按 `ToolKey.backend` 查找。

**`ToolIndex`**

last-known-good：`backend → vector<{tool, description, inputSchema}>`。

- 只由 `IndexRefresher` 在 probe **成功**时写。
- Health **禁止**写。失败 **不抹**。
- DOWN / OPEN **不删**。

**`FacetView`**

```cpp
class FacetView {
public:
    FacetView(const ToolIndex&, Policy&);
    ir::Json listTools(const ir::Principal&) const; // 已应用 visibilityFilter + 内置工具
};
```

**`IndexRefresher`**

吃 `onProbeResult(server, state, optional<toolListJson>)`。成功解析 `tools/list` 则替换该 backend 条目，并触发该 backend 的 `governor.tools` 校验。间隔：`max(health.interval_ms, 上游 ttlMs)`，上游没 ttl 则只用 interval。

### 6.3 `backend/` —— Backend / HttpMcpBackend

```cpp
class Backend {
public:
    virtual ~Backend() = default;
    virtual void call(const ir::BackendCall&, std::function<void(ir::Response)>) = 0;
};
```

`HttpMcpBackend`：

- worker 上阻塞 `POST url`。传输是 `KeepAliveClient`：同一线程对同一 `host:port` 复用 httplib Client（keep-alive + TCP_NODELAY）。Client 非线程安全，禁止跨线程共享、禁止每次 `call` new Client。传输失败丢弃该槽，下次重建。connect 超时 2s；read/write 超时按本请求 deadline 收紧。
- 带 `MCP-Protocol-Version: 2026-07-28`、`Mcp-Method`、需要时 `Mcp-Name`（上游工具名，**无** `backend__` 前缀）、`traceparent`。
- 网关作为客户端：`_meta` 填自己的 `serverInfo` 当 clientInfo；`clientCapabilities` 对上游声明 tasks **当且仅当** 我们准备把上游 task 接住。M1：**不对上游声明 tasks**（不 re-attach `remoteTaskId`，割线后）。上游若因长调用返回 `-32021`，klass=Capability，对 agent 映射为 Unavailable/Upstream（不得把上游的 `-32021` 原样冒充本网关能力错误，除非本网关自己 promote 失败）。
- 写 `upstreamMs`。完成 `queueInLoop`。
- **不**自己重试、**不**自己熔断、**不**读 Principal。
- 取消：若本地有上游 `taskId` 则 `tasks/cancel`；否则停等并标 Cancelled。

### 6.4 `pipeline/` —— Pipeline / Call / InFlight

```cpp
class Pipeline {
public:
    void handle(ir::Request, std::function<void(ir::Response)> onDone);
};

class Call {
    Governor::Permit permit_;
    ir::TraceContext trace_;
    bool responded_ = false;
public:
    Call(Governor::Permit, ir::TraceContext);
    ~Call(); // Permit 归还 + InFlight 摘除 + 取消 promote 定时器 + Tracer::end
};
```

六步（必须按序，禁止 interceptor）：

```
1. visibilityFilter / authorizeCall
      Deny / Unknown → Authz，onDone，return
      （不变量 8：Governor/InFlight/Backend 计数仍为 0）
2. InFlight.lookup(agentId, ToolKey, paramsHash)
      命中 && caps.tasks → 返回已有 taskId（working），不进 Governor
      命中 && !tasks && meta 无匹配 confirm → Throttled + 人话 + if_* 令牌
      命中 && confirm == inflightId → 放行（真的再打）
3. Governor.acquire(..., waitDeadlineMs, onAdmit)
      Reject / 排队超时 → Throttled（不建 task）
4. CountCircuit：OPEN → Unavailable
5. 造 BackendCall（剥 confirm）→ Backend.call → arm promote → InFlight.insert
      上游先回 → 同步 JSON，cancel 定时器，responded_=true
6. promote 到点（仍 !responded_）
      caps.tasks → MemTaskStore 先 insert 再 CreateTaskResult；Call 可摘 HTTP；Permit 仍占槽直到上游回调
      !caps.tasks → 立刻 -32021 HTTP 400；Permit 持到上游回调
```

`promote_after_ms` **只在 Backend.call 已丢到 worker 之后** arm（不变量 22）。排队不建 task。DEGRADED 时 arm 那一刻 `promote_after_ms` **减半**（一行；可在注释标明 Week 3 后可删）。

`paramsHash`：`params.dump()`。nlohmann object 有序，确定性够用。不补「像不像」。只在同一 `agentId` 内去重。

确认令牌：`if_` + inflightId（不可猜测，用 128-bit 随机 hex）。绑 inflightId 不是 `force: true`。`~Call` 摘除即失效。

### 6.5 `govern/` —— Governor / LocalGovernor

```cpp
class Governor {
public:
    enum class Admit { Go, Queue, Reject };
    class Permit { // move-only；pImpl 持 Governor* + ToolKey；禁止空壳当「已持有」
        struct Impl;
        std::unique_ptr<Impl> impl_;
    public:
        Permit() = default; // 仅作为「未持有」；析构无操作
        Permit(Permit&&) noexcept;
        Permit& operator=(Permit&&) noexcept;
        Permit(const Permit&) = delete;
        ~Permit();          // 唯一归还路径；无公开 release
    };
    virtual ~Governor() = default;
    virtual void acquire(const ir::Principal&, const ir::ToolKey&,
                         uint64_t waitDeadlineMs,
                         std::function<void(Admit, Permit)> onAdmit) = 0;
};
```

`LocalGovernor`：

- per-tool：`governor.tools[ToolKey::str()].max_concurrency`，否则 `default.per_tool_concurrency`
- per-principal：`default.per_principal_concurrency`
- 超限 FIFO 队列；`queue_wait_ms` 内拿不到 → `Reject`（对外 Throttled）
- `rate_per_sec` **YAML 占位，M1 不实现**（读了忽略，debug 日志一次）
- 探测流量 **不占** 配额
- 实现必须能把 `permit_held` 与析构归还对账

`governor.tools` 校验：只对 **已成功 list** 的 backend。key 必须是合法 ToolKey 且 tool 在该 backend 快照内。不命中 → WARN + `perfacet status` 显示未生效，**不退出**。从未 list 成功 → `unvalidated`，不阻塞 listen。拼错不能静默当无限制。

### 6.6 `health/` —— ProbeHealth / CountCircuit / RetryPolicy

**`Health`**

```cpp
class Health {
public:
    enum class State { Up, Degraded, Down };
    virtual ~Health() = default;
    virtual State state(const std::string& server) const = 0;
    virtual uint64_t latencyEwmaMs(const std::string& server) const = 0;
};
```

`ProbeHealth`：interval 对上游发 `tools/list`（独立预算）。EWMA ≥ `degraded_latency_ms` → Degraded；连续 `down_after_failures` → Down。回调 `onProbeResult`。不写 ToolIndex。

**`CountCircuit`（具体类，不抽）**

连续失败 `open_after` → OPEN → `cooldown_ms` → HALF_OPEN（`half_open_probes` 次探测）。OPEN **不**改 FacetView。OPEN 期间业务 call 在 Pipeline 第 4 步失败，计入 span / `circuit_open++`，不再锤上游。探测走独立路径，可把 HALF_OPEN 探成功当关闭。

**`RetryPolicy`（具体类）**

读 yaml `retry`。仅当 klass ∈ `retryable` 且未过 `deadlineMs` 且电路非 OPEN。`never` 列表绝对不重试。额外：

- Timeout **必须** `tool ∈ idempotent_tools`
- **在途未完成禁止当可重试 Timeout**（上一枪还在跑，幂等前提不成立）

M1 Retry 细测可弱于 demo；接口必须先写对。

### 6.7 `task/` —— Task / MemTaskStore

```cpp
struct Task {
    std::string taskId;
    std::string agentId;      // 别人的 id 失败（不变量 7）
    ir::ToolKey key;
    std::string status;       // working|completed|failed|cancelled
    ir::Json    resultOrError;
    uint64_t    createdAtMs, lastUpdatedAtMs, ttlMs;
    // 进行中的 task 绑创建时的 Principal 快照，不随后续 Grant 变
    ir::Principal owner;
};

class MemTaskStore {
public:
    void insert(Task);                    // 先于 HTTP 写句柄
    std::optional<Task> get(std::string_view id) const;
    void update(std::string_view id, ...);
};
```

不 fsync、无 WAL、不抽接口。头文件一行：「持久化时抽 TaskStore + JSONL/SQLite 即满足 MUST。」

### 6.8 `observe/` —— Tracer / OtlpHttpJsonTracer / Counters

```cpp
class Tracer {
public:
    virtual ~Tracer() = default;
    virtual ir::TraceContext start(const ir::Request&, const char* spanName) = 0;
    virtual void set(const ir::TraceContext&, const char* key, const std::string& value) = 0;
    virtual void end(const ir::TraceContext&, ir::FailureClass, uint64_t latencyMs) = 0;
};
```

手写 OTLP/HTTP JSON，**禁止** opentelemetry-cpp。loop 只入队（`queue_max`，默认 1024）；worker POST `otel.endpoint`。满则丢，`otlp_dropped++`。Jaeger 不在不得拖死网关。

Span 字段：`trace_id` `span_id` `principal`(=agentId) `level`(=levelName) `tool` `server` `task_id` `inflight_id` `inflight_hit` `latency` `status`(FailureClass)。**禁止写 token。**

根 span 在 Pipeline 开。Policy Deny 也要短 span。`inflight_hit=true` 时 **无** 第二条 upstream span。

`Counters`（不抽）：原子量，热路径只 `fetch_add`。

| 名 | 类型 |
|---|---|
| `inflight_hit` | 累加 |
| `inflight_confirm` | 累加 |
| `throttled` | 累加 |
| `circuit_open` | 累加 |
| `otlp_dropped` | 累加 |
| `permit_held` | 瞬时 |
| `inflight_held` | 瞬时 |

无 `calls` 计数、无 Prometheus、无 `/metrics`。

### 6.9 `audit/` —— JsonlAuditLog

loop 投递一条 POD，worker append 一行。事件集合 **闭**：

`auth_fail` / `deny` / `throttled` / `inflight_hit` / `circuit_open` / `grant_approve` / `grant_expire`

字段 = observe 表 + `event` + `ts_ms`。禁止 token。头文件一行：「抽 AuditLog」。M1 不抽。

不变量 10：构造 Deny，审计桩有一条且含 `trace_id`，不启 HTTP 可测。

### 6.10 `frontend/` —— HttpMcp

职责 **仅** HTTP ↔ IR：

- llhttp 解析；读 token / `_meta` / `traceparent` / `Mcp-Method` / `Mcp-Name`
- `authenticate` + `effectiveBump` 填 Principal
- `Pipeline::handle`；把 `ir::Response` 写成 HTTP
- `GET /healthz`：进程活着即可，**不含**各 MCP 明细
- `GET /upstreams`：要求 `admin` Bearer；JSON 含每台上游 state/latency + `observe:` 计数块
- **禁止**在 Frontend 调 Policy / Governor / Circuit / Backend / InFlight / promote
- **禁止**直接写 audit / OTLP

无 token / 未知 token → 401，审计 `auth_fail`（token 不得入日志，只记 `reason=missing|unknown`）。

### 6.11 `cli/` —— `perfacet` 单二进制

用 CLI11。子命令：

| 命令 | 行为 |
|---|---|
| `perfacet serve -c <yaml>` | 前台跑网关（默认入口） |
| `perfacet grant approve --id <grantId>` | append 同一 `grants.jsonl` 为 approved |
| `perfacet grant approve --agent <id> --bump <level>` | 直接批（仍受 max_level） |
| `perfacet status` | HTTP GET `/upstreams`，用 yaml 里某个 `admin: true` 的 token（CLI 读 yaml，**仍不算** frontend 热路径） |

最高档不能自动批自己的提权：CLI 假定操作员是人。M1 不把 CLI 做成第二个 Policy。

无 `perfacet reload`、无 `perfacet trace show`。

---

## 7. 内置工具

命名空间 `perfacet__`。不参与 Rank，但 **参与 list 可见性**（避免 intern 看见 admin 工具）。

| 工具 | list 可见 | call |
|---|---|---|
| `perfacet__request_elevation` | 已认证即可见 | `hasLevel==false` → 失败；否则校验 `bump_to`，同步写 pending，返回 `{grantId}`（**不是** task） |
| `perfacet__upstream_status` | 仅 `admin==true` | 返回各 server Health 快照 |
| 其它 `perfacet__*` | 仅 admin | 仅 admin；未知名 Unknown |

`arguments`：

```json
{ "bump_to": "engineer" }
```

成功：

```json
{ "content": [{ "type": "text", "text": "{\"grantId\":\"g_...\",\"status\":\"pending\"}" }] }
```

（也允许 structured 字段；demo 用 JSON 文本即可。）

---

## 8. 配置 schema

两个示例必须能启动：`examples/perfacet.yaml`（一档）、`examples/perfacet.multi-level.yaml`（剧本 1–6）。

完整键（未出现的用默认）：

```yaml
listen: "0.0.0.0:8741"
workers: 4
drain_timeout_ms: 3000
grants_path: "grants.jsonl"
grant_refresh_ms: 100
list_ttl_ms: 5000

http:
  origin_allowlist: ["*"]

access:
  levels: [intern, engineer, finance-admin]
  elevation:
    max_level: engineer
    ttl_ms: 900000

agents:
  cursor: { token: "pf_cursor_intern", level: intern }
  ops-admin: { token: "pf_admin", admin: true }

backends:
  - name: postgres
    url: "http://127.0.0.1:9002/mcp"
    level: intern
    secret: false
    idempotent_tools: [explain]

health:
  interval_ms: 5000
  degraded_latency_ms: 1000
  down_after_failures: 3

circuit:
  open_after: 5
  cooldown_ms: 30000
  half_open_probes: 1

retry:
  max_attempts: 2
  retryable: [Timeout, Unavailable, Upstream]
  never: [Authz, Cancelled, Protocol, Throttled, Capability]

governor:
  default:
    per_tool_concurrency: 4
    per_principal_concurrency: 10
    queue_wait_ms: 5000
    rate_per_sec: 20          # M1 忽略
  tools:
    postgres__query:
      max_concurrency: 3
      queue_wait_ms: 8000

tasks:
  promote_after_ms: 2000
  ttl_ms: 3600000

otel:
  endpoint: "http://127.0.0.1:4318/v1/traces"
  service_name: perfacet
  queue_max: 1024

audit:
  path: "audit.jsonl"
```

M1 **无热加载**。改 YAML 必须重启。

出厂文件 `examples/perfacet.yaml`：`levels: [default]`，agents/backends **都省略 level**。

---

## 9. HTTP 以外的进程形状

单个可执行文件 `perfacet`：

```
src/main.cpp          # CLI11 分发
src/frontend/...
...
```

链接：P1 起每层独立 CMake target + `PRIVATE`。M1 允许先一个 `libperfacet` 静态库，但 **include 方向** 从第 1 周就由 `scripts/check_layers.sh` 强制。

### 9.1 目录（实现必须按此创建，禁止再发明 `mcpd/` `twigrpc/`）

```
perfacet/
  SPEC.md
  perfacet_project_scope(1).md
  CMakeLists.txt
  README.md
  include/perfacet/
    ir/          Request.h ToolKey.h ClientCaps.h classify.h
    pipeline/    Pipeline.h Call.h InFlight.h
    catalog/     Catalog.h ToolIndex.h FacetView.h IndexRefresher.h
    govern/      Governor.h LocalGovernor.h
    policy/      YamlIdentityStore.h Taxonomy.h Policy.h JsonlGrantStore.h YamlConfig.h
    health/      Health.h CountCircuit.h RetryPolicy.h ProbeHealth.h
    backend/     Backend.h HttpMcpBackend.h
    task/        Task.h MemTaskStore.h
    observe/     Tracer.h OtlpHttpJsonTracer.h Counters.h
    frontend/    HttpMcp.h
    audit/       JsonlAuditLog.h
    cli/         main_cli.h
  src/           与 include 镜像
  netlib/        保留的 IO 底座（无 TcpClient）
  third_party/   llhttp, nlohmann, cpp-httplib, CLI11, doctest
  scripts/check_layers.sh
examples/
  perfacet.yaml
  perfacet.multi-level.yaml
  demo.sh
  mock_mcp.py
tests/
  invariants/
  ir/
  governor/
  inflight/
  observe/
bench/           割线后；M1 可空
```

### 9.2 依赖（自己不写的）

| 库 | 用途 |
|---|---|
| llhttp | agent HTTP/1.1 |
| nlohmann/json | JSON-RPC |
| yaml-cpp | 配置（系统包或 third_party） |
| cpp-httplib | 上游阻塞 client（KeepAliveClient 线程内复用） |
| CLI11 | CLI |
| doctest | 单测 |
| Python 3 + aiohttp 或 FastAPI | `mock_mcp.py` |

禁止：opentelemetry-cpp、simdjson、protobuf、Catch2、自研 HTTP 解析器、自研上游异步 client。

### 9.3 `scripts/check_layers.sh`

M1 即进 CI（本地 `cmake` 后 `ctest` 调该脚本，或作为自定义 target）。

**include 方向（必须失败）：**

- `ir/*` include `frontend` / `netlib` / `httplib` / `yaml-cpp`
- `pipeline` include `frontend`
- `backend` include `policy` / `frontend` / `Principal` 使用（grep `who.` 于 HttpMcpBackend.cpp）

**语义 grep（`*.h` `*.cpp`，行内 `PERFACET_LAYER_ALLOW` 豁免）：**

- agent token：`Authorization` / `Bearer` / `authenticate(` 只允许 `frontend/` 与 `cli/`
- 业务档位名：`intern` `engineer` `finance-admin` 禁止出现在 `src/` `include/`（测试与 examples 除外）

失效条件写在脚本注释：下游自带鉴权头落地时，改为禁止 agent 侧 token、允许 backend 侧下游凭据。

P1：拆 `perfacet_ir` / `perfacet_pipeline` / `perfacet_frontend` 等 target，`pipeline` `PRIVATE` 不链 `frontend`。

---

## 10. 追踪与审计字段

W3C `traceparent`：有则续；无则网关生成 32 hex traceId + 16 hex spanId。下游 `HttpMcpBackend` 带出同一 `traceparent`。禁止私有 trace 头作为唯一方案。

`JsonlAuditLog` 一行示例：

```json
{"ts_ms":0,"event":"deny","trace_id":"...","principal":"cursor","level":"intern","tool":"payroll__x","server":"","status":"Authz"}
```

demo 验收：`github__search` 的 `trace_id` 在 Jaeger 与 `audit.jsonl` 对得上；在途旁路能 grep `event=inflight_hit`。

---

## 11. Python mock 与 demo

`examples/mock_mcp.py`：

```
python mock_mcp.py --port 9001 --tools echo
python mock_mcp.py --port 9002 --tools query,explain
python mock_mcp.py --port 9005 --fail-after 5
python mock_mcp.py --port 9006 --delay 30
```

必须实现：`2026-07-28` 头校验、`tools/list`（ttlMs+cacheScope）、`tools/call`。`--delay` 在 call 里 sleep。`--fail-after N` 第 N 次 list/call 起失败。可选 `--declare-tasks` 用于升格旁路镜头。

`examples/demo.sh`：CI，7 步编号 0–6 + 旁路（在途去重、slow `-32021`）。必须与范围文档 §3.3 一致。Grant 批准后 **sleep 0.2** 再 list。

剧本 4：四个带 level 的身份打 `postgres__query`（max=3）：第四个 FIFO 排队，到点 Throttled，**不升格 task**。

---

## 12. 测试规格

框架：doctest + CTest。不启 HTTP 能测的，禁止去启 HTTP。

| 目录 | 必须覆盖 |
|---|---|
| `tests/ir/` | `ToolKey` `a__b__c`；`classify` 表 |
| `tests/invariants/` | §3.7 全表能自动化的条目；**不变量 8、10、23 纯内存** |
| `tests/inflight/` | 同 params 合流；带令牌才第二条；Deny 不进表；跨 agent 不合流 |
| `tests/governor/` | 4 抢 3 FIFO；Permit 析构归还；排队超时 Throttled |
| `tests/observe/` | Deny 写审计含 `trace_id`；禁止 token 字符串出现在审计行 |
| `tests/policy/` | 一档全集；secret unknown；纯 admin 无业务；Grant 过期读时生效 |
| `examples/demo.sh` | 第 3 周末必须绿 |

泄漏对账：混合终态（同步回 / 句柄 / -32021 / Throttled / Deny）循环后 `permit_held==0 && inflight_held==0`。M1 可用 10⁴ 量级；范围里 10⁵ 可在割线后。

yaml fail-closed：拼错 level、多档缺引用、backend 名含 `__` → 启动失败测试（可 `YamlConfig` 单测，不启 socket）。

---

## 13. 三周实现清单（文件级，按周交付）

原则：每周结束必须有可演示的垂直切片，而不是横向铺接口。

### 第 1 周 —— 切面可 list

**交付：** 两个 Bearer 一档 `tools/list` 都是全集；拼错 level 拒绝启动；冷启动不空缓存；不变量 8 纯内存；loop 上 Grant 零 syscall（可用测试桩断言 refresh 不在 loop 线程）。

| 顺序 | 产出 |
|---|---|
| 1 | `third_party` 接入；CMake 出 `perfacet` 可执行文件空壳 `serve` 听端口 |
| 2 | `ir/ToolKey.*` `ClientCaps.*` `classify.*` + `tests/ir` |
| 3 | `YamlConfig` `Taxonomy` fail-closed + 单测 |
| 4 | `YamlIdentityStore` `JsonlGrantStore`（worker refresh + 无锁读） |
| 5 | `RankPolicy` + `visibilityFilter` |
| 6 | `Catalog` `ToolIndex` `FacetView` |
| 7 | `ProbeHealth` + `IndexRefresher`；listen 前首轮 probe |
| 8 | `HttpMcp`：llhttp + `server/discover` + `tools/list`（尚无 call 治理） |
| 9 | `scripts/check_layers.sh` + CTest 挂钩 |
| 10 | `examples/perfacet.yaml` + mock echo；手动两 token list 一致 |
| 11 | 不变量 8：Pipeline 骨架只接 Policy，Governor/InFlight/Backend 桩计数 |

`gatewayMs` / `upstreamMs` 本周开始打点（list 的 upstreamMs 为 probe 缓存，call 下周）。

### 第 2 周 —— call / task / 去重 / Grant

**交付：** slow 声明 tasks → 句柄；不声明 → `-32021`；同 params 再打合流；带令牌才第二条；剧本 3 Grant。

| 顺序 | 产出 |
|---|---|
| 1 | `HttpMcpBackend` httplib + worker + `traceparent` |
| 2 | `Pipeline` 六步骨架 + `Call` 析构契约 |
| 3 | `MemTaskStore` + promote 定时器 + `-32021` |
| 4 | `InFlight` + confirm 令牌 + 计数 |
| 5 | 内置 `perfacet__request_elevation` + CLI `grant approve` |
| 6 | `tasks/get` `tasks/cancel` |
| 7 | `tests/inflight` + Grant ttlMs 缩短 |

### 第 3 周 —— 治理 + 观测 + demo 绿

**交付：** `demo.sh` 0–6 绿 + 在途旁路；audit 对 `trace_id`；`inflight_hit≥1`；flaky DOWN 网关仍起；4 抢 3；Permit/InFlight 归零。

| 顺序 | 产出 |
|---|---|
| 1 | `LocalGovernor` + Permit pImpl |
| 2 | `CountCircuit` + OPEN 不改 list |
| 3 | `OtlpHttpJsonTracer` 有界队列 + `JsonlAuditLog` + `Counters` |
| 4 | `GET /upstreams` `perfacet status` |
| 5 | `examples/perfacet.multi-level.yaml` `demo.sh` `mock_mcp.py` 全旗标 |
| 6 | `tests/invariants` 补全；泄漏对账 |

**P1（不挡 demo）：** `BackendCall` 替换暂传的 Request；CMake per-module target；token grep 白名单按脚本注释落地。

**明确不做（本规格禁止开分支）：** 范围文档 §7 表。另加：不恢复已删的 rpc/agent/proto/python/deploy。

---

## 14. 不变量检查表（实现者打印贴显示器）

从范围文档 §3.7 逐条落到代码位置：

| # | 落点 |
|---|---|
| 1 | Pipeline 第 1 步无「skip policy」分支 |
| 2 | list=`visibilityFilter`，call=`authorizeCall`，同一 Policy 实例 |
| 3 | 一档 + hasLevel → FacetView 等于 ToolIndex ∪ 内置 |
| 4 | secret Deny 走 Unknown 形状 |
| 5 | list 写死 `cacheScope=private` |
| 6 | `JsonlGrantStore::effectiveBump` 只比较 now；ttlMs=min |
| 7 | `MemTaskStore::get` 校验 `task.agentId==who.agentId` |
| 8 | 测试：Deny 后桩计数 0 |
| 9 | FacetView 不读 Health/Circuit |
| 10 | 列出的 7 类事件都能对 `trace_id`；worker append |
| 11 | RetryPolicy 两道门 |
| 12 | Cancelled 文案与 span 不得写「killed」 |
| 13–16 | YamlConfig |
| 17 | IndexRefresher 成功回调里校验 governor.tools |
| 18 | Permit 无 `release` |
| 19 | promote / inflight 复用句柄 |
| 20 | listen 前 probe；空 list 则 ttlMs=0 |
| 21 | RankPolicy 内置 elevation |
| 22 | arm 定时器在 `Backend.call` 之后 |
| 23 | InFlight 在 acquire 之前 |
| 24 | Audit/OTLP 队列 API 断言 `!loop->inLoopThread()` 于写文件/POST 点 |

---

## 15. 扩展性验收（改什么 / 不许改）

与范围文档 §6.4 相同，实现时用它做 code review 提问单。新增一条仓库级：

| 新增 | 允许改 | 不许改 |
|---|---|---|
| 把旧 RPC 加回来当 Backend | 禁止。RPC 不是主语 | 整个 `rpc/` 树不准复活 |

---

## 16. 性能目标（M1 打点，数字可割线后）

| 指标 | 目标 |
|---|---|
| 网关附加延迟 p99（`total - upstream`，本机 mock） | < 1ms |
| 单进程吞吐 | 5–10k RPS（割线后 bench） |
| Governor 4 抢 3 | FIFO，计数可观察 |
| Permit / InFlight | 混合终态后归零 |

OTLP / Audit 必须先离开热路径，再谈延迟。

---

## 17. 简历口径（实现完成后对照，禁止超前写入未做能力）

只允许写范围文档 §9 那段。禁止写：网络库、又一个 MCP 网关、自研 Docker、分布式网关、TwigRPC、3.1× gRPC。

---

## 附录 A. 旧代码语义对照（防误复用）

| 旧符号 | 新落点 | 不要做的事 |
|---|---|---|
| `agent::TaskTree` | `Call` + `MemTaskStore` | 不要树、不要 RPC requestId |
| `agent::IPolicy` / Admission | `RankPolicy` + `LocalGovernor` | 不要插件链 |
| `agent::IRetry` | `RetryPolicy` 具体类 | 不要虚接口 |
| `twigrpc::RpcServer::stop` GOAWAY | Frontend drain | 不要 GOAWAY 帧 |
| `TcpClient` | **删除**；上游 httplib | 不要在 loop 上非阻塞连上游充异步 |
| `Collector` / Grafana | `Counters` + OTLP + JSONL | 不要 `/metrics` |
| mcpd `StdioBackend` | 网关外 mcp-proxy | 不要 fork MCP |
| mcpd 多扩展点「为了分布式」 | 5 个虚接口 + 头文件一行 | 不要为没写的实现先画接口 |

## 附录 B. Agent 接入样例（演示用，写入 README）

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

客户端必须是会 `2026-07-28` 的官方 SDK v2 / 已升级 Cursor / Claude Code。不会该版本的客户端 **不接**，不提供双栈。
