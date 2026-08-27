# Perfacet 应改事项

对照 `SPEC.md` / 范围文档。只列要改的；实现仍以规格为准。

## P0

### 失败探活不得关掉已 OPEN 的熔断

`ProbeHealth` 失败但还没到 `down_after_failures` 时 state 仍是 Up/Degraded，`Gateway` 见此就 `onProbeSuccess()`，`CountCircuit` 会把 Open/HalfOpen 打成 Closed，业务刚熔断会被下一次失败 list 探活立刻合闸。只在探活成功时合闸；OPEN 只走 cooldown → HALF_OPEN。补单测：失败探活不合闸。

### promote / `-32021` 之后禁止再打上游

`shouldRetry` 在 `responded_` 之前，`inflightAttemptOutstanding` 还写死 `false`，客户端已拿到句柄或 `-32021` 后第一次 httplib 超时仍会 `fireAttempt()`，重试会放大。`responded_ || cancelled_` 时禁止 retry；超时/取消当作上一枪未完成（`true`），把 `retry_test.cpp` 已覆盖的分支接到 Pipeline。

### 在途命中复用已有 `taskId`，禁止第二条路径 insert

第一次 call 已发出、尚未 promote 时 `hit->taskId` 为空，第二次带 `caps.tasks` 会新 insert 一个 `tsk_*`，第一次 `onPromote` 再造一个，两个客户端看到不同句柄。命中且无 id 时在原 Call 上 `attachTask`（或等到第一次 promote）。补 Pipeline 测试（不变量 19/23）；`demo.sh` 跑声明 tasks → 已有句柄。

### Grant 的 append/refresh 离开 IO loop

`perfacet__request_elevation` 在 Pipeline（loop）里同步 `ofstream` append，`refreshOnWorker` 持锁全量 parse 且不比 mtime，loop 上 `effectiveBump` 会等同一把锁，违反「loop 禁止 open grants」。append 投 worker，loop 只换内存快照；解析在锁外再 swap；`lastMtimeNs_` 未变则跳过。补 loop 零 syscall / 不变量 24（`JsonlAuditLog` 已 include cassert 但没用）。

### HTTP 粘包只消费已解析字节，写回后再喂 parser

`llhttp_execute` 后把 `readableBytes()` 全部 retrieve，`on_message_complete` 不 pause，两包会串；`busy` 时留下缓冲但 `doneWrite` 不再 parse，pipelining 会挂。pause、只 retrieve 已解析部分、写回后缓冲非空立刻再 parse。

### 剧本 6：同一 `trace_id` 拆 gateway / upstream / total，并进 `audit.jsonl`

现在只有根 span `gateway`，`demo.sh` 只检查 observe 有键，规格要的 Jaeger 三截和对账没做。Backend 完成时打 upstream span（同一 `traceparent`）。成功审计、`task_id`、`auth_fail` 的 `trace_id` 见下面三条；三条不齐则剧本 6 仍对不上。若决定不做满，先改规格和简历口径，不要假装已验收。

### 成功 `tools/call` 写审计，才能对上 `github__search` 的 `trace_id`

事件集合是闭的（`auth_fail` / `deny` / `throttled` / `inflight_hit` / `circuit_open` / `grant_*`），成功调用不进 `audit.jsonl`。剧本 6 要求这条 trace 在 Jaeger 和审计里对得上，现在成功路径无行可对。成功 call 至少打一条带同一 `trace_id` 的事件（扩集合或单独 `ok`），`demo.sh` 对 `github__search` 真 grep，不要只查 observe 有键。

### span 补上 `task_id`

规格 span 字段含 `task_id`，热路径只 `set` 了 `inflight_hit` / `inflight_id`。promote 出句柄或复用已有句柄时写进去，Jaeger 才能把升格和那次 call 对上。

### `auth_fail` 带上 `trace_id`

401 时已有 body / `traceparent`，`HttpMcp` 审计却空着 `trace_id`，和 span 对不上。生成或继承后再 emit（`/mcp` 与 `/upstreams` 都要）。

### 停机按 GOAWAY：停 accept、取消在途、再 quit

`requestStop` 只拒新 `/mcp`、队列 Reject，然后超时 `quit()`，不停 accept、不对在途 `cancelWait()`，agent 看到断连而不是 Cancelled。drain 时 cancel 在途，确认 `~Call` 仍归还 Permit/InFlight。

## P1

### 第 4 步 OPEN 拒绝也要 `circuit_open++`

现在只在 `onFailure` 转入 OPEN 时加一，status 变成「熔断了几次」而不是「挡住了几次」。OPEN 期间连打 N 次应加 N。

### `MemTaskStore` 按 TTL 回收，满则拒新升格

`ttlMs` 只回给客户端，task 永远留在 map，长跑内存单调涨。读时过期或定时扫；上限满返回 Capability/Unavailable。

### 收到 `Mcp-Session-Id` / `Last-Event-ID` 回 400 Protocol

硬约束是当未知头拒绝，实现当普通 POST，旧 session 客户端会以为支持。`initialize` 保持未知方法即可。

### `Permit` 自定义 move-assign：先释放再接管

`= default` 只移 `unique_ptr`，被覆盖的 Impl 不走 `~Permit`，槽位回不来。热路径几乎不赋值，但 `permit = {}` 以后会踩。

### HTTP body 与 worker 队列设上限

`onBody` 无限 append，`ThreadPool` 的 deque 无界；慢上游占满 4 worker 后 probe/OTLP/grant/audit 全堵，内存可被打爆。超限 413；队列满对 agent Unavailable，观测可独立队列或丢。

