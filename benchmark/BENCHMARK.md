# TwigRPC 基准测试报告（2026-08 重测版）

> 环境：WSL2 Ubuntu 24.04（16 核，kernel 6.18），g++ 13.3 **-O2 Release**，同机回环 127.0.0.1。
> 压测工具：`benchmark/bench`（自研，基于 ConnPool × RpcConn 真实多路复用；本轮升级为多指标输出：QPS / P50 / P90 / P99 / P99.9 / avg / min / max / MB/s / RSS）。
> 服务端：`math_server` / `kv_server`，**4 个 sub-reactor + 8 个 worker**。
> 方法论：每场景预热 1 次 + 正式 ≥2~3 次取中位数；所有多路复用延迟均为**端到端口径**（t0 取自发送时刻，旧版报告的 0.2µs 是修复前的错误口径）；各吞吐-延迟点用 Little 定律（L=λW）交叉验证自洽。
> 正确性门禁：全量单测 18/18、TSan/ASan/UBSan 全绿、工具链一致性配置期校验，详见 §十一。
> 复现：见文末「复现命令」。

## 〇、本轮重测修复的两个缺陷

重测不是简单复跑——按标准场景铺开后暴露了两处真实问题，修复后才得到有效数据：

1. **kv_server sleep 粒度失真（模拟口径错误）**：协作取消改造后轮询粒度为 5ms，`--sleep-ms 1` 实际睡 5ms（旧报告 6.7K QPS 测于改造前，本轮初测跌到 1.5K 且大量超时）。已改为 1ms 粒度，恢复 6.7K。
2. **★ TcpConnection ET 读循环缺失（netlib 核心缺陷）**：ET 模式下 `handleRead` 只读一次（单次上限 ~66KB），内核残留数据不再触发边沿。小帧场景（旧报告全部场景）完全掩盖；≥8KB 报文吞吐崩塌（8KB 仅 334 QPS，≥256KB 全部超时）。修复为循环读到 EAGAIN（写路径同步修复）后：8KB 334→42.0K QPS（**126 倍**），详见 `docs/问题记录.md` §7.1。

---

## 一、echo 纯网络基线（math.Add，16 连接 × 128 在途 × 10s）

| 运行 | QPS | P50 | P90 | P99 | P99.9 | 失败 | RSS |
|------|-----|-----|-----|-----|-------|------|-----|
| #1 | 69,209 | 30.3 ms | 33.6 ms | 37.5 ms | 42.9 ms | 0 | 10.9 MB |
| #2 | 70,318 | 29.4 ms | 33.6 ms | 48.4 ms | 59.8 ms | 0 | 10.9 MB |
| #3 | 99,555 | 19.2 ms | 21.1 ms | 28.3 ms | 32.8 ms | 0 | 10.9 MB |

**中位数 ≈ 70.3K QPS**（#3 为宿主低负载窗口的离群值；WSL2 宿主侧负载漂移导致同配置 ±40% 波动，#1/#2 为典型值）。自洽校验：2048 在途 ÷ 70.3K QPS = 29.1ms = 实测 P50 ✓。

> 口径说明：多路复用模式下 P50 是「2048 个在途请求的端到端排队+处理延迟」（Little 定律下由并发度决定），单请求真实网络往返延迟由串行模式（§三）的 P50=109µs 代表。

## 二、kv 1ms-sleep 业务 IO（kv.Set，16 连接 × 128 在途 × 10s）

| 运行 | QPS | P50 | P99 | P99.9 | 失败 | RSS |
|------|-----|-----|-----|-------|------|-----|
| #1 | 6,732 | 293.6 ms | 304.7 ms | 307.8 ms | 0 | 10.2 MB |
| #2 | 6,708 | 294.6 ms | 307.4 ms | 314.6 ms | 0 | 10.2 MB |
| #3 | 6,708 | 294.6 ms | 304.7 ms | 305.5 ms | 0 | 10.2 MB |

**≈6.7K QPS**，达理论上限（8 worker × ~1000 req/s）的 **84%**——handler 睡 1ms 时吞吐由 worker 池决定，2048 在途把 worker 打满（扩展曲线见 §四）。

## 三、串行端到端延迟对照（1 连接 × 1 在途）

| 场景 | QPS | P50 | P90 | P99 | P99.9 | max | RSS |
|------|-----|-----|-----|-----|-------|-----|-----|
| echo（math.Add）#1 | 6,711 | 115 µs | 252 µs | 399 µs | 527 µs | 1.38 ms | 6.4 MB |
| echo（math.Add）#2 | 7,055 | 109 µs | 244 µs | 392 µs | 519 µs | 1.71 ms | 6.4 MB |
| kv 1ms IO #1 | 684 | 1,444 µs | 1,626 µs | 1,870 µs | 2,129 µs | 2.29 ms | 6.4 MB |
| kv 1ms IO #2 | 688 | 1,438 µs | 1,618 µs | 1,824 µs | 2,019 µs | 2.15 ms | 6.4 MB |

