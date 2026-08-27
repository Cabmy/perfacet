# Perfacet —— per-agent facet（一个身份，一个切面）

> 制定日期：2026-08-26（整理稿）
> 名字：**Perfacet** = per-agent facet。同一张目录、同一条 URL，每个身份只看见自己的切面。
> 协议：**只实现 MCP `2026-07-28`**
> 原则：一张注册表，一个入口，一种协议，按身份裁切面。虚接口只留给真有第二实现的点；请求主流程在 Pipeline 里只认 IR。

产品只认：MCP 接入、权限档位、切面、Grant、Tasks、在途去重、上游健康、并发治理、可观测（一条 trace、一份审计、几个计数）。本仓库的 epoll 是工程便利，不是产品定义。

---

## 0. 产品

**Perfacet 是 Multi-Agent Tool Gateway：接入 MCP，按身份切面，并治理多 agent 对共享工具的并发。**

已经在跑的 MCP server（本机或远端）登记 HTTP endpoint 即可接入。不同机器上的 agent 连同一条 Streamable HTTP 入口。共享的是目录投影，不是权限——**权限层始终在**。

- `access.levels` 是档位唯一声明；`agents:` / `backends:` 只引用 `level`
- 出厂 `levels: [default]`、两处省略 `level` → 全体同一级，切面等于 Catalog 全集
- 加档是 `levels` 多一行，再给连接写上引用。C++ 零改
- 高保密对低权限不可见；提权走 Grant（CLI 审批 + TTL）
- `admin: true` 是控制面开关，**不是**一档 level

对 **agent**：它就是一个普通的 `2026-07-28` MCP server，`mcp.json` 里一条 URL + Bearer。

对 **MCP server**：它是一个普通 MCP client。只要求对端会 MCP，不负责拉起、重启、镜像、沙箱。健康检查是「看它活着没有」，不是「替它养活」。stdio 对端在网关外用 `mcp-proxy` 转成 HTTP 再登记。

| | Docker MCP Gateway | Perfacet |
|---|---|---|
| 职责 | **跑** MCP（镜像、生命周期、隔离） | **接入并治理** MCP |
| 注册对象 | 镜像 / 启动命令 | MCP HTTP endpoint |
| agent 在哪 | 多为本机 | 任意能打到入口的机器 |
| 谁看见哪些工具 | 静态 profile / `--tools` | 出厂一档全可见；加档 + 引用 → 每请求切面 |
| 提权 | 改 profile / 重启 | Grant：CLI 人批 + TTL |
| 上游活着没有 | 进程/容器是否还在 | Health：UP / DEGRADED / DOWN + latency |
| 多 agent 抢同一工具 | 弱 / 容器配额 | Governor：per-tool、per-principal、FIFO |
| 长任务 | 无 | 代实现 Tasks（已发出的上游调用还没完才拆连接） |
| agent 超时后重打 | 无 | 同身份同参数在途则合流 / 确认，不叠第二条 SQL（§5.5） |
| 追踪 | 无此主语 | 一条 OTel trace + 审计 JSONL 对 `trace_id` + status 几个计数（§5.6） |

Docker 比沙箱和供应链。Perfacet 比的是：**同一入口上按身份切面，并把共享工具当被治理的资源。**

**部署：** agent 与上游可跨机器；**网关自身单实例（SPOF）**。多实例不是「换一个共享 Governor」——见 §5.4。

---

## 1. 协议（只跟最新版）

不实现、不兼容 `2025-11-25` 及更早。客户端不会 `2026-07-28` 就不接。演示用官方 SDK v2 / 已升级的 Cursor、Claude Code。

| 点 | `2026-07-28` | M1 明确不做 |
|---|---|---|
| 传输 | Streamable HTTP：`POST /mcp` | stdio 前端、旧 SSE GET、WebSocket |
| 握手 | 每请求 `_meta` 自带版本与能力；`server/discover` | `initialize` / `Mcp-Session-Id` |
| 路由 | **必须**读写 `Mcp-Method`、`Mcp-Name` | 靠解析 JSON 做路由 |
| 列表 | 必填 `ttlMs` + `cacheScope` | 过滤后的列表标 `public` |
| 长任务 | `CreateTaskResult` → `tasks/get` / `update` / `cancel` | `tasks/result`、`tasks/list`、阻塞等结果 |
| 中途输入 | 规范有 MRTR：`input_required` | **整条不做。** 提权走 CLI |
| 流 | 短调用一次 JSON；需要流才 SSE | `Last-Event-ID` 回放 |
| 任务路由 | `tasks/*` 时 `Mcp-Name` = `taskId` | 会话粘滞 |
| 缺客户端能力 | 无法不返回 task 且未声明 `tasks` → **`-32021`**，HTTP 400，`data.requiredCapabilities.extensions["io.modelcontextprotocol/tasks"]` | 干等到 timeout；偷偷给句柄 |

SEP-2663 MUST：

> A server MUST NOT return `CreateTaskResult` until the task is durably created.

**M1 知情缺口：** `MemTaskStore`（进程内）。单实例崩了 agent 本来就要重试；头文件一行「抽接口 + JSONL/SQLite 即满足 MUST」。M1 **仍然**遵守：未声明 `tasks` 永不给句柄；promote 到点无法同步返回时立刻 `-32021`。

码号跟 `2026-07-28` 核心 schema：**`-32021`**。ext-tasks overview / 部分草稿仍印 `-32003`。面试主动说这一码。

追踪：接请求 `_meta` / `traceparent`；没有则网关建根 span。下游带出 `traceparent`。不发明私有 trace 头作为唯一方案。

---

## 2. 架构

### 2.1 分层

```
 Agent
              │  Bearer + 可选 traceparent
              │  POST /mcp
┌─────────────▼──────────────┐
│ Frontend                   │  HTTP ↔ IR（token / _meta / traceparent）
│                            │  YamlIdentityStore + JsonlGrantStore → Principal
└─────────────┬──────────────┘
              │  ir::Request
┌─────────────▼──────────────┐
│ Pipeline                   │  authorize → inflight → admit → circuit → call → promote
│                            │  只认 IR；Call 住这里
│  FacetView                 │  ToolIndex ⨯ Policy::visibilityFilter
│  Governor                  │  回调准入；move-only Permit
│  Health + CountCircuit     │  OPEN 不打上游；不改切面
│  Backend                   │  HttpMcpBackend；吃 BackendCall（P1）
└────────────────────────────┘
              │
    MCP Server（HTTP）
    stdio：网关外 mcp-proxy 转 HTTP 后再登记

控制面（不在热路径上编排）：
  Catalog        name → Backend + BackendMeta；不带 Principal
  ToolIndex      last-known-good；IndexRefresher 写，Health 不写
  ProbeHealth    探活，回调 onProbeResult(server, state, optional<toolList>)
```