### 内置工具 list 也走 `visibilityFilter`

`FacetView` 里 `request_elevation` 只要 `agentId` 非空就塞进列表，和 call 用的不是同一套 Policy。现在碰巧一致，改规则时 list/call 会分叉。

### 目录刷新间隔吃 `max(health.interval_ms, 上游 ttlMs)`

`ProbeHealth::startTimer` 只用配置的 `interval_ms`，上游 list ttl 变长仍按短间隔狂打。

### 先改 `classify` 再给 Backend 用

表会把上游 `-32021` 标成 Capability，规格禁止冒充本网关能力错误；Backend 手写 klass 反而是对的。加「是否网关自身」或改表，接上之前不要直接调用。

## 测试与 demo

### 补规格已要求、现在没锁住的测试

`tests/inflight` 只有 map lookup，没有「带令牌才第二条」。Governor 测了排队超时，没测放槽后第四个 FIFO Go。Policy 缺 secret→unknown、纯 admin 无业务、一档全集。不变量 10 只测了 Deny。frontend 不在 `perfacet_tests` 里，协议头（缺版本、406、session 头）无单测。随对应修复一起补，不启 HTTP 能测的不要去启。

### `demo.sh` 按范围 §3.3 真跑旁路和剧本 3/6

0–5 骨架在，但剧本 3 只断言 `ttlMs <= 5000`（15min Grant 时恒成立），没有「到期消失」；旁路 `-32021` / 升格句柄 / confirm 第二条没跑。`mock_mcp.py --declare-tasks` 写入后从未读取，要接上。在途去重：upstream 计数仍为 1、`inflight_hit≥1`、audit 的 `trace_id` 对得上。

## 秋招 bench（现有只有附加延迟 + echo RPS）

`bench/` 只有 in-process 附加延迟和 HTTP echo 吞吐。面试官会觉得 0 ms「没走 HTTP」，6k RPS 也撑不住切面/治理/去重这几句产品话。不要再刷 64/128 并发 echo，也不要对 gRPC 比倍数。数字写入 `bench/README.md` 实测值，简历不要写「5–10k 已达成」。

### 网关税：直连 mock vs 经 Perfacet

同一 `echo`、同一并发，并排 RTT / RPS。in-process 的 0 ms 没走 HTTP；本机单并发经网关 p50 已约 0.4 ms，缺「直连」对照就讲不清切面 + 治理加了多少税。

### Governor 4 抢 3

规格有打点、bench 没有。4 个身份同时打 `max=3` 的慢工具（`--delay`）：在途是否始终 ≤3、第四个是排队还是 Throttled、放槽后是否 FIFO、`throttled` / `permit_held`。比 6k RPS 更像项目差异化。现有 Governor 单测偏超时 Reject，没锁 FIFO 出队。

### InFlight「重试不放大」

同一 intern、相同 params 叠打 N 次（模拟 agent 超时重试）。记客户端 N 次 vs 上游实际 1 次、`inflight_hit`、带 confirm 才变成 2。范围文档写给面试的那句，没有数字就只是设计；现有 inflight 测试只有 map lookup。

### 熔断后不再锤上游

`--fail-after` 打挂上游。记 OPEN 之后上游 call/list 是否停、agent 是否立刻失败（不应再等上游超时）、list 里工具是否还在。和「失败探活误关熔断」绑在一起：数字对了，那条 bug 也验了。

### 观测开关差

OTLP+审计全开 vs 关掉（或 endpoint 不可达）。记 RPS / p99 掉多少、`otlp_dropped`。规格写「先离开热路径再谈延迟」；Jaeger 挂了不能拖死网关，用这一条证明。

### （余力）promote / `-32021` 不干等

慢调用（30s）：声明 tasks vs 不声明。看 HTTP 何时断开、上游是否仍是 1 条。证明不干等客户端超时；也可锁「promote 后不再叠打」。

### （余力）混合终态 HTTP 泄漏

同步成功 / 句柄 / `-32021` / Throttled / Deny 跑约 30s 后 `permit_held==0 && inflight_held==0`。内存 1e5 单测已有，补一条走真实 HTTP 更好讲。

### （余力）Grant 读路径不跟文件大小走

list 热路径叠很多 grant 行，p99 不该随文件变大。**loop 上 refresh 那条修完再测**，现在测只会量到持锁 parse。

## 清理与性能

### 删确认无用的重复和死代码

`demo.sh` 里 `mcp` / `names_of` / `wait_port` 定义两遍。`LocalGovernor` 的 `canceledWait_` / `rateLogged_` 未使用（令牌桶 M1 不做，`rate_per_sec` 保持打一次日志）。`Call.h` 只是转包 `Pipeline.h`。`httpStatusFor` 里 `Unavailable && httpStatus==503` 不可达。`localIpFor` 本产品不用，业务禁止调用即可，不必从 netlib 删。

### 热路径少 syscall、停机别忙等 OTLP

`Random.cpp` 每次 hex 都 `open("/dev/urandom")`，缓存 fd 或用 `getrandom` / 线程局部 PRNG。`paramsHashOf` 对整个 params `dump()`，大参数每次复制一份，能流式 hash 更好。`~OtlpHttpJsonTracer` 按条同步等 POST，停机可能拖很久，有界丢弃 + 短超时。单 loop + 阻塞 httplib 的 ~3.7k RPS 是架构上限，不作为本次必做；README 写实测值。

### 示例 listen 改 `127.0.0.1`

yaml 默认 `0.0.0.0` 加仓库明文 Bearer，内网演示也不该是这个默认；注释写明 token 仅演示。
