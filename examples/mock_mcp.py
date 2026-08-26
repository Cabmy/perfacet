#!/usr/bin/env python3
"""MCP 2026-07-28 上游桩：供网关探活与 demo 剧本使用。统一用仓库根目录的 uv 环境。"""

from __future__ import annotations

import argparse
import asyncio
import json
import time
from aiohttp import web

PROTOCOL = "2026-07-28"
META_VER = "io.modelcontextprotocol/protocolVersion"
META_CAPS = "io.modelcontextprotocol/clientCapabilities"


def _tools(names: list[str]) -> list[dict]:
    out = []
    for n in names:
        out.append(
            {
                "name": n,
                "description": f"mock tool {n}",
                "inputSchema": {"type": "object"},
            }
        )
    return out


class State:
    def __init__(self, tools: list[str], fail_after: int, delay: float) -> None:
        self.tools = tools
        self.fail_after = fail_after
        self.delay = delay
        self.hits = 0


async def handle(request: web.Request) -> web.StreamResponse:
    st: State = request.app["state"]
    ver = request.headers.get("MCP-Protocol-Version", "")
    if ver != PROTOCOL:
        return web.json_response(
            {
                "jsonrpc": "2.0",
                "id": None,
                "error": {
                    "code": -32022,
                    "message": "unsupported version",
                    "data": {"supported": [PROTOCOL]},
                },
            },
            status=400,
        )
    try:
        body = await request.json()
    except Exception:
        return web.json_response(
            {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse"}},
            status=400,
        )
    rid = body.get("id")
    method = body.get("method", "")
    params = body.get("params") or {}
    meta = params.get("_meta") or {}
    if meta.get(META_VER) != PROTOCOL or META_CAPS not in meta:
        return web.json_response(
            {
                "jsonrpc": "2.0",
                "id": rid,
                "error": {"code": -32602, "message": "missing _meta"},
            },
            status=400,
        )

    st.hits += 1
    if st.fail_after > 0 and st.hits >= st.fail_after:
        return web.json_response(
            {
                "jsonrpc": "2.0",
                "id": rid,
                "error": {"code": -32000, "message": "injected failure"},
            },
            status=500,
        )

    if method == "server/discover":
        result = {
            "protocolVersion": PROTOCOL,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "mock-mcp", "version": "0.1.0"},
            "ttlMs": 5000,
            "cacheScope": "private",
        }
        return web.json_response({"jsonrpc": "2.0", "id": rid, "result": result})

    if method == "tools/list":
        result = {
            "tools": _tools(st.tools),
            "ttlMs": 5000,
            "cacheScope": "private",
        }
        return web.json_response({"jsonrpc": "2.0", "id": rid, "result": result})

    if method == "tools/call":
        name = params.get("name") or request.headers.get("Mcp-Name", "")
        if st.delay > 0:
            await asyncio.sleep(st.delay)
        text = json.dumps({"tool": name, "ok": True, "t": time.time()})
        result = {
            "content": [{"type": "text", "text": text}],
            "isError": False,
        }
        return web.json_response({"jsonrpc": "2.0", "id": rid, "result": result})

    return web.json_response(
        {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "Method not found"}},
        status=404,
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--tools", default="ping", help="comma-separated tool names")
    ap.add_argument("--fail-after", type=int, default=0)
    ap.add_argument("--delay", default="0", help="seconds (e.g. 30 or 30s) for tools/call")
    ap.add_argument("--declare-tasks", action="store_true")
    args = ap.parse_args()
    delay_s = args.delay.rstrip("s")
    delay = float(delay_s) if delay_s else 0.0
    tools = [t.strip() for t in args.tools.split(",") if t.strip()]
    app = web.Application()
    app["state"] = State(tools, args.fail_after, delay)
    app["declare_tasks"] = args.declare_tasks
    app.router.add_post("/mcp", handle)
    web.run_app(app, host="127.0.0.1", port=args.port, print=lambda *_: None)


if __name__ == "__main__":
    main()