请求顺序是 Pipeline 里的六步，不是文档约定。Deny 不进 Governor、不进 InFlight（不变量 8），纯内存可测。

Policy 答「有没有这把工具」；Governor 答「现在能不能再开一把」。出厂一档时 Policy 恒通过，仍走 Rank，没有旁路。

**可见性只由 Rank 决定，与 Health / Circuit 无关。** DOWN / OPEN 时切面内仍列出 last-known-good，call 才是 Unavailable。secret 且不在切面里的 server，不得通过 list 或错误码让低权限知道它存在（包括它已 DOWN）。

**纪律（编译器 / CI，不靠自觉）：**

- `ir` 不认 HTTP，不 include 上层
- 只有 `frontend` 碰 agent HTTP 与 agent token
- C++ 不出现用户业务档位名；加载期消解成 `Rank`
- `classify()` 在 `ir/`；Backend 禁止私自 `retry=true`
- 虚接口必须有真实现、被真调用、且 M1 就有可信第二实现（或明确排期）
- IR 不留没人读的字段

分层强制：P1 起每层独立 CMake target + `PRIVATE` 链接（`pipeline` 不链 `frontend`）。此前 `scripts/check_layers.sh` 兜底 include 方向。grep 只留语义规则——agent token、业务档位名。token 规则的失效条件写在脚本里：只扫 `*.h`/`*.cpp`；行内 `PERFACET_LAYER_ALLOW` 豁免；「下游自带鉴权头」落地时改为禁止 agent 侧 token、允许 backend 侧下游凭据。

### 2.2 IR

```cpp
namespace perfacet::ir {

using Rank = uint16_t;   // 全序链上的位置，不是格节点。见 §3.1

// 四处共用：tools/call name、Mcp-Name、governor.tools、ToolIndex
// first "__"：parse("a__b__c") → {a, b__c}。backend 名禁止含 "__"。
struct ToolKey {
    std::string backend;
    std::string tool;
    static std::optional<ToolKey> parse(std::string_view);
    std::string str() const;   // backend + "__" + tool
};

struct ClientCaps {
    bool tasks = false;
    bool has(std::string_view ext) const;   // 不变量 19 只读 tasks
};

struct Principal {
    std::string agentId;
    Rank        level = 0;        // 仅 hasLevel 时有意义
    Rank        grantBump = 0;    // 读时求值
    bool        hasLevel = false; // fail-closed
    bool        admin = false;    // 控制面，不是一档
    std::string levelName;        // 仅 span / 审计
};

struct BackendMeta {
    Rank level = 0;
    bool secret = false;
    std::vector<std::string> idempotentTools;  // Timeout 仅列内可重试
};

struct TraceContext {
    std::string traceId, spanId, parentSpanId;
};

enum class FailureClass {
    Ok,
    Cancelled,     // 网关放弃等待；不保证远端终止
    Timeout,
    Unavailable,   // DOWN / OPEN
    Throttled,
    Protocol,
    Capability,    // -32021；不重试
    Upstream,
    Authz,
    Internal
};

struct Request {
    std::string  method, name, upstreamId;
    Json         params, meta;     // meta 可含 perfacet/confirm；造 BackendCall 时剥掉
    Principal    who;
    TraceContext trace;
    uint64_t     deadlineMs;
    ClientCaps   caps;             // Frontend 从 _meta.extensions 解一次
};

// P1：Backend 只吃这个。透传身份必须显式加 forwardedIdentity。
struct BackendCall {
    std::string  method, name;
    Json         params, meta;
    uint64_t     deadlineMs;
    TraceContext trace;
};

struct Response {
    std::string   upstreamId;
    Json          body;
    bool          isError = false;
    FailureClass  klass = FailureClass::Ok;
    uint64_t      gatewayMs = 0;
    uint64_t      upstreamMs = 0;
};

FailureClass classify(const Response&, std::error_code);

} // namespace perfacet::ir
```

```
effectiveRank(who) = max(who.level, who.grantBump)
visible(who, mcp)  = who.hasLevel && effectiveRank(who) >= mcp.level
```

热路径零字符串比较。未知 level 在请求期类型上无法出现。

Frontend 造 Principal：`YamlIdentityStore.authenticate`（失败 401）+ `JsonlGrantStore.effectiveBump`。后者在 loop 上 **只无锁读快照** + `now < expiresAt`，worker 才 `stat`/解析。见 §3.6。

`hasLevel` 默认 `false`。只有 authenticate 显式成功才为 true。割线后可用 `AuthenticatedPrincipal`（私有构造 + friend）把「忘了认证」变成编译错误。

### 2.3 虚接口 vs 具体类

M1 留 **5** 个虚接口。其余具体类，头文件一行「何时抽」。

| 接口 | 实现 | 为什么留 |
|---|---|---|
| `Backend` | `HttpMcpBackend` | 产品就是多接入；其后 `RpcBackend` |
| `Policy` | Rank 比较 + 内置工具分流 | CEL/OPA；切面的可插拔点 |
| `Governor` | `LocalGovernor` | 隔离准入。**不是**已为多实例留好的接口 |
| `Health` | `ProbeHealth`（不写 ToolIndex） | 可换探测方法 |
| `Tracer` | 手写 OTLP/HTTP JSON；可换成 no-op | 导出器可换 |

```cpp
class Backend {
public:
    virtual void call(const ir::BackendCall&, std::function<void(ir::Response)>) = 0;
};

class Policy {
public:
    virtual Decision authorizeCall(const ir::Principal&, const ir::ToolKey&) = 0;
    virtual std::function<bool(const ir::ToolKey&)>
        visibilityFilter(const ir::Principal&) = 0;   // list：一次编译，避免 n 次求值
};

class Health {
public:
    enum class State { Up, Degraded, Down };
    virtual State state(const std::string& server) const = 0;
    virtual uint64_t latencyEwmaMs(const std::string& server) const = 0;
};

class Governor {
public:
    enum class Admit { Go, Queue, Reject };
    class Permit {   // move-only；pImpl 持 Governor* + ToolKey；禁止空壳
        struct Impl;
        std::unique_ptr<Impl> impl_;
    public:
        Permit() = default;
        Permit(Permit&&) noexcept;
        Permit& operator=(Permit&&) noexcept;
        Permit(const Permit&) = delete;
        ~Permit();
    };
    virtual void acquire(const ir::Principal&, const ir::ToolKey&,
                         uint64_t waitDeadlineMs,
                         std::function<void(Admit, Permit)> onAdmit) = 0;
};

class Tracer {
public:
    virtual ir::TraceContext start(const ir::Request&, const char* spanName) = 0;
    virtual void set(const ir::TraceContext&, const char* key, const std::string& value) = 0;
    virtual void end(const ir::TraceContext&, FailureClass, uint64_t latencyMs) = 0;
};
```

