# Perfacet 代码导读：从字节到上游

> 这份文档按**真实执行顺序**讲整棵仓库。所有带路径的链接都可以 Ctrl/Cmd+点击跳到源码（`#L` 是行号）。
>
> 产品规格以 [SPEC.md](SPEC.md) 为准；本文只解释「代码怎么走」。交互式架构图见 [archify/README.md](archify/README.md)。

**一句话：** Perfacet 是 Multi-Agent MCP Tool Gateway。Agent 连一条 `POST /mcp`；网关按身份切目录、治理并发、把调用转到已登记的上游 HTTP MCP。epoll [`netlib/`](../netlib/include/netlib/EventLoop.h) 是工程底座，不是产品主语。

---

## 目录

1. [怎么读这份仓库](#1-怎么读这份仓库)
2. [目录与分层](#2-目录与分层)
3. [进程与线程](#3-进程与线程)
4. [启动：从 main 到 listen](#4-启动从-main-到-listen)
5. [入口：HTTP → IR](#5-入口http--ir)
6. [Pipeline 方法分流](#6-pipeline-方法分流)
7. [tools/call 六步（热路径）](#7-toolscall-六步热路径)
8. [Call 生命周期](#8-call-生命周期)
9. [控制面：目录 / 探活 / Grant](#9-控制面目录--探活--grant)
10. [停机](#10-停机)
11. [netlib 底座](#11-netlib-底座)
12. [建议阅读顺序](#12-建议阅读顺序)
13. [文件索引](#13-文件索引)

---

## 1. 怎么读这份仓库

先记住三层边界，再往下钻函数：

```
Agent  ──Bearer──►  Frontend (HTTP ↔ IR)  ──ir::Request──►  Pipeline
                                                              │
                                              authorize → inflight → admit
                                              → circuit → Backend.call → promote
                                                              │
上游 MCP  ◄──httplib worker──  HttpMcpBackend  ◄──ir::BackendCall──┘
```

| 层 | 允许做什么 | 禁止做什么 |
|---|---|---|
| [`frontend/`](../include/perfacet/frontend/HttpMcp.h) | 解析 HTTP、鉴权、组 `ir::Request`、写 HTTP 响应 | 调 Policy / Governor / Circuit / Backend |
| [`pipeline/`](../include/perfacet/pipeline/Pipeline.h) | 编排六步、升格 Task、审计/trace | include frontend；自己打 HTTP |
| [`backend/`](../include/perfacet/backend/HttpMcpBackend.h) | worker 上 POST 上游 | 读 Principal、自己重试、自己熔断 |

include 方向由 [`scripts/check_layers.sh`](../scripts/check_layers.sh) 强制：`ir` 不碰 HTTP；`pipeline` 不碰 `frontend`；`backend` 不碰 `policy` / `who.`。

跨层唯一货币是 [`ir::Request`](../include/perfacet/ir/Request.h#L61) / [`ir::BackendCall`](../include/perfacet/ir/Request.h#L78) / [`ir::Response`](../include/perfacet/ir/Request.h#L85)。IR 不认 token、不 include netlib。

---

## 2. 目录与分层

CMake 按层拆静态库，[`pipeline` 故意不链 `frontend`](../CMakeLists.txt#L128-L135)：

```
perfacet/
  include/perfacet/     头文件，与 src/ 镜像
  src/                  实现
  netlib/               Agent 侧字节流服务器（无 MCP 概念）
  examples/             YAML、mock 上游、demo.sh
  tests/                doctest，按模块分子目录
  bench/                压测（割线后）
```

| CMake target | 源 | 职责 |
|---|---|---|
| `perfacet_ir` | [`src/ir/`](../src/ir/ToolKey.cpp) | ToolKey、FailureClass、JSON-RPC 形状 |
| `perfacet_policy` | [`src/policy/`](../src/policy/YamlConfig.cpp) | YAML、身份、Grant、档位全序 |
| `perfacet_catalog` | [`src/catalog/`](../src/catalog/Catalog.cpp) + RankPolicy | 上游登记、切面、目录快照 |
| `perfacet_backend` | [`src/backend/`](../src/backend/HttpMcpBackend.cpp) | httplib + keep-alive |
| `perfacet_govern` | [`src/govern/`](../src/govern/LocalGovernor.cpp) | 并发许可 FIFO |
| `perfacet_health` | [`src/health/`](../src/health/ProbeHealth.cpp) | 探活、熔断、重试门 |
| `perfacet_pipeline` | [`src/pipeline/`](../src/pipeline/Pipeline.cpp) | 请求编排 |
| `perfacet_frontend` | [`src/frontend/`](../src/frontend/HttpMcp.cpp) | llhttp + 路由 |
| `perfacet_cli` | [`src/cli/`](../src/cli/Gateway.cpp) | 组装 + 子命令 |
| `netlib` | [`netlib/`](../netlib/include/netlib/EventLoop.h) | EventLoop / TcpServer |

五个虚接口（M1 恰好这些，不再抽）：[`Backend`](../include/perfacet/backend/Backend.h)、[`Policy`](../include/perfacet/policy/Policy.h)、[`Governor`](../include/perfacet/govern/Governor.h)、[`Health`](../include/perfacet/health/Health.h)、[`Tracer`](../include/perfacet/observe/Tracer.h)。

---

## 3. 进程与线程

单进程、单 IO loop、一组 worker。共享状态（Call / InFlight / Governor 队列）都活在 loop 线程上，所以 [`TcpServer(..., ioThreads=0)`](../src/frontend/HttpMcp.cpp#L177) —— 连接不进 sub-reactor。

```
┌──────────── IO loop（唯一）─────────────┐
│  accept / llhttp / Pipeline 状态机      │
│  定时器：probe / grant refresh / promote │
│  Permit、InFlight、responded_           │
│  queueInLoop 回来后再写 HTTP            │
└──────────────┬──────────────────────────┘
               │ pool_->add
┌──────────────▼──────────────────────────┐
│  ThreadPool（yaml workers，默认 4）      │
│  httplib POST 上游 / OTLP / audit 写盘  │
│  Grant 文件 stat+parse                  │
└─────────────────────────────────────────┘
```

| 角色 | 代码 | 规则 |
|---|---|---|
| IO loop | [`EventLoop::loop`](../netlib/src/EventLoop.cpp#L43) | 禁止 `stat`/`open` grants、禁止 httplib、禁止 yaml 解析、禁止 OTLP POST |
| Worker | [`ThreadPool`](../netlib/include/netlib/ThreadPool.h) | 阻塞 IO；完成必须 [`queueInLoop`](../netlib/include/netlib/EventLoop.h#L46) 回 loop |
| 上游回调 | [`HttpMcpBackend::call`](../src/backend/HttpMcpBackend.cpp#L127) | worker 跑 `callBlocking`，再投回 loop 调 `onUpstream` |

---

## 4. 启动：从 main 到 listen

```
main
  → runCli
    → runServe
      → YamlConfig::load          fail-closed，坏配置直接退出
      → EventLoop + Gateway 构造   把所有依赖焊在一起
      → Gateway::start             先 probe，再 listen
      → loop.loop()                阻塞直到 SIGINT / drain
```

### 4.1 CLI 入口

- [`src/cli/main.cpp`](../src/cli/main.cpp) 只有一行：调 [`runCli`](../include/perfacet/cli/main_cli.h#L5)。
- [`runCli`](../src/cli/Cli.cpp#L129) 用 CLI11 分发三个子命令：`serve` / `grant approve` / `status`。
- [`runServe`](../src/cli/Cli.cpp#L28)：加载 YAML → 建 loop 与 Gateway → 挂 SIGINT/SIGTERM → [`gw.start()`](../src/cli/Gateway.cpp#L60) → [`loop.loop()`](../netlib/src/EventLoop.cpp#L43)。

配置键见 [`YamlConfig`](../include/perfacet/policy/YamlConfig.h#L35)，加载在 [`YamlConfig::load`](../src/policy/YamlConfig.cpp#L20)。拼错档位名、多档缺 `level`、backend 名含 `__` 都会抛，进程非 0 退出。出厂配置：[`examples/perfacet.yaml`](../examples/perfacet.yaml)。

### 4.2 Gateway 组装（依赖注入的唯一地点）

构造函数 [`Gateway::Gateway`](../src/cli/Gateway.cpp#L9) 按成员声明顺序把整机焊上：

1. Worker 池、身份表、Grant 文件。
2. [`RankPolicy(catalog_)`](../src/cli/Gateway.cpp#L14) + [`FacetView(index_, policy_)`](../src/cli/Gateway.cpp#L14) —— list 与 call 共用同一 Policy 实例。
3. Governor / Circuit / Retry / TaskStore / InFlight / Tracer / Audit。
4. [`Pipeline::Deps{...}`](../src/cli/Gateway.cpp#L22) 一次性注入，Pipeline 自己不 new 依赖。
5. 每个 YAML backend → [`HttpMcpBackend`](../src/cli/Gateway.cpp#L34) 放进 [`Catalog::add`](../src/catalog/Catalog.cpp#L5)。
6. 探活成功 → [`IndexRefresher`](../src/catalog/IndexRefresher.cpp#L7) 写 ToolIndex，并 [`governor_.onToolsListed`](../src/govern/LocalGovernor.cpp#L113) 校验 `governor.tools`。
7. 最后才造 [`HttpMcp`](../src/cli/Gateway.cpp#L54)（Frontend）。

头文件里能一眼看到「进程里有什么」：[`include/perfacet/Gateway.h`](../include/perfacet/Gateway.h#L29)。

### 4.3 `Gateway::start`：listen 前必须探一轮

[`Gateway::start`](../src/cli/Gateway.cpp#L60)：

```
grants_.refreshOnWorker()     把 grants.jsonl 读进无锁快照
health_.probeAllBlocking()    同步 tools/list 每个上游（失败记 Down，不抛）
http_->startListen()          这时才 accept
health_.startTimer()          之后按 interval 异步探
runEvery(grantRefreshMs)      worker 上 stat 文件，mtime 变才 parse
```

首轮探活在 [`ProbeHealth::probeAllBlocking`](../src/health/ProbeHealth.cpp#L38)：直接 `callBlocking`，不占 Governor 配额。成功列表经 [`IndexRefresher::onProbeResult`](../src/catalog/IndexRefresher.cpp#L7) 写入 [`ToolIndex::replace`](../src/catalog/ToolIndex.cpp#L5)。失败**不抹** last-known-good。若从未成功过，[`ToolIndex::cold()`](../src/catalog/ToolIndex.cpp#L19) 为 true，list 的 `ttlMs` 会被打成 0（见下文）。

listen：[`HttpMcp::startListen`](../src/frontend/HttpMcp.cpp#L174) 解析 `listen:` → `TcpServer(loop, addr, 0)` → 注册 `onConn` / `onMessage` → `server_->start()`。

---

## 5. 入口：HTTP → IR

Frontend 的合同写在头文件第一行：[`只做 HTTP ↔ IR`](../include/perfacet/frontend/HttpMcp.h#L2)。

### 5.1 连接与 llhttp

新连接 [`onConn`](../src/frontend/HttpMcp.cpp#L186) 把一个 llhttp [`Session`](../src/frontend/HttpMcp.cpp#L72) 塞进 `TcpConnection::setContext`。

[`onMessage`](../src/frontend/HttpMcp.cpp#L190) 从 [`Buffer`](../netlib/include/netlib/Buffer.h) 喂 llhttp。一个请求没写完响应前 `sess->busy`，写完再 [`queueInLoop`](../src/frontend/HttpMcp.cpp#L233) 消费同一连接上的下一帧（HTTP/1.1 pipelining）。

### 5.2 路径分流（进 Pipeline 之前）

[`onMessage` 后半](../src/frontend/HttpMcp.cpp#L238) 按路径处理，失败在这里就回 HTTP，**不进 Pipeline**：

| 条件 | 行为 | 位置 |
|---|---|---|
| `GET /healthz` | `{"ok":true}`，不鉴权 | [L242](../src/frontend/HttpMcp.cpp#L242) |
| Origin 不在 allowlist | 403 | [L248](../src/frontend/HttpMcp.cpp#L247) |
| `GET /upstreams` | 必须 admin Bearer；返回健康 + Counters + governor 校验状态 | [L263](../src/frontend/HttpMcp.cpp#L263) |
| 非 `POST /mcp` | 404 `-32601` | [L311](../src/frontend/HttpMcp.cpp#L311) |
| `Mcp-Session-Id` / `Last-Event-ID` | 400，本产品无 session | [L320](../src/frontend/HttpMcp.cpp#L320) |
| 正在 drain | 503 | [L329](../src/frontend/HttpMcp.cpp#L329) |
| `Accept` 不含 `application/json` | 406 | [L338](../src/frontend/HttpMcp.cpp#L338) |
| `MCP-Protocol-Version` ≠ `2026-07-28` | 400 `-32022` | [L354](../src/frontend/HttpMcp.cpp#L354) |
| 头与 body `_meta` 版本/方法/名字不一致 | 400 `-32020` | [L382](../src/frontend/HttpMcp.cpp#L382)–[L419](../src/frontend/HttpMcp.cpp#L414) |
| 未知 method（含 `initialize`） | 404 `-32601` | [L422](../src/frontend/HttpMcp.cpp#L422) |
| 无/未知 Bearer | 401，审计 `auth_fail`（不写 token） | [L436](../src/frontend/HttpMcp.cpp#L436) |

已知方法只有五个：`server/discover`、`tools/list`、`tools/call`、`tasks/get`、`tasks/cancel`。

### 5.3 鉴权 → Principal → Request

1. [`YamlIdentityStore::authenticate`](../src/policy/YamlIdentityStore.cpp#L17)：token → `{agentId, level, hasLevel, admin, levelName}`。失败 `nullopt`。`grantBump` 此处为 0。
2. [`grants_->effectiveBump`](../src/policy/JsonlGrantStore.cpp#L86)：loop 上读 `shared_ptr` 快照，`now < expiresAt` 的最高 approved rank。无 syscall。
3. 填 [`ir::Request`](../include/perfacet/ir/Request.h#L61)：method / name / params / meta / deadline / caps / 可选 `traceparent`。
4. **Tasks 能力只看本请求** `_meta.clientCapabilities.extensions["io.modelcontextprotocol/tasks"]`](../src/frontend/HttpMcp.cpp#L477)，不记忆 session。
5. [`pipeline_->handle`](../src/frontend/HttpMcp.cpp#L490)；回调里 [`wrapRpc`](../src/frontend/HttpMcp.cpp#L47) 写成 JSON-RPC HTTP。

工具名对 Agent 永远是 `backend__tool`，由 [`ToolKey::parse`](../src/ir/ToolKey.cpp#L5) 在第一个 `"__"` 切开：`a__b__c` → `{backend:"a", tool:"b__c"}`。

有效档位：[`effectiveRank`](../include/perfacet/ir/Request.h#L43) = `max(level, grantBump)`。可见性：[`visible`](../include/perfacet/ir/Request.h#L47) = `hasLevel && effectiveRank >= mcp.level`。热路径零字符串比较档位名。

---

## 6. Pipeline 方法分流

[`Pipeline::handle`](../src/pipeline/Pipeline.cpp#L109) 开 `total` + `gateway` 两个 span，用 `finish` 统一打 `gatewayMs` 并 `Tracer::end`。然后按 `req.method` 分流：

```
handle
 ├─ server/discover  → 广告 tools + tasks 扩展，listChanged=false     L127
 ├─ tools/list       → FacetView + ttlMs + cacheScope=private          L141
 ├─ tasks/get        → MemTaskStore，别人的 id → 当不存在              L147
 ├─ tasks/cancel     → 标 cancelled，cancelWait（不保证远端死）        L171
 ├─ tools/call       → handleCall                                      L175
 └─ 其它             → 404 -32601                                      L179
```

### 6.1 `tools/list`

[`listBody`](../src/pipeline/Pipeline.cpp#L98)：

- [`FacetView::listTools`](../src/catalog/FacetView.cpp#L8) 用 [`RankPolicy::visibilityFilter`](../src/policy/RankPolicy.cpp#L28) 过滤 ToolIndex，再追加内置 `perfacet__request_elevation` / `perfacet__upstream_status`。
- **不读 Health / Circuit**（DOWN 的上游只要曾经 list 成功，切面里仍在）。
- `ttlMs = min(list_ttl_ms, 该身份最短未过期 Grant 剩余)`；[`index->cold()`](../src/pipeline/Pipeline.cpp#L101) 则强制 0。
- `cacheScope` 写死 `"private"`。

### 6.2 `tasks/get` / `tasks/cancel`

[`getTask`](../src/pipeline/Pipeline.cpp#L187) 校验 `task.agentId == who.agentId`，别人的句柄当 not found。

[`cancelTask`](../src/pipeline/Pipeline.cpp#L195) 更新 MemTaskStore，并对还活着的 [`Call::cancelWait`](../src/pipeline/Pipeline.cpp#L458)。文案是 `"cancelled; remote may still run"` —— 取消是放弃等待，不是 kill 上游。

---

## 7. `tools/call` 六步（热路径）

全部在 [`Pipeline::handleCall`](../src/pipeline/Pipeline.cpp#L269)。**顺序钉死，没有 interceptor。** Deny 在第 1 步就 return，Governor / InFlight / Backend 计数必须仍为 0（不变量 8）。

```
handleCall
  1. parse ToolKey + authorizeCall
        Deny / Unknown → 审计 deny，return
        backend==perfacet → handleBuiltin，不进治理
  2. InFlight.lookup(agentId, key, paramsHash)
        命中 && tasks     → 复用/创建 task 句柄，不进 Governor
        命中 && !tasks    → Throttled + if_<inflightId> 确认令牌
        命中 && confirm   → 真的再打（inflightConfirm++）
  3. Governor.acquire → 排队或立刻 Go
        Reject → Throttled（不建 Call、不建 task）
  4. Circuit OPEN 或 Health Down → Unavailable（Permit 析构归还）
  5. InFlight.insert + new Call + startUpstream
        Backend.call 丢到 worker 之后才 arm promote 定时器
  6. 上游先回 → 同步 JSON；定时器先到 → promote（句柄或 -32021）
```

### 第 1 步：授权

[`RankPolicy::authorizeCall`](../src/policy/RankPolicy.cpp#L17)：

| 情况 | Decision | 对外 |
|---|---|---|
| `perfacet__*` | [`builtinDecision`](../src/policy/RankPolicy.cpp#L7) | elevation 需 `hasLevel`；`upstream_status` 需 `admin` |
| 未知 backend | `Unknown` | [`"unknown tool"`](../src/pipeline/Pipeline.cpp#L50) |
| `!hasLevel`（纯 admin） | `Deny` | `"not allowed"` |
| `effectiveRank < mcp.level` 且 `secret` | `Unknown` | 与拼错工具同一形状，不暴露 secret 上游存在 |
| `effectiveRank < mcp.level` | `Deny` | `"not allowed"` |
| 否则 | `Allow` | 继续 |

内置工具在 Allow 后走 [`handleBuiltin`](../src/pipeline/Pipeline.cpp#L215)，**不占 Permit、不进 InFlight**：

- `request_elevation`：校验 `bump_to` ≤ `elevation.max_level`，[`appendPending`](../src/pipeline/Pipeline.cpp#L236) 写 grants.jsonl，同步返回 `{grantId, status:pending}`（不是 task）。人用 [`perfacet grant approve`](../src/cli/Cli.cpp#L49) 批。
- `upstream_status`：仅 admin，返回各 server Health 快照。

### 第 2 步：在途去重

哈希：[`paramsHashOf`](../src/pipeline/Pipeline.cpp#L16) 跳过 `_meta`，FNV-1a。只在**同一 `agentId`** 内去重，跨 Agent 不合流。

[`InFlight::lookup`](../src/pipeline/InFlight.cpp#L20) 用 `agent \n tool \n hash` 做 map key。

命中且客户端声明了 tasks：复用已有 `taskId`，必要时先 [`MemTaskStore::insert`](../src/pipeline/Pipeline.cpp#L326) 再返回 [`CreateTaskResult`](../src/pipeline/Pipeline.cpp#L54)（SEP-2663：句柄必须先于 HTTP 可查）。`inflight_hit` 这条路径**没有**第二条 upstream span。

命中且未声明 tasks：返回人话 + `_meta["perfacet/confirm"]="if_<inflightId>"`。令牌绑的是 inflightId，不是 `force:true`。再打一次带匹配令牌才放行。

[`~Call`](../src/pipeline/Pipeline.cpp#L424) 里 [`inflight->erase`](../src/pipeline/InFlight.cpp#L53)，令牌随条目消失。

### 第 3 步：Governor 准入

[`LocalGovernor::acquire`](../src/govern/LocalGovernor.cpp#L55)：

- 限额：per-tool（`governor.tools[key].maxConcurrency` 或 default）+ per-principal。
- 有空槽 → 立刻 [`issue`](../src/govern/LocalGovernor.cpp#L50) 一张 move-only [`Permit`](../include/perfacet/govern/Governor.h#L13)。
- 满 → FIFO [`Waiter`](../include/perfacet/govern/LocalGovernor.h#L35) + `runAfter(queue_wait_ms)`；到期 [`Admit::Reject`](../src/govern/LocalGovernor.cpp#L73)。
- YAML `rate_per_sec` **读了忽略**（构造时打一行 debug）。

Permit 无公开 `release`。[析构 / move-assign](../src/govern/Governor.cpp#L25) 是唯一归还路径，内部调 [`releaseSlot`](../src/govern/LocalGovernor.cpp#L103) → [`tryDequeue`](../src/govern/LocalGovernor.cpp#L86) 唤醒队头。`permitHeld` 计数与此对账。

排队超时对外是 Throttled，**不升格 task**（demo 剧本：4 抢 3 的第四个）。

### 第 4 步：熔断 / 探活

acquire 回调里（仍持有 Permit）：

[`circuit->isOpen` 或 `health->state == Down`](../src/pipeline/Pipeline.cpp#L394) → Unavailable，函数 return 时 Permit 析构归还。OPEN **不改** FacetView。

[`CountCircuit`](../include/perfacet/health/CountCircuit.h)：连续失败 `open_after` → OPEN → `cooldown_ms` → HALF_OPEN。探活成功只在 HALF_OPEN 时 [`onProbeSuccess`](../src/health/CountCircuit.cpp#L63) 合闸；OPEN 期间探活不得直接关。

### 第 5 步：发出上游

[`inflight->insert`](../src/pipeline/InFlight.cpp#L30) 生成 128-bit hex `inflightId` → [`make_shared<Call>`](../src/pipeline/Pipeline.cpp#L408) → [`startUpstream`](../src/pipeline/Pipeline.cpp#L540)。

[`Call::fireAttempt`](../src/pipeline/Pipeline.cpp#L545)：

1. [`catalog->backend`](../src/catalog/Catalog.cpp#L27) 取 `HttpMcpBackend*`。
2. **第一次 attempt 才** [`armPromote`](../src/pipeline/Pipeline.cpp#L489)（不变量 22：定时器在 `Backend.call` 已丢到 worker **之后**；实现上 arm 紧挨在 `be->call` 之前，且只在 attempt==1。排队等待期间没有 Call，也就没有定时器）。
3. DEGRADED 时 `promote_after_ms` 减半。
4. 开 `upstream` span，[`be->call(makeBackendCall(), ...)`](../src/pipeline/Pipeline.cpp#L562)。

[`makeBackendCall`](../src/pipeline/Pipeline.cpp#L471) 剥掉 `perfacet/confirm`，`name` 改成**上游工具名**（无 `backend__` 前缀）。Backend 看不到 Principal。

[`HttpMcpBackend::call`](../src/backend/HttpMcpBackend.cpp#L127) 若 worker 队列满，立刻 Unavailable。否则 worker 上 [`callBlocking`](../src/backend/HttpMcpBackend.cpp#L45)：

- 按 deadline 收紧超时；过期直接 Timeout。
- 组 JSON-RPC + `MCP-Protocol-Version` / `Mcp-Method` / `Mcp-Name` / `traceparent`。
- **不对上游声明 tasks**（不 re-attach `remoteTaskId`）。
- [`keepAlivePost`](../src/backend/KeepAliveClient.cpp#L34)：`thread_local × host:port` 复用 httplib Client（非线程安全，禁止跨线程共享）。
- 失败分类走 [`ir::classify`](../src/ir/classify.cpp#L42)：网关自己的 `-32021` 才是 Capability；上游同码是 Upstream。
- 完成 [`loop_->queueInLoop`](../src/backend/HttpMcpBackend.cpp#L143) 再进 [`Call::onUpstream`](../src/pipeline/Pipeline.cpp#L567)。

### 第 6 步：同步返回 vs 升格

两条赛跑，[`responded_`](../include/perfacet/pipeline/Pipeline.h#L105) 保证只写一次 HTTP：

**上游先回** [`onUpstream`](../src/pipeline/Pipeline.cpp#L567)：

- 结束 upstream span；已 `cancelled_` 则只记 `upstreamDone_`，不再写 HTTP。
- Ok → [`circuit.onSuccess`](../src/health/CountCircuit.cpp#L31)；否则 [`onFailure`](../src/health/CountCircuit.cpp#L45)，刚 OPEN 则审计 `circuit_open`。
- [`RetryPolicy::shouldRetry`](../src/health/RetryPolicy.cpp#L11)：klass 在 `retryable`、不在 `never`、未过 deadline、电路非 OPEN、attempt < max。**Timeout 额外两道门**：工具必须在 `idempotent_tools`；且「上一枪还在跑」时禁止当可重试 Timeout。
- 需要重试则再 `fireAttempt()`（不再 arm 第二个 promote）。
- 否则取消 promote 定时器；若已有 taskId，更新 MemTaskStore `completed|failed`；若 HTTP 还没写，`respond`。

**定时器先到** [`onPromote`](../src/pipeline/Pipeline.cpp#L497)：

- 已 `responded_` / `cancelled_` → 什么都不做。
- `caps.tasks`：MemTaskStore **先 insert**，再返回 `resultType=task` 的句柄。HTTP 连接可以结束；**Permit 仍占着**，直到上游回调把 Call 析构。
- `!caps.tasks`：立刻 HTTP 400 `-32021` [`missingTasksError`](../src/pipeline/Pipeline.cpp#L43)。禁止干等到客户端超时。Permit 同样持到上游回调。

Agent 之后用 `tasks/get` 轮询；终态带原 `CallToolResult` 或 error。

---

## 8. Call 生命周期

[`Call`](../include/perfacet/pipeline/Pipeline.h#L78) 是一次已准入调用的 RAII 包：Permit + InFlight 条目 + promote 定时器 + span。用 `shared_ptr` 是因为定时器、worker 回调、Pipeline 的 `live_` / `byTask_` 都要持活它。

```
acquire Go
  → insert InFlight
  → Call 构造（拿走 Permit）
  → startUpstream / fireAttempt / armPromote
        ┌──────────── 仍 !responded_ ────────────┐
        │ 上游回调 onUpstream                    │
        │    或 promote 到点 onPromote            │
        │    或 cancelWait / ~Call 兜底          │
        └──────────── respond() 一次 ────────────┘
  → shared_ptr 归零
  → ~Call：cancel 定时器、end 未关的 span、
           若还没 respond 则 Cancelled、
           erase InFlight、摘 byTask_/live_
           Permit 成员析构 → releaseSlot
```

关键方法：

| 方法 | 作用 |
|---|---|
| [`startUpstream`](../src/pipeline/Pipeline.cpp#L540) | 登记 `live_[inflightId]`，开火 |
| [`respond`](../src/pipeline/Pipeline.cpp#L448) | 幂等；成功 call 写审计 `ok` |
| [`cancelWait`](../src/pipeline/Pipeline.cpp#L458) | 停机 / tasks/cancel：放弃等，远端可能还在跑 |
| [`attachTask`](../src/pipeline/Pipeline.cpp#L441) | 把 taskId 写进 InFlight 与 `byTask_` |
| [`~Call`](../src/pipeline/Pipeline.cpp#L424) | 唯一收尾点 |

停机时 [`Pipeline::requestStop`](../src/pipeline/Pipeline.cpp#L70) 对所有 live Call `cancelWait`；新 call 在 acquire 前若 [`stopping_`](../src/pipeline/Pipeline.cpp#L361) 则 503。

---

## 9. 控制面：目录 / 探活 / Grant

这些**不在** `tools/call` 热路径里编排，但决定 list 看见什么、call 会不会 Unavailable。

### 9.1 目录切面数据流

```
YAML backends
  → Catalog（name → Backend* + BackendMeta）
  → ProbeHealth 周期性 tools/list（独立预算）
  → IndexRefresher 成功才 replace ToolIndex
  → FacetView = ToolIndex ⨯ Policy.visibilityFilter ∪ 内置工具
```

- [`Catalog`](../include/perfacet/catalog/Catalog.h)：不带 Principal。热路径按 `ToolKey.backend` 查找。
- [`ToolIndex`](../include/perfacet/catalog/ToolIndex.h)：last-known-good。Health **禁止**写；失败不抹；DOWN/OPEN 不删。
- [`FacetView`](../include/perfacet/catalog/FacetView.h)：可见性只由 Rank 决定。

定时探活 [`ProbeHealth::startTimer`](../src/health/ProbeHealth.cpp#L57)：间隔 `max(health.interval_ms, 上游 ttlMs)`，走 `Backend::call`（worker），回调仍回 loop。EWMA ≥ `degraded_latency_ms` → Degraded；连续 `down_after_failures` → Down。探测**不占** Governor 配额。

### 9.2 Grant 提权

文件格式见 [`JsonlGrantStore`](../include/perfacet/policy/JsonlGrantStore.h) 头注释。两条时间线：

| 线程 | 做什么 |
|---|---|
| Worker | [`refreshOnWorker`](../include/perfacet/policy/JsonlGrantStore.h#L40)：`stat`，mtime 变才 parse，原子 swap `shared_ptr<const GrantTable>` |
| IO loop | [`effectiveBump`](../src/policy/JsonlGrantStore.cpp#L86) / [`shortestRemainingMs`](../src/policy/JsonlGrantStore.cpp#L99)：只读快照 + `now` 比较。过期不依赖 reload |

过期时 [`setOnExpire`](../src/cli/Gateway.cpp#L46) 发审计 `grant_expire`。进行中的 Task 绑的是创建时的 Principal 快照，不随后续 Grant 变。

### 9.3 观测

- [`Counters`](../include/perfacet/observe/Counters.h)：热路径只 `fetch_add`。`permit_held` / `inflight_held` 是瞬时值，混合终态后必须归零。
- [`OtlpHttpJsonTracer`](../include/perfacet/observe/OtlpHttpJsonTracer.h)：loop [`start`/`end`](../src/observe/OtlpHttpJsonTracer.cpp#L47) 只入队；worker POST。满则丢，`otlp_dropped++`。禁止写 token。
- [`JsonlAuditLog::emit`](../src/audit/JsonlAuditLog.cpp#L14)：同样投递 worker append。事件闭集：`ok` / `auth_fail` / `deny` / `throttled` / `inflight_hit` / `circuit_open` / `grant_approve` / `grant_expire`。

`perfacet status` 是 CLI 用 admin token [`GET /upstreams`](../src/cli/Cli.cpp#L98)，不算 Frontend 热路径。

---

## 10. 停机

SIGINT → [`onSig`](../src/cli/Cli.cpp#L20) `queueInLoop` → [`Gateway::requestStop`](../src/cli/Gateway.cpp#L74)：

```
pauseAccept()           停听，已有连接不关
HttpMcp::setStopping()  新 POST /mcp → 503
pipeline_.requestStop() 在途 Call cancelWait
governor_.rejectAllQueued()  队列里的 Waiter → Throttled
runAfter(drain_timeout_ms)   默认 3s 后 loop.quit()
```

HTTP 没有 GOAWAY 帧。信号就是：停 accept + 在途连接写完响应后 shutdown。`MemTaskStore` 随进程消失（M1 知情缺口：进程内、不 fsync）。

---

## 11. netlib 底座

禁止在 netlib 里出现 MCP / Rank / token。Perfacet 只用它当 **Agent 侧** 字节流服务器；上游不用 `TcpClient`，用 cpp-httplib。

| 类型 | M1 用法 | 入口 |
|---|---|---|
| `EventLoop` | 唯一 IO loop：epoll + timerfd + eventfd 唤醒 | [`EventLoop.h`](../netlib/include/netlib/EventLoop.h) |
| `TcpServer` | `ioThreads=0`，连接跑在 mainLoop | [`TcpServer.h`](../netlib/include/netlib/TcpServer.h#L21) |
| `TcpConnection` + `Buffer` | HTTP/1.1 入缓冲；llhttp 消费 | [`TcpConnection.h`](../netlib/include/netlib/TcpConnection.h) |
| `Channel` | fd + 事件 + 回调；`tie` 防析构后继续分发 | [`Channel.h`](../netlib/include/netlib/Channel.h) |
| `ThreadPool` | httplib / OTLP / audit / grant refresh | [`ThreadPool.h`](../netlib/include/netlib/ThreadPool.h) |
| `Acceptor` / `Socket` / `Epoll` | TcpServer 内部 | 业务不要直接碰 |

一圈 loop：[poll → handleEvent → doPending → flushAll](../netlib/src/EventLoop.cpp#L50)。跨线程投递：`runInLoop` / `queueInLoop` + eventfd。

---

## 12. 建议阅读顺序

按「能讲清楚一条请求」而不是按目录字母序：

1. [`include/perfacet/ir/Request.h`](../include/perfacet/ir/Request.h) —— 跨层货币，20 分钟读完。
2. [`src/cli/Gateway.cpp`](../src/cli/Gateway.cpp) —— 进程里有哪些对象、怎么焊。
3. [`src/frontend/HttpMcp.cpp`](../src/frontend/HttpMcp.cpp) `onMessage` —— 哪些错误根本进不了 Pipeline。
4. [`src/pipeline/Pipeline.cpp`](../src/pipeline/Pipeline.cpp) `handle` → `handleCall` → `Call::*` —— 本文第 6–8 节对着看。
5. [`src/policy/RankPolicy.cpp`](../src/policy/RankPolicy.cpp) + [`src/catalog/FacetView.cpp`](../src/catalog/FacetView.cpp) —— 切面。
6. [`src/pipeline/InFlight.cpp`](../src/pipeline/InFlight.cpp) + [`src/govern/LocalGovernor.cpp`](../src/govern/LocalGovernor.cpp) —— 去重与配额。
7. [`src/backend/HttpMcpBackend.cpp`](../src/backend/HttpMcpBackend.cpp) —— worker 边界。
8. [`src/health/`](../src/health/ProbeHealth.cpp) —— 探活 / 熔断 / 重试门，理解「list 仍可见、call 才 Unavailable」。
9. [`tests/pipeline/pipeline_test.cpp`](../tests/pipeline/pipeline_test.cpp) —— 用桩把六步跑出来，不必起 HTTP。
10. [`tests/invariants/`](../tests/invariants/inv8_test.cpp) —— 不变量 8 等契约。

想对着规格核对：SPEC [§4 架构](SPEC.md#4-架构与线程)、[§6.4 Pipeline 六步](SPEC.md#64-pipeline--pipeline--call--inflight)、[§14 不变量](SPEC.md#14-不变量检查表实现者打印贴显示器)。

跑通一条真请求：`examples/demo.sh` + [`examples/mcp_call.py`](../examples/mcp_call.py) / [`examples/mock_mcp.py`](../examples/mock_mcp.py)。

---

## 13. 文件索引

### 热路径（一次 `tools/call` 会碰到）

| 文件 | 跳转 |
|---|---|
| HTTP 解析与鉴权 | [`HttpMcp.cpp`](../src/frontend/HttpMcp.cpp#L190) |
| 方法分流 | [`Pipeline::handle`](../src/pipeline/Pipeline.cpp#L109) |
| 六步 | [`Pipeline::handleCall`](../src/pipeline/Pipeline.cpp#L269) |
| 授权 | [`RankPolicy.cpp`](../src/policy/RankPolicy.cpp#L17) |
| 在途表 | [`InFlight.cpp`](../src/pipeline/InFlight.cpp) |
| 许可 | [`LocalGovernor.cpp`](../src/govern/LocalGovernor.cpp#L55) |
| 熔断查询 | [`CountCircuit::isOpen`](../src/health/CountCircuit.cpp#L21) |
| 上游 POST | [`HttpMcpBackend.cpp`](../src/backend/HttpMcpBackend.cpp#L45) |
| keep-alive | [`KeepAliveClient.cpp`](../src/backend/KeepAliveClient.cpp#L34) |
| 失败分类 | [`classify.cpp`](../src/ir/classify.cpp#L42) |
| 重试门 | [`RetryPolicy.cpp`](../src/health/RetryPolicy.cpp#L11) |
| Call 状态机 | [`Call::onUpstream`](../src/pipeline/Pipeline.cpp#L567) / [`onPromote`](../src/pipeline/Pipeline.cpp#L497) |

### 控制面与身份

| 文件 | 跳转 |
|---|---|
| 进程组装 | [`Gateway.cpp`](../src/cli/Gateway.cpp) |
| YAML | [`YamlConfig.h`](../include/perfacet/policy/YamlConfig.h) / [`YamlConfig.cpp`](../src/policy/YamlConfig.cpp#L20) |
| 档位全序 | [`Taxonomy.h`](../include/perfacet/policy/Taxonomy.h) |
| Bearer | [`YamlIdentityStore.cpp`](../src/policy/YamlIdentityStore.cpp#L17) |
| Grant | [`JsonlGrantStore.h`](../include/perfacet/policy/JsonlGrantStore.h) |
| Catalog | [`Catalog.cpp`](../src/catalog/Catalog.cpp) |
| 目录快照 | [`ToolIndex.cpp`](../src/catalog/ToolIndex.cpp) |
| 切面 | [`FacetView.cpp`](../src/catalog/FacetView.cpp) |
| 探活写目录 | [`IndexRefresher.cpp`](../src/catalog/IndexRefresher.cpp) |
| 探活 | [`ProbeHealth.cpp`](../src/health/ProbeHealth.cpp) |
| Task 句柄 | [`MemTaskStore.h`](../include/perfacet/task/MemTaskStore.h) / [`Task.h`](../include/perfacet/task/Task.h) |

### 观测、CLI、底座

| 文件 | 跳转 |
|---|---|
| 计数 | [`Counters.h`](../include/perfacet/observe/Counters.h) |
| Trace | [`OtlpHttpJsonTracer.cpp`](../src/observe/OtlpHttpJsonTracer.cpp) |
| 审计 | [`JsonlAuditLog.cpp`](../src/audit/JsonlAuditLog.cpp) |
| CLI | [`Cli.cpp`](../src/cli/Cli.cpp) |
| 分层检查 | [`check_layers.sh`](../scripts/check_layers.sh) |
| EventLoop | [`EventLoop.cpp`](../netlib/src/EventLoop.cpp#L43) |
| TcpServer | [`TcpServer.cpp`](../netlib/src/TcpServer.cpp#L22) |

### 测试怎么对上六步

| 目录 | 覆盖 |
|---|---|
| [`tests/pipeline/`](../tests/pipeline/pipeline_test.cpp) | 用桩跑 handleCall |
| [`tests/inflight/`](../tests/inflight/inflight_test.cpp) | 同参合流、令牌、跨 agent |
| [`tests/governor/`](../tests/governor/governor_test.cpp) | 4 抢 3 FIFO、Permit 归还 |
| [`tests/policy/`](../tests/policy/policy_test.cpp) | 切面、secret unknown、Grant 过期 |
| [`tests/invariants/`](../tests/invariants/inv8_test.cpp) | Deny 不进 Governor/InFlight |
| [`tests/ir/`](../tests/ir/toolkey_test.cpp) | ToolKey / classify |
| [`tests/health/`](../tests/health/circuit_test.cpp) | 熔断 / 重试 |
