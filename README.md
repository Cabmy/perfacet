# Perfacet

C++17 高性能 RPC 框架：epoll 主从 Reactor 网络库（netlib）+ 自研二进制协议（24B 定长头 + TLV，echo 吞吐 3.1× gRPC）+ 注册中心/负载均衡/心跳 + GOAWAY 排空停机 + Prometheus/Grafana 观测栈 + Python asyncio 客户端，并内建 **agent 运行时**（任务树全生命周期、准入/重试双扩展点、级联取消）——fan-out、随时打断、整树预算这类 LLM 工具调用负载获得协议级原生支持。

## 架构

```
┌────────────────────────────────────────────────────────────────────┐
│ 应用层 examples/                                                     │
│   C++: server.bind("math.Add", handler); server.serve()            │
│   Py:  client = AsyncClient(registry, "math"); await client.call() │
├────────────────────────────────────────────────────────────────────┤
│ agent 层（Agent 运行时，独立库）                                    │
│   TaskTree(deadline 收紧 / DFS 级联取消 / complete 摘除)            │
│   Runtime(调用粘合: 叶子出口自动结束, 入站 doneHook 对称收尾)       │
│   IPolicy 准入(Admission 槽位) / IRetry 重试 —— 插件注入，框架零 if │
├────────────────────────────────────────────────────────────────────┤
│ rpc 层（taskId/deadline/CANCEL/GOAWAY 原语透传，不感知 agent）        │
│   codec(24B 头+TLV)  dispatcher(方法表)  server(RpcServer)          │
│   client(RpcConn×ConnPool×PendingTable 多路复用)  balancer(RR/CH)   │
│   registry(自举)  heartbeat(Provider)  取消(CANCEL/CancelToken)     │
│   停机(GOAWAY 排空→拒新→强收)  stats(线程分片无锁采集)              │
├────────────────────────────────────────────────────────────────────┤
│ 可观测: AdminEndpoint(/metrics /healthz /stats) → Prometheus → Grafana │
├────────────────────────────────────────────────────────────────────┤
│ netlib 层（通用网络库，零业务依赖）                                   │
│   EventLoop(eventfd+timerfd)  Epoll  TcpServer/TcpClient  Socket    │
│   Acceptor(TCP/UDS Endpoint)  Buffer(writev 聚合)  ThreadPool      │
└────────────────────────────────────────────────────────────────────┘
```

**协议**（24B 定长头，大端）：`magic(2B) version flags msgType status reserved(2B) requestId(8B) metaLen(4B) bodyLen(4B)` + TLV metadata + protobuf body。TLV 携带方法名、**deadline（绝对 Unix 毫秒）**、**taskId**；`CANCEL` 帧取消在途请求，`GOAWAY` 帧通知对端排空停机。requestId 进程内全局唯一，单连接可承载海量在途复用；JSON 调试模式可 curl 手工组帧。

## 核心特性

- **单连接多路复用**：requestId 关联在途请求，吞吐随（连接数 × 在途数）线性扩展至 worker 饱和；每连接 256 在途上限 + BUSY 快速失败，给 fan-out 提供背压
- **agent 运行时**：TaskTree 任务树（spawn 收紧 deadline、cancel DFS 收集在途并逐个发 CANCEL、complete 无子无在途才摘除）；跨进程树靠 CANCEL 叶子传播，无需额外协议字段；准入（`IPolicy`）与重试（`IRetry`）分离，四个示例画像（research/code/llm/summary）全部由插件组合表达
- **任务全生命周期闭环**：谁创建、谁结束——叶子由 Runtime 在调用出口（成功/失败/异常同一出口）自动结束，根与中间节点由创建方显式 complete，入站任务靠 doneHook 对称收尾（只结束本进程 adopt 的节点，自连拓扑下不越权摘别人的叶子）；spawn 拒绝孤儿节点，agent_demo 验收断言任务树清空
- **全链路协作取消**：CANCEL 帧 + 服务端 CancelToken 轮询（不杀线程）；deadline 沿任务树只收紧不放宽；客户端超时自动补发 CANCEL，服务端不空跑
- **排空停机**：stop() 先拒新请求（BUSY）再向所有连接广播 GOAWAY，等在途 handler 全部回包（上限 3s）才强制收尾；客户端认 GOAWAY 后不再发新调用、在途照常收响应——重启/发布不断存量流量
- **单次调用语义**：rpc 层失败即抛、零内置重试；重试收敛为 agent 层 `IRetry` 扩展点，杜绝「客户端库与业务各重试一次」的故障放大
- **传输**：TCP 与 UDS（`unix:///path`）统一 Endpoint，同机通信免协议栈开销
- **分布式**：注册中心由 RpcServer 自举、RoundRobin + 一致性哈希（160 vnode，1/N 迁移）、心跳摘除、kill 实例零失败漂移
- **可观测**：分片无锁采集（`Collector` 独占数据面）；`/metrics` 导出 QPS/状态名、延迟直方图、`in_flight`、`ready`；`/healthz` 与 `ready` 同源（未 serve / 排空中 503）；`/stats` 人类摘要；慢调用超 P99 打日志（BUSY 不进直方图）；Grafana：QPS / 直方图 P99 / in-flight / ready 实例
- **多语言**：Python asyncio 客户端（conn/pool/registry/facade + 手写 stub），协议与 C++ 黄金字节向量双端一致；TSan/ASan 全绿