| 具体类 | M1 | 将来 |
|---|---|---|
| `YamlIdentityStore` | YAML agents | 抽 `IdentityStore`（OIDC） |
| `JsonlGrantStore` | worker 刷新快照；loop 无锁读 | 抽 `GrantStore` |
| `MemTaskStore` | 进程内；不 fsync | 抽 `TaskStore` + JSONL/SQLite |
| `CountCircuit` | 计数熔断 | 抽 `Circuit` |
| `Taxonomy` / `Catalog` / `ToolIndex` | 数据结构 | —— |
| `FacetView` | `ToolIndex&` + `Policy&` | 可插拔的是 Policy |
| `RetryPolicy` | 读 YAML | Timeout 必须看 `idempotent_tools`；在途未完成禁止当可重试 Timeout |
| `IndexRefresher` | 吃 `onProbeResult` | —— |
| `Pipeline` / `Call` | 主流程 / 唯一 owner | —— |
| `InFlight` | 进程内 map；`~Call` 摘除 | 不抽接口。不跨身份、不持久化 |
| `JsonlAuditLog` | worker append；loop 只投递 | 头文件一行「抽 AuditLog」。M1 不抽 |
| `Counters` | 原子量；`perfacet status` 读 | 不抽。不是 Prometheus |

**不存在：** `Executor`、`Promoter`。promote 就是 Pipeline 里一只定时器。

### 2.4 Pipeline

Frontend 只做 HTTP ↔ IR。编排在 Pipeline。

```cpp
class Pipeline {
public:
    void handle(ir::Request, std::function<void(ir::Response)> onDone);
};

class Call {   // 一次 tools/call 的唯一 owner
    Governor::Permit permit_;
    ir::TraceContext trace_;
    bool responded_ = false;   // HTTP 已结束（上游 / 句柄 / -32021）
public:
    Call(Governor::Permit, ir::TraceContext);
    ~Call();   // 归还 Permit + 从 InFlight 摘除 + 取消定时器 + 关 span
};
```

定时器在 IO loop；httplib 在 worker。worker 完成必须 **post 回 loop** 再看 `responded_`。

```
1. visibilityFilter / authorizeCall     Deny → Authz（不变量 8）
2. InFlight.lookup                      命中且 caps.tasks → 已有句柄（不进 Governor）
                                        命中且 !tasks → Throttled + 确认令牌（不进 Governor）
                                        _meta 带匹配令牌 → 放行，真的再打
3. Governor.acquire                     Reject / 排队超时 → Throttled
4. CountCircuit                         OPEN → Unavailable
5. Backend.call(BackendCall)，arm promote，InFlight.insert
                                        上游先回 → 同步 JSON
6. promote 到点                         caps.tasks → 句柄；否则 -32021
```

不变量 8 的测试：构造 `ir::Request`，Policy Deny，Governor / InFlight / Backend 计数桩仍为 0。不启 HTTP。

**不抽 interceptor。** 六步不是同构的（纯函数 / 查表 / 异步回调 / 定时器）。硬塞统一 filter 会让每个环节拿到最宽能力。等真正同构的横切点再抽。

根 span 在 Pipeline 开。Policy Deny 也要有短 span。P1：第 5 步传 `BackendCall`；工期紧可暂传 `Request`，Backend 仍不得读 `who`。

---

## 3. 身份、档位、切面、Grant

无 session：**每个请求自己带身份**。M1 无 `tenant`；span 的 `principal` 只用 `agentId`。

### 3.1 全序链，不是格

产品可以说「权限格」。实现是 **全序链**：`effectiveRank >= mcp.level`。

finance-admin 排在 github 后面就会顺带看见 github。这是建模天花板，不是换 Policy 实现能修的。升级到偏序 = DAG 可达性，Principal / BackendMeta / Policy / 测试全换。

**面试主动讲** M1 取全序链、偏序是已知天花板。

| 概念 | 是 | 不是 |
|---|---|---|
| Taxonomy | `access.levels` 全序；加载期 → Rank | 请求期字符串表；格 |
| Catalog | 已登记 MCP | 切面 |
| ToolIndex | last-known-good 快照 | 健康状态 |
| FacetView | ToolIndex ⨯ `visibilityFilter` | 第三条网关 |
| Grant | TTL；快照上读时 `now < expiresAt` | 改 YAML；loop 上读文件 |
| admin | 控制面布尔 | `levels` 里最高档 |
| Governor | 切面内并发；单进程 RAII | 第二套权限；已为多机留好的接口 |
| InFlight | 同身份同参数在途去重；确认不是 MRTR | 杀 SQL；跨 agent 合流；paramsHash 之外的「像不像」 |
| 可观测 | 一条 trace、一份 JSONL、几个计数 | Prometheus；自研 UI；采样策略 |

### 3.2 配置

出厂 `examples/perfacet.yaml`（一档，`level` 两处都省略）：

```yaml
listen: "0.0.0.0:8741"

access:
  levels: [default]

agents:
  cursor:      { token: "pf_cursor" }
  claude-code: { token: "pf_claude" }

backends:
  - name: echo
    url: "http://127.0.0.1:9001/mcp"
```

多档 `examples/perfacet.multi-level.yaml`（必须能跑剧本 3/4/5）：

