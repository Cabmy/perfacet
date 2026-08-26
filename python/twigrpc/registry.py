"""twigrpc registry: RegistryWatcher——asyncio 后台 task 每 30s Discover，
暴露 instances() 快照。断连自动重建 registry 连接（自愈）。"""
import asyncio
import sys

from .stubs import rpc_pb2

from .conn import RpcConnection

REFRESH_SEC = 30.0


class RegistryWatcher:
    def __init__(self, registry_addr: str, service: str):
        self._addr = registry_addr  # 地址串 "tcp://host:port"
        self._service = service
        self._conn: RpcConnection | None = None
        self._instances: list[rpc_pb2.Instance] = []
        self._task: asyncio.Task | None = None
        self._version = 0

    async def start(self):
        self._conn = RpcConnection(self._addr)
        await self._conn.connect()
        await self.refresh()
        self._task = asyncio.get_running_loop().create_task(self._loop())

    async def _loop(self):
        while True:
            await asyncio.sleep(REFRESH_SEC)
            try:
                await self.refresh()
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 - 后台任务不许死，但要留痕
                print(f"[twigrpc.registry] refresh failed: {e!r}", file=sys.stderr)

    async def _reconnect(self):
        """断连自愈：重建 registry 连接（失败则置空，下轮再试）。"""
        if self._conn:
            try:
                await self._conn.close()
            except Exception:  # noqa: BLE001
                pass
            self._conn = None
        self._conn = RpcConnection(self._addr)
        await self._conn.connect()

    async def refresh(self):
        if self._conn is None or not self._conn.connected:
            await self._reconnect()  # 断连/未连：重建后继续
        req = rpc_pb2.DiscoverRequest(service=self._service)
        frame = await self._conn.call(
            "twigrpc.Registry.Discover", req.SerializeToString(), timeout=1.0
        )
        resp = rpc_pb2.DiscoverResponse()
        resp.ParseFromString(frame["body"])
        self._instances = [i for i in resp.instances if i.healthy]
        self._version = resp.version

    def invalidate(self):
        """失败路径：插队立即刷新。"""
        if self._task:
            self._task.cancel()
            self._task = asyncio.get_running_loop().create_task(self._refresh_then_loop())

    async def _refresh_then_loop(self):
        try:
            await self.refresh()
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001
            print(f"[twigrpc.registry] refresh failed: {e!r}", file=sys.stderr)
        await self._loop()

    def instances(self) -> list[rpc_pb2.Instance]:
        return list(self._instances)

    @property
    def version(self) -> int:
        return self._version

    async def stop(self):
        if self._task:
            self._task.cancel()
        if self._conn:
            await self._conn.close()
