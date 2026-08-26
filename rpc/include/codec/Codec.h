#pragma once
// twigrpc::Codec —— 协议编解码（纯函数 + 无状态，最易单测）。
#include "netlib/Buffer.h"
#include "codec/Protocol.h"

namespace twigrpc {

class Codec {
public:
    // 编码：Frame → 追加进 Buffer
    static void encode(const Frame& f, netlib::Buffer& out);

    struct DecodeResult {
        enum Kind { NEED_MORE, OK, ERROR } kind;
        Frame frame;
    };

    // 解码：尝试从 Buffer 取出一帧（拆包状态机由每连接独立持有 Buffer 实现）
    static DecodeResult decode(netlib::Buffer& in);
};

} // namespace twigrpc