纯 echo 端到端 P50=**109µs**（编解码 + Reactor 线程模型 + 回环往返的框架路径开销）；kv P50=1.44ms = 1ms handler sleep + ~0.44ms 框架开销。

## 四、多路复用收益曲线（kv.Set，handler 睡 1ms）

| 配置（连接 × 在途） | 真实在途 | QPS | P50 | 相对串行 |
|------|---------|------|------|---------|
| 1 × 1（串行同步） | 1 | **686** | 1.44 ms | 1.0× |
| 2 × 2 | 4 | 2,770 | 1.41 ms | 4.0× |
| 4 × 4 | 16 | 6,733 | 2.30 ms | 9.8× |
| 8 × 8 | 64 | 6,712 | 9.37 ms | 9.8× |
| 16 × 16 | 256 | 6,724 | 36.9 ms | 9.8× |
| 16 × 64 | 1,024 | 6,731 | 146.8 ms | 9.8× |
| 16 × 128 | 2,048 | **6,700** | 294.7 ms | 9.8× |
| 32 × 128 | 4,096 | 6,715 | 598.5 ms | 9.8× |

**16 个在途即打满 8 worker**（上限 8K）；此后吞吐恒定在 worker 上限，延迟按 Little 定律精确线性（如 2048÷6700=305ms ≈ 实测 294.7ms）——队列与 PendingTable 管理无异常放大、无队头阻塞放大。

**背压边界**（64 连接 × 512 在途 = 32K 在途，超每连接 256 上限）：吞吐守住 6.3K，超限请求被 BUSY/超时快速拒绝（232K fail），不拖垮存量请求——在途上限背压按设计生效。

### 为什么需要多路复用（定量解释）

- **串行**：1 个在途，客户端 99.3% 时间空等，服务端 worker 利用率仅 686/8000 = 8.6%。要 10K QPS 就得开 10K 个线程（thread-per-connection 模型直接爆炸）。
- **多路复用**：1 条 TCP 连接承载 128 在途（requestId 关联 PendingTable），IO 线程数 O(连接数)，在途并发 O(连接数 × 在途数)——epoll + 多路复用相对一连接一线程的数量级优势。
- **代价与边界**：要求协议支持乱序响应（requestId 路由）、在途上限背压（每连接 256，超限回 BUSY）、幂等重试语义（迟到帧丢弃）。

## 五、连接数扩展曲线（echo，验证 epoll 承载力）

| 配置（连接 × 在途/连接） | QPS | P50 | P99 | 客户端 RSS |
|------|------|-----|-----|-----------|
| 1 × 128 | 52.4K | 2.19 ms | 3.86 ms | 10.4 MB |
| 16 × 8 | 55.1K | 2.10 ms | 3.52 ms | 9.9 MB |
| 16 × 128 | 70.3K | 29.5 ms | 40.3 ms | 10.9 MB |
| 100 × 20 | 71.7K | 25.9 ms | 48.5 ms | 28.6 MB |
| 1000 × 2 | 41.5K | 44.1 ms | 87.1 ms | 110.5 MB |

- **单连接多路复用即可达 52.4K QPS**（P50 仅 2.2ms）——多路复用消除连接数与吞吐的耦合。
- 16→100 连接无吞吐增益：4 个 IO 线程已饱和，瓶颈从连接数转移到 CPU。
- 1000 连接回落至 41.5K：压测端 1000 线程调度 + 逐连接缓冲内存（RSS 110MB）+ accept 风暴的开销。epoll 本身承载 1K 连接无事件丢失（0 失败）。

## 六、大报文场景（math.Echo 回显，载荷 1KB~1MB）★ 新增

> 本轮重测定位并修复 ET 读循环缺陷（见 §〇）后的数据；修复前 8KB 仅 334 QPS、≥256KB 全部超时失败。

| 载荷 | 配置 | QPS | 双向吞吐 | P50 | P99 | 失败 | RSS |
|------|------|-----|---------|-----|-----|------|-----|
| 1 KB | 16×16 | 56.7K | 116 MB/s | 4.3 ms | 6.6 ms | 0 | 11.2 MB |
| 8 KB | 16×16 | 42.0K | **688 MB/s** | 5.4 ms | 11.4 ms | 0 | 16.0 MB |
| 64 KB | 16×16 | 14.0K | **1,829 MB/s** | 15.2 ms | 28.4 ms | 0 | 23.9 MB |
| 256 KB | 8×16 | 3.1K | 1,615 MB/s | 31.5 ms | 55.5 ms | 0 | 67.8 MB |
| 1 MB | 8×8 | 338 | 709 MB/s | 124.3 ms | 215.6 ms | 0 | 97.6 MB |