```yaml
listen: "0.0.0.0:8741"

access:
  levels: [intern, engineer, finance-admin]
  elevation:
    max_level: engineer
    ttl_ms: 900000

agents:
  cursor:       { token: "pf_cursor_intern", level: intern }
  claude-code:  { token: "pf_claude_eng",    level: engineer }
  research-bot: { token: "pf_researcher",    level: intern }
  intern-bot:   { token: "pf_intern_bot",    level: intern }
  ops-admin:    { token: "pf_admin",         admin: true }

backends:
  - name: postgres
    url: "http://127.0.0.1:9002/mcp"
    level: intern
    idempotent_tools: [explain]   # query 不在列内。慢查询超时后重放会叠 SQL；见 §5.5
  - name: github
    url: "http://127.0.0.1:9003/mcp"
    level: engineer
  - name: payroll
    url: "http://127.0.0.1:9004/mcp"
    level: finance-admin
    secret: true
  - name: flaky
    url: "http://127.0.0.1:9005/mcp"   # python mock_mcp.py --fail-after 5
    level: intern
  - name: slow
    url: "http://127.0.0.1:9006/mcp"   # python mock_mcp.py --delay 30s
    level: intern
```

加载顺序（一档/多档同一套）：

1. `admin: true` 且省略 `level` → `hasLevel = false`（优先，与档数无关）
2. 一档且省略 `level` → Rank 0，`hasLevel = true`
3. 多档且省略 `level` → 拒绝启动
4. 写了 `level` → 解析；不在 `levels` 里 → 拒绝启动

纯 admin：`perfacet__upstream_status` 可见；业务 call 一律 Authz；`perfacet__request_elevation` 可见但调用失败（没有可抬的档）。要调业务必须同时写 `level`。

启动即失败（日志带名字）：拼错 level、多档缺引用、删档仍被引用。加档的 diff 只有 `levels` 多一个名字。

治理段（同一文件；key = `ToolKey::str()`）：

```yaml
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
    rate_per_sec: 20          # M1 不实现
  tools:
    postgres__query:
      max_concurrency: 3
      queue_wait_ms: 8000
    github__search:
      max_concurrency: 2

tasks:
  promote_after_ms: 2000
  ttl_ms: 3600000

otel:
  endpoint: "http://127.0.0.1:4318/v1/traces"
  service_name: perfacet
  queue_max: 1024          # 满则丢，otlp_dropped++。禁止 loop 上 POST

audit:
  path: "audit.jsonl"      # worker append。禁止 loop 上写
```

`governor.tools`：**只校验已成功 list 的 backend**。不命中 → WARN + `perfacet status` 显示未生效，不退出。从未 list 成功的标 `unvalidated`，**不阻塞 listen**（flaky 启动即 DOWN 不能拖死网关）。第一次 list 成功时再校验。拼错不能静默当成无限制。

M1 **无热加载**。改 YAML 后重启。不要 `perfacet reload`。

### 3.3 演示剧本（`examples/demo.sh`，CI，7 步）

0. 出厂一档：两个 token 的 `tools/list` 都是全集
1. 换成 multi-level：intern 看不见 payroll / github
2. intern 猜 `payroll__x` 与拼错同错
3. intern 调 `perfacet__request_elevation`（同步 `grantId`，不是 task）→ `perfacet grant approve --id` → **sleep 0.2** → list 出现 `github__*` → 到期读时消失（`ttlMs` 变短）
4. 四个带 level 的身份打 `postgres__query`（max=3）：第四个 FIFO 排队，到点 Throttled（不升格 task）
5. 打挂 flaky：DOWN + OPEN；intern 的 list 里 `flaky__*` 仍在，call 为 Unavailable；payroll 仍 unknown
6. 一条 `github__search` 的 trace 在 Jaeger 里拆出 gateway / upstream / total；同一 `trace_id` 出现在 `audit.jsonl`

旁路镜头（不进 0–6 编号）：
- slow 默认不声明 tasks → promote 到点 `-32021`；另起 `--delay` 且声明 tasks 的实例验升格
- **在途去重（为 agent 超时后叠打 SQL）**：同一 intern 对 slow 再打一次相同 params。不声明 tasks → Throttled 正文含在途参数摘要 + `if_*`，upstream 计数仍为 1；带匹配 `_meta.perfacet/confirm` 才放行第二条。声明 tasks → 返回已有 `taskId`，不新建上游调用。params 不同则不命中，走 Governor。`perfacet status` 的 `inflight_hit` ≥ 1；audit 有 `event=inflight_hit` 且 `trace_id` 对得上

### 3.4 规则链

`tools/list` / `tools/call` / `tasks/*` 同一套。grants 已折入 `effectiveRank`。

1. 未认证 → 401
2. 内置 `perfacet__*` 不参与 Rank：
   - `perfacet__request_elevation`：已认证可见；`hasLevel == false` 则调用失败；成功则同步写 pending、返回 `grantId`
   - `perfacet__upstream_status`：仅 `admin: true`
   - 其它默认仅 admin
3. 业务工具 + `hasLevel == false` → Deny
4. 业务工具 + `effectiveRank < mcp.level` → Deny；`secret` 则对外 unknown
5. 否则 Allow

控制面（`GET /upstreams`、CLI grant）看 `admin`，不看 Rank 是否最高。

### 3.5 鉴权

每个 agent 一块 token。可在另一台机器：

```json
{
  "mcpServers": {
    "perfacet": {
      "type": "http",
      "url": "http://192.168.1.10:8741/mcp",
      "headers": { "Authorization": "Bearer pf_cursor_intern" }
    }
  }
}
```

无 token / 未知 token → 401。token 只出现在 YAML `agents:`。薄的是 token；厚的是始终存在的 Rank 切面。

### 3.6 Grant

Agent 不能 `set_level`。`bump_to` 必须在 `levels` 且 Rank ≤ `elevation.max_level`。

M1 不走 MRTR。申请是普通 `tools/call perfacet__request_elevation`。批准：

```
perfacet grant approve --id <grantId>
# 或直接：perfacet grant approve --agent cursor --bump engineer
```

CLI append 同一份 `grants.jsonl`。worker ≤100ms `stat` + 解析，原子 swap `shared_ptr<const GrantTable>`。loop 上 `effectiveBump` 只读快照 + `now < expiresAt`。

- **过期消失**是 `now` 比较，不依赖 reload（不变量 6）
- **新 Grant 出现**最多一个刷新周期——demo 里 `sleep 0.2`
- **禁止** IO loop 上 `stat` / 解析该文件（和 OTLP 一样，阻塞 I/O 不准进 loop）
- 最高档不能自动批自己的提权：CLI 假定操作员是 admin

`tools/list` 的 `ttlMs = min(默认, 该身份最短有效 Grant 剩余)`。`cacheScope` 恒 `private`。

### 3.7 不变量

