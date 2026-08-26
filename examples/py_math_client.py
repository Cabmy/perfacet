"""py_math_client：Python 客户端调 C++ math server 输出 3。
用法：python3 examples/py_math_client.py [ip] [port]"""
import asyncio
import sys

# twigrpc 包路径通过 PYTHONPATH 环境变量注入（运行前 export PYTHONPATH=python）

from twigrpc.pool import ConnPool
from twigrpc.stubs.math_stub import MathStub


async def main() -> int:
    ip = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

    pool = ConnPool([f"tcp://{ip}:{port}"])
    await pool.start()

    async def raw_call(method, body, timeout=0.5):
        return await pool.pick().call(method, body, timeout)

    stub = MathStub(raw_call)
    result = await stub.add(1, 2)
    print(result)
    await pool.close()
    return result


if __name__ == "__main__":
    sys.exit(0 if asyncio.run(main()) == 3 else 1)
