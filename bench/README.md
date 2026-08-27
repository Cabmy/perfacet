# 压测

附加延迟看 **in-process** 的 `gatewayMs − upstreamMs`（instant backend，不经 HTTP）。
HTTP 数字是本机 mock + Streamable HTTP 入口。不要对 gRPC 比倍数，也不要把 in-process 的 0 ms 写成「网关延迟」。

```bash
cmake --build build -j
bash bench/run.sh
```

`bench/run.sh` 会打：in-process 附加延迟、经 Perfacet 的 echo、**直连 mock** 对照（同并发）。Governor 4 抢 3 / 在途去重 / 熔断停锤 / OTLP 开关差用 `demo.sh` 与单测锁语义，数字以本机 `run.sh` 为准。

## 本机记录（2026-08-27，WSL2）

| 项 | 数字 |
|---|---|
| in-process 附加延迟 p50 / p99 / max | 0 / 0 / 1 ms（n=50000）；**未走 HTTP** |
| in-process 吞吐 | ~3.1×10⁵ RPS（同上，不经 HTTP） |
| HTTP 经网关 `echo`（concurrency=8，n=2000） | 以 `bench/run.sh` 当次输出为准 |
| HTTP 直连 mock 同工具（concurrency=8） | 对照「切面+治理税」；不要写「5–10k 已达成」 |

单 IO loop + worker 上阻塞 httplib 是架构上限。规格里的 5–10k 是打点，不是本机实测。不要再刷 64/128 并发 echo。
