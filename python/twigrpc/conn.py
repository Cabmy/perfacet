"""twigrpc conn: asyncio 长连接（tcp:// / unix:// 地址串）+ 拆包状态机 + pending 字典。

单次调用语义：超时先摘 pending、发 CANCEL 帧通知服务端停手，再抛 TimeoutError。
重试不属于本层（与 C++ 一致，只存在于 agent 层 RetryPolicy）。
"""
import asyncio
import time

from .codec import encode, decode, OK, ERROR, REQUEST, RESPONSE, PING, PONG, CANCEL, GOAWAY


class RpcConnection:
    """单条多路复用连接：requestId 关联 asyncio.Future。"""

    def __init__(self, addr: str):
        # "tcp://host:port" 或 "unix:///abs/path"
        self.addr = addr
        self._reader = None
        self._writer = None
        self._pending: dict[int, asyncio.Future] = {}
        self._next_id = 0
        self._recv_buf = b""
        self._reader_task: asyncio.Task | None = None
        self._closed = False
        self._goaway = False  # 对端排空停机：拒新调用，在途照常收响应

    async def connect(self, timeout: float = 2.0):
        if self.addr.startswith("unix://"):
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_unix_connection(self.addr[len("unix://"):]), timeout
            )
        else:
            host, port = self.addr[len("tcp://"):].rsplit(":", 1)
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(host, int(port)), timeout
            )
        self._reader_task = asyncio.get_running_loop().create_task(self._read_loop())
        self._closed = False

    @property
    def connected(self) -> bool:
        return (
            self._writer is not None
            and not self._writer.is_closing()
            and not self._closed
            and not self._goaway
        )

    async def _read_loop(self):
        """拆包状态机：读 socket → 解帧 → 匹配 pending。"""
        try:
            while True:
                chunk = await self._reader.read(65536)
                if not chunk:
                    break  # EOF
                self._recv_buf += chunk
                self._drain_frames()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            self._fail_all(ConnectionError("connection closed"))

    def _drain_frames(self):
        while True:
            result, frame = decode(self._recv_buf)
            if result != OK:
                if result == ERROR:
                    # 协议错位：不可恢复，断连
                    self._closed = True
                    if self._writer:
                        self._writer.close()
                    self._fail_all(ValueError("protocol error"))
                break
            self._recv_buf = self._recv_buf[frame["_consumed"]:]
            if frame["msgType"] == RESPONSE:
                fut = self._pending.pop(frame["requestId"], None)
                if fut is not None and not fut.done():
                    fut.set_result(frame)
                # else: 迟到响应（已超时摘除），丢弃
            elif frame["msgType"] == PONG:
                pass  # 心跳回包不入表
            elif frame["msgType"] == GOAWAY:
                # 对端排空停机：本连接不再发新调用；在途请求照常等 RESPONSE
                self._goaway = True

    def _fail_all(self, exc: Exception):
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(exc)
        self._pending.clear()
        self._closed = True

    def _write_cancel(self, rid: int):
        """尽力而为：连接已断则无事可做（服务端也随之停了）。"""
        if self.connected:
            self._writer.write(encode({"msgType": CANCEL, "requestId": rid}))

    async def call(self, method: str, body: bytes, timeout: float = 0.5,
                   task_id: int = 0) -> dict:
        """单次调用。timeout 只用于计算绝对 deadline（写入 TLV）与本地等待；
        超时后发 CANCEL 再抛 TimeoutError。"""
        if self._goaway:
            raise ConnectionError("goaway")
        if not self.connected:
            raise ConnectionError("not connected")
        self._next_id += 1
        rid = self._next_id
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[rid] = fut

        wire = encode({
            "msgType": REQUEST,
            "requestId": rid,
            "method": method,
            "body": body,
            "deadlineUnixMs": int(time.time() * 1000 + timeout * 1000),
            "taskId": task_id,
        })
        try:
            self._writer.write(wire)
            await self._writer.drain()
        except (ConnectionError, RuntimeError):
            self._pending.pop(rid, None)
            raise ConnectionError("send failed")

        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            # pop 到 None 可能是响应到达，也可能是 _fail_all（断连）清空了表。
            # 只有 future 已成功完成才取结果；其余一律 CANCEL + 抛超时。
            if (self._pending.pop(rid, None) is None
                    and fut.done()
                    and not fut.cancelled()
                    and fut.exception() is None):
                return fut.result()
            self._write_cancel(rid)
            raise

    async def cancel(self, rid: int):
        """对在途请求发 CANCEL（不摘 pending：promise 由 CANCELLED 响应完成）。"""
        if not self.connected:
            return
        self._writer.write(encode({"msgType": CANCEL, "requestId": rid}))
        await self._writer.drain()

    async def ping(self):
        wire = encode({"msgType": PING, "requestId": 0})
        self._writer.write(wire)
        await self._writer.drain()

    async def close(self):
        self._closed = True
        if self._writer:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except (ConnectionError, OSError):
                pass
        if self._reader_task:
            self._reader_task.cancel()
