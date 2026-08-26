"""twigrpc client: 同步门面（内部事件循环线程）+ AsyncClient 原生异步接口。

单次调用语义：失败即抛（无内置重试）。失败路径只做摘除坏连接 + invalidate 快照，
让下一次调用换实例；重试策略由调用方（C++ agent 层 RetryPolicy 的对等位置）实现。
"""
import asyncio
import threading

from .pool import ConnPool
from .registry import RegistryWatcher


class AsyncClient:
    """原生异步接口：服务发现 + 连接池 + 单次调用。"""

    def __init__(self, registry_addr: str, service: str,
                 hash_keys: bool = False, conns_per_instance: int = 1):
        self._watcher = RegistryWatcher(registry_addr, service)
        self._pool: ConnPool | None = None
        self._hash_keys = hash_keys
        self._conns_per = conns_per_instance
        self._started = False

    async def start(self):
        await self._watcher.start()
        await self._rebuild_pool()
        self._started = True

    async def _rebuild_pool(self):
        instances = self._watcher.instances()
        if self._pool:
            await self._pool.close()
        addrs: list[str] = []
        for inst in instances:
            addrs.extend([f"tcp://{inst.ip}:{inst.port}"] * self._conns_per)
        if not addrs:
            return
        self._pool = ConnPool(addrs, hash_keys=self._hash_keys)
        await self._pool.start()

    async def call_raw(self, method: str, body: bytes, timeout: float = 0.5,
                       hash_key: str = "") -> dict:
        """单次调用：失败即抛。摘除坏连接 + invalidate，让下次调用换实例。"""
        if not self._pool or not self._pool.live_addrs():
            self._watcher.invalidate()
            await asyncio.sleep(0.05)
            await self._rebuild_pool()
        if not self._pool:
            raise ConnectionError("no healthy instance")
        conn = self._pool.pick(hash_key)
        try:
            return await conn.call(method, body, timeout=timeout)
        except Exception:
            # 立即摘除坏连接（避免 rebuild 前再次被 pick）；
            # watcher 快照可能仍含该实例（healthy 未过期），靠 invalidate + 下次重建收敛。
            self._pool.remove(conn.addr)
            self._watcher.invalidate()
            raise

    async def stop(self):
        if self._pool:
            await self._pool.close()
        await self._watcher.stop()


class Client:
    """同步门面：内部事件循环线程 + asyncio.run_coroutine_threadsafe。"""

    def __init__(self, registry_addr: str, service: str, hash_keys: bool = False):
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()
        self._async = AsyncClient(registry_addr, service, hash_keys=hash_keys)
        fut = asyncio.run_coroutine_threadsafe(self._async.start(), self._loop)
        fut.result(timeout=10)

    def _run_loop(self):
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    def call_raw(self, method: str, body: bytes, timeout: float = 0.5) -> dict:
        fut = asyncio.run_coroutine_threadsafe(
            self._async.call_raw(method, body, timeout), self._loop
        )
        return fut.result(timeout=timeout + 5)

    def close(self):
        asyncio.run_coroutine_threadsafe(self._async.stop(), self._loop).result(5)
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(timeout=5)
