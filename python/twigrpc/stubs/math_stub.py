"""math 服务手写绑定示例：每方法一个薄封装（<20 行）。
raw_call: async fn(method, body_bytes, timeout) -> frame dict"""
from . import math_pb2


class MathStub:
    def __init__(self, raw_call):
        self._call = raw_call

    async def _invoke(self, method, req, timeout, resp_type):
        frame = await self._call(method, req.SerializeToString(), timeout)
        if frame["status"] != 0:
            raise RuntimeError(f"rpc failed: status={frame['status']}")
        resp = resp_type()
        resp.ParseFromString(frame["body"])
        return resp

    async def add(self, a: int, b: int, timeout: float = 0.5) -> int:
        return (await self._invoke(
            "math.Add", math_pb2.AddRequest(a=a, b=b), timeout,
            math_pb2.AddResponse)).result

    async def echo(self, msg: str, repeat: int = 1, timeout: float = 0.5) -> str:
        return (await self._invoke(
            "math.Echo", math_pb2.EchoRequest(msg=msg, repeat=repeat), timeout,
            math_pb2.EchoResponse)).msg
