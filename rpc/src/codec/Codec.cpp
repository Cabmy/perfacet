#include "codec/Codec.h"

#include <cstdio>
#include <stdexcept>

namespace twigrpc {

void Codec::encode(const Frame& f, netlib::Buffer& out) {
    // metadata：多个 TLV（type + 2B len + value），非空才写。
    // TLV 长度字段是 16 位：超长 method 直接拒绝，否则帧头部与实际内容自相矛盾
    const std::string& m = f.method;
    if (m.size() > 0xFFFF) {
        throw std::invalid_argument("Codec: method name too long (>65535)");
    }
    uint32_t metaLen = 0;
    if (!m.empty()) metaLen += 3u + static_cast<uint32_t>(m.size());
    if (f.deadlineUnixMs != 0) metaLen += 3u + 8u;
    if (f.taskId != 0) metaLen += 3u + 8u;
    const uint32_t bodyLen = static_cast<uint32_t>(f.body.size());

    // 24B 定长头（全大端）
    out.appendUint16(kMagic);
    out.appendUint8(kVersion);
    out.appendUint8(f.flags);
    out.appendUint8(static_cast<uint8_t>(f.msgType));
    out.appendUint8(static_cast<uint8_t>(f.status));
    uint16_t reserved = 0;
    out.appendUint16(reserved);
    out.appendUint64(f.requestId);
    out.appendUint32(metaLen);
    out.appendUint32(bodyLen);

    // TLV metadata
    if (!m.empty()) {
        out.appendUint8(kMetaMethod);
        out.appendUint16(static_cast<uint16_t>(m.size()));
        out.append(m);
    }
    if (f.deadlineUnixMs != 0) {
        out.appendUint8(kMetaDeadlineUnixMs);
        out.appendUint16(8);
        out.appendUint64(f.deadlineUnixMs);
    }
    if (f.taskId != 0) {
        out.appendUint8(kMetaTaskId);
        out.appendUint16(8);
        out.appendUint64(f.taskId);
    }

    // body
    out.append(f.body);
}

Codec::DecodeResult Codec::decode(netlib::Buffer& in) {
    DecodeResult res;
    res.kind = DecodeResult::NEED_MORE;

    // 1) 头都不够：半帧等待
    if (in.readableBytes() < 24) return res;

    // 2) magic 错位：不可恢复，断连
    if (in.peekUint16() != kMagic) {
        res.kind = DecodeResult::ERROR;
        return res;
    }

    const uint8_t version = static_cast<uint8_t>(in.peek()[2]);
    if (version != kVersion) {
        res.kind = DecodeResult::ERROR;
        return res;
    }

    const uint32_t metaLen = in.peekUint32At(16);
    const uint32_t bodyLen = in.peekUint32At(20);
    if (metaLen > kMaxMeta || bodyLen > kMaxBody) {
        res.kind = DecodeResult::ERROR;
        return res;
    }

    const size_t total = 24u + metaLen + bodyLen;
    if (in.readableBytes() < total) return res; // 半帧等待

    // 3) 取完整帧
    Frame& f = res.frame;
    f.flags = static_cast<uint8_t>(in.peek()[3]);
    f.msgType = static_cast<MsgType>(in.peek()[4]);
    f.status = static_cast<Status>(in.peek()[5]);
    f.requestId = in.peekUint64At(8);

    in.retrieve(24); // 头部消费

    // metadata TLV 解析（未知类型跳过）
    uint32_t consumed = 0;
    while (consumed + 3 <= metaLen) {
        const uint8_t type = static_cast<uint8_t>(in.peek()[0]);
        const uint16_t len = in.peekUint16At(1);
        if (consumed + 3u + len > metaLen) break; // 长度越界，容错截断
        if (type == kMetaMethod) {
            f.method.assign(in.peek() + 3, len);
        } else if (type == kMetaDeadlineUnixMs && len == 8) {
            f.deadlineUnixMs = in.peekUint64At(3);
        } else if (type == kMetaTaskId && len == 8) {
            f.taskId = in.peekUint64At(3);
        }
        in.retrieve(3u + len);
        consumed += 3u + len;
    }
    if (consumed < metaLen) {
        in.retrieve(metaLen - consumed); // 跳过残缺 TLV
    }

    f.body.assign(in.peek(), bodyLen);
    in.retrieve(bodyLen);

    res.kind = DecodeResult::OK;
    return res;
}

} // namespace twigrpc