1. 每次请求都走 Policy，没有「未配就跳过」
2. list 与 call 同一套 Policy（list 走 `visibilityFilter`）
3. 出厂一档：已认证且 `hasLevel` 的 list = ToolIndex 全集
4. secret 不够格：list 无、call 为 unknown
5. `cacheScope` 恒 `private`
6. Grant 过期由读时 `now < expiresAt` 保证（loop 无锁读快照）；`ttlMs` 不超过最短剩余。新 Grant 允许一个刷新周期延迟
7. 别人的 `taskId` 失败
8. Policy Deny 不进 Governor、不进 InFlight、不打上游、不重试（Pipeline 第 1 步；纯内存测）
9. 可见性只由 Rank 决定，与 Health/Circuit 无关；OPEN 时切面内 list 仍在、call 为 Unavailable
10. 审计 + span：authenticate 失败、deny、throttled、approve、expire、circuit_open、inflight_hit 都能对上 `trace_id`。落点是 `audit.jsonl`（worker append），不是「有个 AuditLog.h」。档位名只用 `levelName`；`principal` = `agentId`；禁止写 token
11. 不在 `idempotent_tools` 里的 Timeout 不重试。在途未完成的 Timeout 亦不重试（与是否幂等无关：上一枪还在跑）
12. `Cancelled` = 网关放弃等待，不保证远端终止
13. 未知 level 名拒绝启动
14. 多档时缺 `level` 拒绝启动（纯 `admin: true` 除外）
15. `admin` 不是一档；不能靠最高档自动批 Grant
16. `hasLevel` 默认 false
17. `governor.tools` 与 `ToolKey` 一致；只校验已成功 list 的 backend；未校验不阻塞启动
18. Permit 只由析构归还；无公开 `release`；实现持 Governor* + ToolKey
19. 未声明 `tasks` 永不收句柄。promote 到点 → `-32021` + `data.requiredCapabilities`，HTTP 400。只判 `Request.caps.tasks`。在途命中复用已有句柄不算「给新句柄」
20. ToolIndex 冷 → 不得返回可长期缓存的空列表
21. 纯 admin 调用 `perfacet__request_elevation` 失败
22. 排队不建 task；`promote_after_ms` 只在已发出 `Backend.call` 之后 arm
23. 同 `(agentId, ToolKey, paramsHash)` 在途且未带匹配 confirm 令牌 → 不发第二条 `Backend.call`。声明 tasks → 复用已有 `taskId`；未声明 → `CallToolResult` `isError` + Throttled，正文告知在途参数与一次性令牌。令牌绑 `inflightId`，`~Call` 摘除即失效。确认不是 MRTR，发生在 agent 自己的下一次 `tools/call`
24. IO loop 不写 `audit.jsonl`、不 POST OTLP。两者进 worker；OTLP 队列满则丢并 `otlp_dropped++`，不阻塞 loop、不重试打满 Jaeger

---

## 4. 接入与目录

Catalog 登记 HTTP endpoint，不是进程。`type: mcp` → `HttpMcpBackend`。stdio 在网关外转 HTTP。不做镜像、compose、健康重启、沙箱、进程内 Stdio backend。

Agent 始终只连 Perfacet 一条 URL。注册是 YAML 加一条 `backends:`（url + 引用 `level`）。`levels` 里必须已有这个名字。

```yaml
backends:
  - name: slack
    url: "http://10.0.0.12:3100/mcp"
    level: intern
    idempotent_tools: []
```

```bash
mcp-proxy --port 9100 -- npx -y @modelcontextprotocol/server-filesystem /tmp/kb
# YAML：url: http://127.0.0.1:9100/mcp
```

启动后网关自动做：

1. `Catalog.add` → `HttpMcpBackend(url)` + 已解析的 `BackendMeta`
2. `ProbeHealth` → `onProbeResult`；`IndexRefresher` 成功则写 ToolIndex，失败不抹 last-known-good
3. 校验该 backend 的 `governor.tools`（见 §3.2）
4. `FacetView(who)` = ToolIndex ⨯ Policy
5. DOWN / OPEN 不从 ToolIndex 删工具

不要让 agent `register`。登记是控制面。进行中的 task 绑旧 Principal。M1 进程死则内存 task 没了，agent 重试。

本仓库 RPC **不是主语**。M1 只做 `HttpMcpBackend`。其后可加 `RpcBackend`（unary + 有限 schema），定位仍是 Backend 的第二个实现。

| ToolIndex | 决定 |
|---|---|
| 何时 list | 首轮 probe，之后 `max(health.interval_ms, 上游 ttlMs)`；走回调 |
| 缓存 | last-known-good 直到下次成功；失败不抹 |
| DOWN 时切面 | **仍在。** call 才 Unavailable |
| 冷启动 | **listen 前等首轮 probe**。禁止对合规客户端发长 `ttlMs` 空列表 |

对 agent 的 `ttlMs` 与上游刷新间隔是两件事。不变量 20。

---

## 5. 请求路径上的治理

### 5.1 Backend

`HttpMcpBackend`：worker 上 **cpp-httplib 阻塞 client** `POST /mcp`；带 `traceparent`；写 `upstreamMs`；完成 post 回 loop。面试直说：上游 client 不是要证明的东西。bench 测 `total − upstream`。

P1：`call(ir::BackendCall)`，不含 Principal。

取消：能转发 `tasks/cancel` 则转发；否则标 `Cancelled` 并停止等待。**不保证远端终止。**

Backend 不自己重试、不自己熔断。

### 5.2 Tasks

挂的是**已经打到上游、还没完**的调用，不是排队号。排队堵这次 HTTP（秒级）。提权不走这条树。

```
working → completed | failed | cancelled     # M1 无 input_required

tools/call（已在切面内）
  ├ Reject → Throttled
  ├ Queue  → 只挂 queue_wait_ms（无 task）
  └ 执行（Permit 在手，Backend.call 已丢到 worker，arm promote）
         ├ 上游先回 → 同步 JSON，取消定时器
         ├ promote 且 caps.tasks → MemTaskStore 句柄；Call 摘下；Permit 仍占槽
         └ promote 且 !caps.tasks → 立刻 -32021；HTTP 结束；Permit 仍持到上游回调
```

升格后槽绑的是在途上游，不是 agent HTTP。`-32021` 同理。

promote 到点 = 必须靠 `CreateTaskResult` 才能继续服务这次 HTTP。未声明则 MUST 拒绝，不是干等 timeout。deadline 打在 promote **之前**仍是真 Timeout。

