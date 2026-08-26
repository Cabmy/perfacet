#!/usr/bin/env python3
"""HTTP 压测：连已启动的 Perfacet + mock。测客户端 RTT 与 RPS。"""
from __future__ import annotations

import argparse
import asyncio
import statistics
import time

import aiohttp

PROTOCOL = "2026-07-28"


async def one(session: aiohttp.ClientSession, url: str, token: str, i: int) -> float:
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/json",
        "MCP-Protocol-Version": PROTOCOL,
        "Mcp-Method": "tools/call",
        "Mcp-Name": "echo__echo",
        "Content-Type": "application/json",
    }
    body = {
        "jsonrpc": "2.0",
        "id": i,
        "method": "tools/call",
        "params": {
            "name": "echo__echo",
            "arguments": {"n": i},
            "_meta": {
                "io.modelcontextprotocol/protocolVersion": PROTOCOL,
                "io.modelcontextprotocol/clientCapabilities": {},
            },
        },
    }
    t0 = time.perf_counter()
    async with session.post(url, headers=headers, json=body) as resp:
        await resp.read()
        if resp.status != 200:
            raise RuntimeError(f"HTTP {resp.status}")
    return (time.perf_counter() - t0) * 1000.0


async def run(url: str, token: str, n: int, concurrency: int) -> None:
    lat: list[float] = []
    sem = asyncio.Semaphore(concurrency)
    timeout = aiohttp.ClientTimeout(total=30)

    async def bound(i: int) -> None:
        async with sem:
            ms = await one(session, url, token, i)
            lat.append(ms)

    t0 = time.perf_counter()
    async with aiohttp.ClientSession(timeout=timeout) as session:
        await asyncio.gather(*(bound(i) for i in range(n)))
    wall = time.perf_counter() - t0
    lat.sort()
    p50 = lat[len(lat) // 2]
    p99 = lat[max(0, int(len(lat) * 0.99) - 1)]
    print("perfacet_bench http")
    print(f"  n={n}  concurrency={concurrency}  wall_s={wall:.4f}  rps={n / wall:.0f}")
    print(f"  client_rtt_ms  p50={p50:.3f}  p99={p99:.3f}  mean={statistics.mean(lat):.3f}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8741/mcp")
    ap.add_argument("--token", default="pf_cursor")
    ap.add_argument("-n", type=int, default=8000)
    ap.add_argument("-c", type=int, default=32)
    args = ap.parse_args()
    asyncio.run(run(args.url, args.token, args.n, args.c))


if __name__ == "__main__":
    main()