- 峰值带宽 1.8 GB/s 出现在 64KB 帧长（writev 聚合与内核拷贝的平衡点）。
- 1MB 帧回落至 709 MB/s：单帧多次 readv + Buffer 扩容 memmove 的成本，且 16MB 协议上限内未做零拷贝。属已知优化空间，非缺陷（0 失败）。

## 七、长稳测试（echo，16 × 128 × 60s）

| 指标 | 结果 |
|------|------|
| QPS（全程） | 70,750（无衰减） |
| 总请求 / 失败 | 4,249,856 / **0** |
| 服务端 RSS | 18.0 MB，t=0/20/40/60s **恒定不变** |
| 服务端 fd | 22 → 38（16 连接）→ 断开后回到 22，**无泄漏** |
| 服务端线程数 | 13，恒定 |
| P50 / P99 / P99.9 | 29.5 / 40.3 / 52.7 ms |

## 八、gRPC 对照基准（同机同负载模型）

**方法**：gRPC async C++（官方示例改造），echo 同语义（`echo.Echo`），server 8 个 CQ 线程（对齐 TwigRPC 4 IO + 8 worker 线程量级），client 16 通道 × 128 在途，每通道独立 CQ。gRPC 1.71 + protobuf 29.3（anaconda 工具链，独立进程）。

| 框架 | 多路复用（16×128）QPS | 串行（1×1）QPS | 串行 P50 | 串行 P99 | RSS |
|------|------|------|------|------|------|
| TwigRPC（math.Add） | **70.3K** | **7.1K** | **109 µs** | **392 µs** | **10.9 MB** |
| gRPC async C++（echo.Echo） | 22.8K | 3.5K | 242 µs | 576 µs | 137 MB |
| 差距 | **3.1×** | **2.0×** | **2.2×** | 1.5× | **12.6×** |

### 差异分析

1. **吞吐（3.1×）**：TwigRPC 每请求固定 24B 定长头 + 1 次 protobuf 编解码；gRPC 走 HTTP/2，每请求涉及 HPACK 头压缩、流状态机、多帧处理。
2. **延迟（2.2×）**：纯 echo 下 109µs vs 242µs，差距来自 HTTP/2 帧封装 + HPACK 编码 + 更重的完成回调路径。
3. **内存（12.6×）**：gRPC 客户端 RSS 137MB（HTTP/2 流表、HPACK 动态表、每流上下文）；TwigRPC 10.9MB（每连接一个 Buffer + pending 表）。
4. **结论边界**：此差距是「简单 echo 负载 + 同步响应」场景的特例。gRPC 的流式传输、拦截器、多语言生态在复杂场景价值更大；TwigRPC 在**高吞吐短请求**场景有协议级优势（定长头免解析、无 HTTP 层开销）。

---

## 九、复现命令

```bash
# 0) Release 构建
#   多 protoc 环境（conda/系统双装）下若报「protoc 与 libprotobuf 版本不一致」，
#   按提示显式指定同源编译器：-DProtobuf_PROTOC_EXECUTABLE=/usr/bin/protoc
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -S . && cmake --build build-rel -j$(nproc)

# 0b) 质量门禁：先跑测试再压测（见 §十一）
cd build-rel && ctest && cd ..   # task_tree_test / goaway_test / agent_chain_test

# 1) echo 纯网络基线（§一）
./build-rel/examples/math_server 19200 --io-threads 4 --workers 8 &
./build-rel/benchmark/bench 127.0.0.1 19200 16 128 10 math.Add

# 2) kv 1ms-sleep 业务 IO（§二）
./build-rel/examples/kv_server 19201 --sleep-ms 1 --io-threads 4 --workers 8 &
./build-rel/benchmark/bench 127.0.0.1 19201 16 128 10 kv.Set

# 3) 串行对照（§三）
./build-rel/benchmark/bench 127.0.0.1 19200 1 1 10 math.Add 0 1   # echo：P50≈109µs
./build-rel/benchmark/bench 127.0.0.1 19201 1 1 10 kv.Set 0 1     # kv：P50≈1.44ms

# 4) 多路复用收益曲线（§四）
./build-rel/benchmark/bench 127.0.0.1 19201 4 4 10 kv.Set     # 16 在途即打满 worker
./build-rel/benchmark/bench 127.0.0.1 19201 64 512 10 kv.Set  # 背压边界（BUSY）

# 5) 连接数扩展（§五）
./build-rel/benchmark/bench 127.0.0.1 19200 1 128 10 math.Add    # 单连接多路复用
./build-rel/benchmark/bench 127.0.0.1 19200 1000 2 10 math.Add   # 1K 连接

# 6) 大报文（§六，末位参数 = 载荷字节数）
./build-rel/benchmark/bench 127.0.0.1 19200 16 16 10 math.Echo 0 0 8192    # 8KB
./build-rel/benchmark/bench 127.0.0.1 19200 16 16 10 math.Echo 0 0 65536   # 64KB

# 7) 长稳（§七）：60s，另开终端监控服务端 /proc/<pid>/status 与 fd 数
./build-rel/benchmark/bench 127.0.0.1 19200 16 128 60 math.Add

# 8) gRPC 对照（§八，anaconda 工具链，独立进程）
cd benchmark/grpc_echo
./gen.sh && cmake -B build -DGRPC_ROOT=/home/cabmy/anaconda3 && cmake --build build -j$(nproc)
LD_LIBRARY_PATH=/home/cabmy/anaconda3/lib ./build/echo_server 127.0.0.1:50051 8 &
LD_LIBRARY_PATH=/home/cabmy/anaconda3/lib ./build/echo_client 127.0.0.1 50051 16 128 10
LD_LIBRARY_PATH=/home/cabmy/anaconda3/lib ./build/echo_client 127.0.0.1 50051 1 1 5   # 串行
```

