"""twigrpc codec: 线上协议编解码（struct 大端 + TLV），与 C++ 共享黄金字节向量。

帧格式（24B 定长头 + metadata + body，全大端）：
  magic(2) version(1) flags(1) msgType(1) status(1) reserved(2)
  requestId(8) metaLen(4) bodyLen(4) metadata(TLV) body
"""
import struct

MAGIC = 0xC75A
VERSION = 1
MAX_BODY = 16 << 20  # 16MB
MAX_META = 64 << 10  # 64KB，防 metaLen 内存放大

REQUEST, RESPONSE, PING, PONG, GOAWAY, CANCEL = 0, 1, 2, 3, 4, 5
STATUS_OK, STATUS_TIMEOUT, STATUS_NO_METHOD = 0, 1, 2
STATUS_DECODE_ERROR, STATUS_HANDLER_EXCEPTION = 3, 4
STATUS_BUSY, STATUS_CONN_CLOSED, STATUS_INTERNAL = 5, 6, 7
STATUS_CANCELLED = 8
STATUS_REJECTED = 9  # 仅本地：Agent 准入拒绝，与 C++ Status::REJECTED 对齐

META_METHOD = 0x01
META_DEADLINE_UNIX_MS = 0x04
META_TASK_ID = 0x05

HEADER = struct.Struct("!HBBBBHQII")  # magic ver flags msgType status reserved id metaLen bodyLen
HEADER_SIZE = HEADER.size  # 24


def encode(frame: dict) -> bytes:
    """frame: {msgType, status=0, flags=0, requestId=0, method='', body=b'',
    deadlineUnixMs=0, taskId=0}（非零才写对应 TLV）"""
    method = frame.get("method", "").encode()
    body = frame.get("body", b"")
    if isinstance(body, str):
        body = body.encode()
    deadline = frame.get("deadlineUnixMs", 0)
    task_id = frame.get("taskId", 0)
    if len(method) > 0xFFFF:
        raise ValueError("method name too long (>65535)")
    meta = b""
    if method:
        meta += struct.pack("!BH", META_METHOD, len(method)) + method
    if deadline:
        meta += struct.pack("!BHQ", META_DEADLINE_UNIX_MS, 8, deadline)
    if task_id:
        meta += struct.pack("!BHQ", META_TASK_ID, 8, task_id)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        frame.get("flags", 0),
        frame["msgType"],
        frame.get("status", 0),
        0,  # reserved
        frame.get("requestId", 0),
        len(meta),
        len(body),
    )
    return header + meta + body


NEED_MORE, OK, ERROR = "need_more", "ok", "error"


def decode(buf: bytes):
    """尝试从 buf 解出一帧。返回 (result, frame|None)。
    result: NEED_MORE / OK / ERROR。OK 时返回 (OK, frame) 且消费的字节数在 frame['_consumed']。
    """
    if len(buf) < HEADER_SIZE:
        return NEED_MORE, None
    (magic, version, flags, msg_type, status, _reserved,
     request_id, meta_len, body_len) = HEADER.unpack_from(buf, 0)
    if magic != MAGIC or version != VERSION:
        return ERROR, None
    if meta_len > MAX_META or body_len > MAX_BODY:
        return ERROR, None
    total = HEADER_SIZE + meta_len + body_len
    if len(buf) < total:
        return NEED_MORE, None

    meta_raw = buf[HEADER_SIZE:HEADER_SIZE + meta_len]
    body = buf[HEADER_SIZE + meta_len:total]

    method = b""
    deadline = 0
    task_id = 0
    i = 0
    while i + 3 <= len(meta_raw):
        mtype = meta_raw[i]
        (mlen,) = struct.unpack_from("!H", meta_raw, i + 1)
        if i + 3 + mlen > len(meta_raw):
            break
        if mtype == META_METHOD:
            method = meta_raw[i + 3:i + 3 + mlen]
        elif mtype == META_DEADLINE_UNIX_MS and mlen == 8:
            (deadline,) = struct.unpack_from("!Q", meta_raw, i + 3)
        elif mtype == META_TASK_ID and mlen == 8:
            (task_id,) = struct.unpack_from("!Q", meta_raw, i + 3)
        i += 3 + mlen

    frame = {
        "msgType": msg_type,
        "status": status,
        "flags": flags,
        "requestId": request_id,
        "deadlineUnixMs": deadline,
        "taskId": task_id,
        "method": method.decode(),
        "body": body,
        "_consumed": total,
    }
    return OK, frame
