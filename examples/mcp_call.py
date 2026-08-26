#!/usr/bin/env python3
"""向本机 Perfacet 发一条 MCP 2026-07-28 POST /mcp。"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request

PROTOCOL = "2026-07-28"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--token", required=True)
    ap.add_argument("--method", required=True)
    ap.add_argument("--name", default="")
    ap.add_argument("--args", default="{}")
    ap.add_argument("--meta", default="{}")
    ap.add_argument("--tasks", action="store_true")
    ap.add_argument("--url", default="http://127.0.0.1:8741/mcp")
    args = ap.parse_args()
    extra_args = json.loads(args.args)
    extra_meta = json.loads(args.meta)
    caps: dict = {}
    if args.tasks:
        caps = {"extensions": {"io.modelcontextprotocol/tasks": {}}}
    meta = {
        "io.modelcontextprotocol/protocolVersion": PROTOCOL,
        "io.modelcontextprotocol/clientCapabilities": caps,
    }
    meta.update(extra_meta)
    params: dict = {"_meta": meta}
    if args.method == "tools/call":
        params["name"] = args.name
        params["arguments"] = extra_args
    elif args.method.startswith("tasks/"):
        params["taskId"] = args.name
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": args.method, "params": params}).encode()
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "MCP-Protocol-Version": PROTOCOL,
        "Mcp-Method": args.method,
        "Authorization": f"Bearer {args.token}",
    }
    if args.name:
        headers["Mcp-Name"] = args.name
    req = urllib.request.Request(args.url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            sys.stdout.write(resp.read().decode())
    except urllib.error.HTTPError as e:
        sys.stdout.write(e.read().decode())
        sys.exit(0)


if __name__ == "__main__":
    main()