## 快速开始

### 1. 构建

```bash
# 依赖：protobuf 3.21（Ubuntu 24.04: apt install libprotobuf-dev protobuf-compiler）
mkdir -p build && cd build
cmake .. && make -j$(nproc)      # Debug 带 -g；压测用 -DCMAKE_BUILD_TYPE=Release
```

### 2. 最小闭环（math）

```bash
./examples/math_server 9000 &                 # 起服务端
./examples/math_client 127.0.0.1 9000         # sync: 3 / async: 200/200 ok
```

### 3. Agent 取消传播演示

```bash
./examples/agent_demo                          # research 画像：两跳 fan-out 中途 cancel(root)，所有叶子 CANCELLED，树清空
./examples/agent_demo --profile llm            # Admission(max=2)：部分请求 REJECTED 快速失败
./examples/agent_demo --no-cancel              # 跑完全程：12 叶子全 OK、准入槽全部归还、树清空
```

### 4. 内建单测

测试套件是内部设施，产品构建默认不感知 test/ 目录：

```bash
# 依赖 Catch2（Ubuntu 24.04: apt install catch2）；Python 用例另需 pytest
cmake -B build -DTWIGRPC_BUILD_TESTS=ON
cmake --build build -j$(nproc)
cd build && ctest
```

- Catch2 套件：netlib/codec/registry/e2e/pending/rpcconn/connpool/registry_daemon/balancer/watcher/stats/agent + Python conn/pool/failover
- 三个独立 main + 断言的端到端测试（零框架依赖）：task_tree_test（任务树全生命周期）、goaway_test（排空停机全链路）、agent_chain_test（agent 跨进程全链路，自 fork 服务进程）

`agent_chain_test` 也可拆两台机器跑，验证跨机取消传播：

```bash
# 机器 A
./build/test/agent_chain_test --server --bind 0.0.0.0 --port 9000
# 机器 B
./build/test/agent_chain_test --host <A 的 IP> --port 9000
```

### 5. 观测栈（docker compose 一键）

```bash
cd deploy && docker compose up -d
# compose 自动构建镜像，起 3 个 math_server + registry + prometheus + grafana
# Grafana http://localhost:3000 (admin/admin)   Prometheus http://localhost:9090
# 3 个 server 实例各自以 hostname-pid 为实例名注册 + 心跳，指标随调用增长
curl -s localhost:9101/metrics | grep twigrpc_server_requests_total
```

### 6. Python 客户端

```bash
python3 examples/py_math_client.py   # 调 C++ math server 输出 3
```

### 7. 配置

所有进程的地址/线程参数支持统一配置：优先级 **命令行参数 > 环境变量 > .env > 默认值**。

```bash
cp .env.example .env    # 按需修改后无需传任何参数即可启动
./build/registryd/registryd &
./build/examples/math_server &      # 端口/注册中心地址全部从 .env 读
```

跨机部署要点：`--registry` 支持 DNS 主机名；服务自注册的 advertise IP **默认自动推导**（本机→注册中心出口 IP），多网卡/NAT 时用 `TWIGRPC_ADVERTISE_IP` 或 `--advertise` 显式指定。全部键见 [.env.example](.env.example)。

## 压测数据摘要（Release -O2，同机回环，详见 [benchmark/BENCHMARK.md](benchmark/BENCHMARK.md)）

| 场景 | QPS | 端到端 P50 | RSS |
|------|-----|-----------|-----|
| echo 纯网络（16 连接 × 128 在途） | **70.3K** | 109 µs（串行） | 10.9 MB |
| kv 业务 IO（handler 睡 1ms，16×128） | **6.7K** | 1.44 ms | 10.2 MB |
| 串行对照（1×1） | 686 | — | 6.4 MB |
| 大报文回显 8KB / 64KB（16×16） | 42.0K / 14.0K | 688 / 1,829 MB/s | 16–24 MB |
| **gRPC async 对照**（同机同参数） | 22.8K | 242 µs | 137 MB |

**多路复用收益**：handler 睡 1ms 下，串行 686 QPS → 2×2 2,770 → 4×4 6,733（16 在途即打满 8 worker）→ 16×128 6,700（**9.8×**）。IO 线程数 O(连接数)，在途并发 O(连接数 × 在途数)。

**vs gRPC**：同 echo 语义下吞吐 3.1×、串行延迟 2.2×、RSS 12.6×——24B 定长头 + 无 HTTP/2 开销的协议级优势；gRPC 的流式/生态在复杂场景价值更大。

## 目录结构

```
proto/          契约（rpc/math/kv/registry，跨语言单一事实源）
netlib/         通用网络库（epoll 主从 Reactor，TCP/UDS Endpoint）
rpc/            RPC 核心（codec/dispatcher/server/client/balancer/registry/heartbeat/stats/取消/排空停机）
agent/          Agent 运行时（TaskTree/Runtime/IPolicy 准入 + IRetry 重试双扩展点）
test/           测试套件（内部设施：Catch2 套件 + 三个独立 main 端到端测试 + Python 用例）
registryd/      注册中心独立进程（基于 RpcServer 自举）
examples/       math/kv/local_mux/agent 示例
python/twigrpc/   Python 生产客户端（asyncio）
benchmark/      bench 压测工具 + grpc_echo 对照 + BENCHMARK.md
deploy/         docker-compose 观测栈（server×3 + registry + prometheus + grafana）
```
