# 压测

附加延迟看 **in-process** 的 `gatewayMs − upstreamMs`（instant backend，不经 HTTP）。
单进程吞吐的 HTTP 数字是本机 mock + Streamable HTTP 入口，含客户端与上游。

```bash
cmake --build build -j
bash bench/run.sh
```

## 本机记录（2026-08-27，WSL2）

| 项 | 数字 |
|---|---|
| in-process 附加延迟 p50 / p99 / max | 0 / 0 / 1 ms（n=50000） |
| in-process 吞吐 | ~3.1×10⁵ RPS |
| HTTP `tools/call` echo（concurrency=32） | ~3740 RPS；客户端 RTT p50≈8.2 ms，p99≈12.3 ms |

HTTP 吞吐受单 IO loop、worker 上阻塞 httplib、以及 Python mock 限制；加并发到 64/128 不再升高。目标 5–10k 是规格里的打点，不是本机 HTTP 实测上限。
