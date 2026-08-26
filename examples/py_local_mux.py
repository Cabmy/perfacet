"""py_local_mux：同一连接 asyncio.gather 多路 call。

用法：python3 examples/py_local_mux.py <port> [--unix /path/to.sock] [N]
"""
import asyncio
import sys
import time

# twigrpc 包路径通过 PYTHONPATH 环境变量注入（运行前 export PYTHONPATH=python）

import math_pb2
from twigrpc.conn import RpcConnection


async def main() -> int:
    args = [a for a in sys.argv[1:]]
    if not args:
        print(f"usage: {sys.argv[0]} <port> [--unix /path/to.sock] [N]",
              file=sys.stderr)
        return 1
    if args[0] == "--unix":
        if len(args) < 2:
            print(f"usage: {sys.argv[0]} --unix <path> [N]", file=sys.stderr)
            return 1
        addr = f"unix://{args[1]}"
        rest = args[2:]
    else:
        addr = f"tcp://127.0.0.1:{args[0]}"
        rest = args[1:]
    n = int(rest[0]) if rest else 32

    conn = RpcConnection(addr)
    await conn.connect()

    async def one(i: int) -> bool:
        req = math_pb2.AddRequest(a=i, b=1000)
        frame = await conn.call("mux.Sleep", req.SerializeToString(), timeout=2.0)
        if frame["status"] != 0:
            return False
        resp = math_pb2.AddResponse()
        return resp.ParseFromString(frame["body"]) and resp.result == i + 1000

    t0 = time.perf_counter()
    results = await asyncio.gather(*[one(i) for i in range(n)])
    batch_ms = (time.perf_counter() - t0) * 1000
    ok = sum(results)
    await conn.close()
    print(f"mux: {ok}/{n} ok batch={batch_ms:.1f}ms ({addr})")
    return 0 if ok == n else 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