## 十、历史基线（回归对比）

| 版本 | 场景 | QPS | 备注 |
|------|------|-----|------|
| T31（debug 编译） | echo 16×128×10s | 13.3K | 写聚合启用后的 P2 基线 |
| T47（-O2） | echo 16×128×10s | 85.7K | 宿主负载较低时的窗口；本轮同配置典型值 70K（±40% 波动属 WSL2 环境噪声） |
| T47（-O2） | kv 1ms 16×128 | 6.7K | 与本轮一致（worker 上限决定） |
| 本轮 | 大报文 8KB 16×16 | 334 → **42.0K** | ET 读循环缺陷修复（§〇.2） |

## 十一、质量门禁（单测结果，数据有效性前提）

压测数字只在「代码正确」的前提下有意义。本轮数据采集前后的正确性门禁：

| 层 | 覆盖 | 结果 |
|------|------|------|
| 内建单测（主构建 `ctest`） | `task_tree_test`：任务树全生命周期（spawn/deadline 收紧/拒孤儿/cancel/complete 摘除/入站节点所有权）；`goaway_test`：排空停机全链路（在途不死等、停机后拒新快败、无死锁）；`agent_chain_test`：agent 跨进程全链路（入站 adopt → 下游子调用 → 跨进程 CANCEL 传播 → 双侧树排空） | **3/3 通过**（agent_chain_test 20/20 轮稳定） |
| Catch2 套件（出库，需手动挂回构建） | 12 个 C++ 测试：netlib/codec/e2e 异常路径/pending/rpcconn/connpool/registry 守护进程/balancer/watcher/stats/agent | **12/12 通过** |
| Python 客户端 | pytest：conn/pool/failover（协议字节与 C++ 黄金向量双端一致） | **3/3 通过** |
| 端到端冒烟 | `agent_demo` 4 画像 ×（取消/全程）：叶子状态断言 + 准入槽归还 + 任务树清空 | **全 PASS** |
| 动态分析 | TSan（ConnPool 轮询游标竞态 → atomic；跨线程钩子槽位 → AtomicHook）/ ASan + UBSan，覆盖三个内建单测与 agent_demo 全画像 | **全绿，无告警** |

> 挂回全量套件：临时在根 `CMakeLists.txt` 追加 `add_subdirectory(test)` 后重新 configure，`ctest` 共 18 项（12 Catch2 + 3 pytest + 3 内建），跑完还原。

**工具链一致性（本轮验证的隐患与加固）**：本机存在双 protoc（系统 3.21 / anaconda 29.3），曾导致全新 build 目录生成与 libprotobuf 不匹配的 pb 代码（编译期报 `runtime_version.h` 缺失）。核查结论：**所有历史构建目录（含本报告的 build-rel）的 pb 均由 protoc 3.21 生成、与链接的 libprotobuf 3.21 同源**，本报告数据不受影响。已加固：proto/CMakeLists.txt 在配置期校验 protoc 与 libprotobuf 版本一致，不一致直接报错并给出修法，生成器改用 CMake 解析的绝对路径，不再受调用环境 PATH 影响。

**gRPC 对照组的工具链**：独立进程 + 独立工具链（gRPC 1.71 + protobuf 29.3，见 §八），其生成物与运行时内部自洽，不影响对照公平性。