不是「预计将超过就提前升格」，更不是排队时升格。DEGRADED 时 arm 那一刻 `promote_after_ms` 减半（一行；Week 3 可删）。

M1：`MemTaskStore`，不 fsync、无 WAL。网关死则内存没了。`~Call()` 覆盖六条终态。

`remoteTaskId` re-attach、孤儿 → `outcome_unknown`：**割线后**。面试讲 MUST 与边界，比做完 WAL 更像判断力。

### 5.3 健康、熔断、分类、重试

管理员视图：每台上游 UP / DEGRADED / DOWN + latency。

| 机制 | 行为 |
|---|---|
| Health | 按 interval probe（默认上游 `tools/list`），`onProbeResult`。EWMA ≥ 阈值 → DEGRADED；连续失败 → DOWN。失败不删 last-known-good |
| Circuit | `CountCircuit`：连续失败 → OPEN → cooldown → HALF_OPEN。OPEN **不**改 FacetView |
| classify | `ir::classify`。Deny 永远 Authz；缺能力是网关自己的 Capability |
| Retry | 仅 `retryable` 且未过 deadline 且未 OPEN。`Authz` / `Throttled` / `Cancelled` / `Protocol` / `Capability` 永不重试。Timeout 还要看 `idempotent_tools`；**在途未完成禁止当可重试 Timeout**（上一枪还在跑，幂等前提不成立） |
| Cooldown | OPEN 期间打到 Circuit 即失败，计入 span，不再锤上游 |

探测走独立预算，不占 governor 配额。DEGRADED 仍放行。DOWN / OPEN：切面内 call → Unavailable；list 不变。

### 5.4 Governor

| 维度 | 含义 |
|---|---|
| per-tool | 例如 `postgres__query` 全局最多 3 把 |
| per-principal | 单身份同时在途上限 |
| queue | 超限 FIFO，回调 `onAdmit` |
| timeout | `queue_wait_ms` 内拿不到 → Throttled，出队。保持秒级 |
| rate | YAML 占位；M1 不实现令牌桶 |

`acquire` 成功才碰 Circuit / Backend。Permit 进 `Call`。无公开 `release`。

**Permit 的 RAII 归还是单进程假设。** 跨进程要租约 + 续约 + fencing：现在的签名表达不了。面试：抽象隔离了准入逻辑；分布式是接口演进，不是实现替换。

不做优先队列。排队与 Tasks 互斥：队列里没有 task。

### 5.5 在途去重

**问题：** agent 调 MCP 背后一条超慢 SQL；60s 后 agent 当工具失败再打一次，库里第一条还在跑。Tasks 只拆 HTTP 连接，不杀远端；`Cancelled` 不保证终止（不变量 12）。网关能做的是 **重试不放大**：N 次重试最多对应已在途的那一条，除非 agent 明确确认再打。

**不新抽虚接口。** `InFlight` 是具体类，挂在 `Call` 旁边。Deny 之后、`Governor.acquire` 之前查一次。

```
key   = (agentId, ToolKey, paramsHash)     # paramsHash = params.dump()；nlohmann object 有序，dump 确定性够用
value = { inflightId, startedAtMs, taskId? }
```

`Backend.call` 发出时 insert；`~Call()` 与 Permit 一起摘除。无第二套生命周期。

命中且 `_meta` **没有**匹配的 `perfacet/confirm`：

| 客户端 | 行为 |
|---|---|
| `caps.tasks` | 返回**已有** `taskId`（`working`）。它该 poll，不是再开火。复用句柄，不是给新句柄（不变量 19） |
| 未声明 tasks | `CallToolResult` `isError: true`，`FailureClass::Throttled`。正文写人话：在途工具名、参数摘要、已运行时长、一次性令牌 `if_*`。不进 Governor、不打上游 |

命中且 `_meta.perfacet/confirm` == 该条 `inflightId`：放行，走 Governor，真的再打一条。`max_concurrency` 仍是硬顶。

令牌规则：

- 放 `_meta` 不放 `params`。造 `BackendCall` 时剥掉，不转发给上游、不进 tool schema
- 一次性，绑 `inflightId` 不是 `force: true`。那条跑完旧令牌失效，agent 没法养成永远带 force
- **不是 MRTR / `input_required`。** 这次调用正常结束；二次确认是 agent 自己的下一次 `tools/call`

边界（写进面试）：

- 确认之后仍可能叠 SQL——机制是让 agent 有机会做对，保证在 Governor
- params 里有时间戳 / 注释则哈希不同，退化成只有 Governor 兜底。固有上限，不补「像不像」
- 只在同一 `agentId` 内去重。跨身份合流要先回答不同 Rank 能否看同一份结果，M1 不碰
- 已经打到库里的那条仍靠 MCP/`statement_timeout`。目标写成「重试不放大」，不写成「超时后库干净」

span：`inflight_hit=true`，`inflight_id`，无第二条 upstream span。

### 5.6 可观测

秋招够用三件，不是监控产品。不做 Prometheus、`/metrics`、Grafana、采样、`perfacet trace show`、自研 UI。

**1. 一条 trace（已有主语）**

```
Agent → Perfacet → MCP Server → External API
```

| 字段 | 来源 |
|---|---|
| `trace_id` / `span_id` | `traceparent` 或网关生成 |
| `principal` | `agentId`（不要写 token） |
| `level` | `levelName`（判定用 Rank） |
| `tool` | `ToolKey::str()` |
| `server` | catalog 名 |
| `task_id` | 无则空 |
| `inflight_id` | 无则空；合流命中打 `inflight_hit` |
| `latency` / `status` | 该 span；`FailureClass` |

手写 OTLP/HTTP JSON ~200 行打 Jaeger `4318`。**禁止 opentelemetry-cpp。** loop 上只入队；worker POST。`queue_max` 满则丢，`otlp_dropped++`。Jaeger 不在不得拖死网关。debug 可并列 JSONL。

**2. 一份审计（让不变量 10 可验收）**

`JsonlAuditLog`：loop 投递一条 struct，worker append 一行。和 Grant 同一条纪律。事件集合闭：

`auth_fail` / `deny` / `throttled` / `inflight_hit` / `circuit_open` / `grant_approve` / `grant_expire`

一行字段 = 上表 + `event` + `ts_ms`。禁止 token。demo：`github__search` 的 `trace_id` 在 Jaeger 和 `audit.jsonl` 对得上；在途去重旁路能 grep 到 `inflight_hit`。

