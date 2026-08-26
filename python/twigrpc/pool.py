"""twigrpc pool: K 连接 round-robin + 一致性哈希（bisect 环）。

地址为地址串（"tcp://host:port" / "unix:///path"）。
"""
import asyncio
import bisect
import hashlib
from typing import Optional

from .conn import RpcConnection


def _hash32(s: str) -> int:
    return int.from_bytes(hashlib.md5(s.encode()).digest()[:4], "big")


class ConsistentHashRing:
    """一致性哈希环：每物理节点 vnodes 个虚拟节点。"""

    def __init__(self, nodes: list[str], vnodes: int = 160):
        self._ring: list[int] = []
        self._node_at: dict[int, str] = {}
        for node in nodes:
            for v in range(vnodes):
                h = _hash32(f"{node}#{v}")
                self._ring.append(h)
                self._node_at[h] = node
        self._ring.sort()

    def pick(self, key: str) -> Optional[str]:
        if not self._ring:
            return None
        h = _hash32(key)
        i = bisect.bisect_right(self._ring, h) % len(self._ring)
        return self._node_at[self._ring[i]]


class ConnPool:
    """K 连接 round-robin / 一致性哈希取用。"""

    def __init__(self, addrs: list[str], hash_keys: bool = False):
        self._addrs = list(addrs)
        self._conns: dict[str, RpcConnection] = {}
        self._rr = 0
        self._hash_keys = hash_keys
        self._ring: Optional[ConsistentHashRing] = None
        if hash_keys:
            self._ring = ConsistentHashRing(self._addrs)

    async def start(self):
        for addr in self._addrs:
            conn = RpcConnection(addr)
            try:
                await conn.connect()
                self._conns[addr] = conn
            except (ConnectionError, OSError, asyncio.TimeoutError):
                # 实例暂不可达：跳过，pick 只从存活连接中选（上层失败路径会摘除）
                continue

    def pick(self, key: str = "") -> RpcConnection:
        # 只从存活连接中选：死连接（kill/断连后 connected=False）必须跳过，
        # 否则 round-robin 会周期性命中坏实例，failover 退化为反复失败。
        live = [a for a, c in self._conns.items() if c.connected]
        if not live:
            raise ConnectionError("no live connection")
        if self._hash_keys and key and self._ring:
            node = self._ring.pick(key)
            conn = self._conns.get(node)
            if conn and conn.connected:
                return conn  # 哈希命中且存活
            # 哈希命中的节点未建成/已死：退化为 round-robin（保可用性）
        addr = live[self._rr % len(live)]
        self._rr += 1
        return self._conns[addr]

    def remove(self, addr: str):
        conn = self._conns.pop(addr, None)
        if conn:
            asyncio.get_running_loop().create_task(conn.close())
        if self._hash_keys:
            self._ring = ConsistentHashRing(list(self._conns))

    def live_addrs(self) -> list[str]:
        return [a for a, c in self._conns.items() if c.connected]

    async def close(self):
        for conn in self._conns.values():
            await conn.close()
        self._conns.clear()