**3. 几个计数（admin 视图，证明治理在干活）**

`Counters`：原子量。热路径只 `fetch_add`。Frontend 只读快照，给 `GET /upstreams` / `perfacet status` 的 `observe:` 块。`GET /healthz` 仍不含明细。

| 名 | 类型 | 为什么有 |
|---|---|---|
| `inflight_hit` | 累加 | 挡住了几次重复 SQL |
| `inflight_confirm` | 累加 | agent 真的确认再打 |
| `throttled` | 累加 | Governor 在干活 |
| `circuit_open` | 累加 | 熔断触发次数 |
| `otlp_dropped` | 累加 | 队列满丢了多少 span |
| `permit_held` | 瞬时 | 与 Permit 析构归零对账 |
| `inflight_held` | 瞬时 | 与 `~Call` 摘除对账 |

没有 QPS 面板。`calls` 不单开——需要看吞吐走 bench。

span：`inflight_hit=true` 时无第二条 upstream span，与计数 `inflight_hit++` 同一条路径。

---

## 6. 工程

### 6.1 线程

| 角色 | 数量 | 职责 |
|---|---|---|
| IO loop | 1 | agent HTTP（llhttp）、定时器（probe / cooldown / promote）。Grant TTL **不**靠定时器判定 |
| Worker 池 | M | httplib、完成 post 回 loop、OTLP POST、Audit/Grant 文件 I/O |

无 WAL 线程。停机：拒新请求、队列里的等或 Throttled、在途有上限。已转发的 cancel 仍是放弃等待。M1 内存 task 随进程消失。

Frontend：首轮 probe 完成后再 accept。`POST /mcp` 造 `ir::Request` → `Pipeline.handle` → 写回。`GET /healthz` 不含各 MCP 明细。`GET /upstreams` / `perfacet status` 要 `admin`，带 `observe:` 计数。M1 无 TLS。

**不**在 Frontend 里调 Policy / Governor / Circuit / Backend / InFlight / promote。admin 视图只读 Counters / Health 快照。Audit / OTLP 不从 Frontend 直接写。

### 6.2 现成轮子

C++ 岗资产是 EventLoop / TcpServer / Pipeline / Governor，不是 HTTP 状态机。

| 不要自己写 | 换成 |
|---|---|
| HTTP/1.1 报文 | **llhttp**（或 picohttpparser） |
| 上游异步 HTTP client | **cpp-httplib** 阻塞 + worker |
| JSON | **nlohmann/json**（别碰 simdjson） |
| YAML | **yaml-cpp** |
| CLI | **CLI11** |
| 测试 | **doctest + CTest** |
| C++ mock_mcp | **Python FastAPI/aiohttp** ~60 行 |
| OTel SDK | 手写 OTLP/HTTP JSON；有界队列 |
| Task WAL / 自研 record | `MemTaskStore`；Grant / Audit 用 JSONL append |
| Prometheus | 不做。计数挂 `perfacet status` |

本仓库只借 EventLoop / TcpServer / Buffer / Endpoint，以及 TaskTree 的取消与 deadline、GOAWAY 排空。**不用** TcpClient 打上游。

### 6.3 目录

```
perfacet/
  include/perfacet/
    ir/          Request.h ToolKey.h ClientCaps.h classify.h
    pipeline/    Pipeline.h Call.h InFlight.h
    catalog/     Catalog.h ToolIndex.h FacetView.h IndexRefresher.h
    govern/      Governor.h LocalGovernor.h
    policy/      YamlIdentityStore.h Taxonomy.h Policy.h JsonlGrantStore.h YamlConfig.h
    health/      Health.h CountCircuit.h RetryPolicy.h
    backend/     Backend.h HttpMcpBackend.h
    task/        Task.h MemTaskStore.h
    observe/     Tracer.h OtlpHttpJsonTracer.h Counters.h
    frontend/    HttpMcp.h
    audit/       JsonlAuditLog.h
    cli/         main_cli.h
  third_party/                   # llhttp, nlohmann, cpp-httplib, CLI11, doctest
  scripts/check_layers.sh
examples/
  perfacet.yaml
  perfacet.multi-level.yaml
  demo.sh
  mock_mcp.py                    # --port --tools --fail-after --delay
tests/
  invariants/                    # §3.7；不变量 8 / 23 纯内存
  ir/                            # ToolKey a__b__c
  governor/
  inflight/                      # 同 params 合流；带令牌才第二条；Deny 不进表
  observe/                       # 不变量 10：event 对得上 trace_id；Deny 也写审计
bench/
```

M1 无 `StdioMcpBackend`、`Promoter`、`InlineExecutor`、`Classifier` 虚接口、`JsonlTaskStore`、C++ mock、Prometheus。

### 6.4 扩展性怎么验收

| 新增 | 允许改 | 不许改 |
|---|---|---|
| 换身份源 | `YamlIdentityStore`（抽接口） | Taxonomy / Policy / Governor |
| 用户加一档 | `levels` 多一个名字 | C++ |
| 切开切面 | 连接写上 `level` | C++ |
| 收回一档 | `levels: [default]` | C++；Policy 仍在 |
| 拼错 / 删档仍引用 | —— | 必须拒绝启动 |
| 改并发上限 | YAML governor | Policy |
| 换熔断 / 探测 / OTLP | 对应实现 | Backend / ToolIndex / IR 字段名 |
| 换审计 sink | `JsonlAuditLog`（抽接口） | 事件集合；span 字段名 |
| 接入新 MCP | YAML 一条 | frontend / Pipeline / Policy；agent 的 mcp.json |
| 接入 stdio | 网关外 mcp-proxy | 进程内 Stdio |
| 其后接 RPC | `RpcBackend` 子类 | 产品主语 |
| Task 持久化 | `MemTaskStore` → JSONL/SQLite | Pipeline / Call / promote |
| 关掉在途去重 | 删 `InFlight.lookup` 那一支 | Governor / Policy / Tasks 语义 |

---

## 7. 范围切分

**M1 必须做**（进 3 周、进 demo.sh）：切面、Grant 快照无锁读、Pipeline 六步、promote + `-32021`、MemTaskStore、InFlight 去重 / 确认令牌、Governor FIFO、Health/Circuit、手写 OTLP（有界队列）、JsonlAuditLog、Counters、Python mock。

**P1（有余力，不挡 demo.sh）：** `BackendCall`；CMake per-module target；`check_layers.sh` token 白名单按脚本注释落地。

**割线后（不挡投递）：** bench 数字；Retry 细测；`AuthenticatedPrincipal`；Task 持久化 / 孤儿 `outcome_unknown` / `remoteTaskId`；MRTR；偏序 DAG；SSE；下游鉴权头；`RpcBackend`。

**明确不做（按类）：**

| 类 | 不做 |
|---|---|
| 产品 | Docker 式养活进程；多租户；Web UI；OAuth / 文档 ACL / OPA；Agent 自助改 level；优先队列；令牌桶；Prometheus / `/metrics` / Grafana；`perfacet trace show` |
| 协议 | 旧版 / 双栈；stdio 前端；给未声明客户端 task 句柄；干等 timeout 代替 `-32021`；把确认做成 MRTR / `input_required` |
| 单实例 | 多副本；把 Permit RAII 说成已为分布式留好 |
| 热路径 | loop 上 stat/解析 grants；loop 上写 audit / POST OTLP；Frontend 编排主流程 |
| 过早抽象 | Classifier/Retry/Executor/Promoter 虚接口；Identity/Grant/Task/Circuit M1 虚接口；interceptor chain；Health 直接写 ToolIndex；n 次 authorizeCall 做 list |
| 轮子 | 手写 HTTP 状态机；自研上游异步 client；opentelemetry-cpp；simdjson；C++ mock_mcp；自研 WAL record |
| 语义 | 把 Cancelled 当成远端已死；DOWN 时从 list 拿掉工具；冷启动长 ttl 空列表；非幂等 Timeout 重试；在途未完成当可重试 Timeout；用 Circuit 管慢查询；跨 `agentId` 合流在途结果 |

---

## 8. 三周排期与测试

第 3 周末 **`examples/demo.sh` 必须绿**。主线：出厂一档全可见；换 multi-level 切面正确；4 抢 3；上游挂了熔断而 list 不变；一条 trace 拆开 gateway/upstream 且 `audit.jsonl` 对得上 `trace_id`。

| 周 | 必须 | 验收 |
|---|---|---|
| 1 | Python mock + IR（ToolKey / ClientCaps / classify）+ yaml-cpp fail-closed + Identity + Grant 快照 + Policy（`visibilityFilter`）+ Pipeline 骨架 + EventLoop/TcpServer + llhttp + Catalog/FacetView/ToolIndex + listen 前首轮 probe + check_layers.sh + doctest（含 `a__b__c`） | 两 Bearer 一档全集；拼错拒绝启动；冷启动不空缓存；不变量 8 纯内存；loop 上 Grant 零 syscall |
| 2 | HttpMcpBackend（httplib + worker）+ MemTaskStore + InFlight + CLI Grant | slow 声明 tasks → 句柄；不声明 → `-32021`；同 params 再打 → 合流 / Throttled+令牌；带令牌才第二条；剧本 3 Grant |
| 3 | Governor + Permit pImpl + Health/Circuit + IndexRefresher + 手写 OTLP（有界队列）+ JsonlAuditLog + Counters + demo.sh | **0–6 绿** + 在途去重旁路；audit 对得上 `trace_id`；`inflight_hit` ≥ 1；flaky DOWN 网关仍起；4 抢 3；Permit / InFlight 析构归零 |

| 指标 | 目标 |
|---|---|
| 网关附加延迟 p50 / p99（total − upstream） | p99 < 1ms（本机 mock） |
| 单进程吞吐 | 5–10k RPS |
| Governor 4 抢 3 | FIFO，计数 |
| 1000 在途 task | 内存有上限 |
| Permit / InFlight 泄漏 | 混合终态 10 万次，计数器归零 |

`gatewayMs` / `upstreamMs` 第 1 周打点。bench 数字可在割线后补。OTLP / Audit 必须先离开热路径。不变量 10：构造 Deny，审计桩有一条且含 `trace_id`，不启 HTTP 也可测。

---

## 9. 简历与面试

简历：

> **Perfacet** —— Multi-Agent Tool Gateway（C++17，MCP 2026-07-28）：一个身份，一个切面。已有 MCP 按 endpoint 接入，不负责拉起与保活。权限档位是符号表：`access.levels` 唯一声明，加载期解析成 rank，请求期整数比较。出厂一档切面等于全集；加档不改 C++。提权是 CLI Grant + TTL。共享工具做 per-tool / per-principal 并发与 FIFO。对不会 Tasks 的客户端代实现长调用（promote 摘连接；未声明能力返回 `-32021`）。同一身份同一参数的在途调用合流或一次确认，避免 agent 超时重试叠打上游。调用链手写 OTLP/HTTP JSON；审计 JSONL 与 span 同 `trace_id`；admin status 暴露在途命中与 OTLP 丢弃。网关自身单实例。

不要写：网络库、又一个 MCP 网关、自研 Docker、分布式网关。

面试主动讲（细节在上文，这里只留判断）：

1. 不是 Docker：探测 ≠ 养活；stdio 外置同理
2. 切面之外必须有 Governor：多 agent 时工具是稀缺资源
3. Policy vs Governor 不能反；出厂一档仍走 Rank
4. 薄的是 token，厚的是档位模型
5. secret 后端 DOWN 不能告诉 intern
6. 排队不是 Task；Tasks 挂在途调用。在途去重也不是 Task，确认不是 MRTR
7. 全序链不是格；偏序是已知天花板
8. Permit RAII 是单进程假设；分布式要改签名
9. Pipeline 六步不抽 interceptor：它们不是同构的
10. list 走 `visibilityFilter`，否则 OPA 是 n 次求值
11. Grant、Audit、OTLP 一样，阻塞 I/O 不准进 loop；OTLP 满队列丢
12. `-32021` 跟现行 schema，不是 overview 上的 `-32003`
13. MemTaskStore 是知情缺口：MUST 知道，单实例崩了 agent 本来就要重试
14. 虚接口 5 个；其余具体类 + 一行注释
15. mock 用 Python、解析用 llhttp：要证明的是 loop / 切面 / 治理
16. Backend 不认身份（P1：`BackendCall`）；透传必须写成字段。confirm 令牌在 `_meta`，造 `BackendCall` 时剥掉
17. 网关杀不了库里的 SQL；能保证的是 agent 超时重试不放大。`query` 不进 `idempotent_tools`；在途 Timeout 不重试
18. 观测三件套不是监控产品：一条 trace、一份 JSONL、几个原子计数。热路径只 bump
