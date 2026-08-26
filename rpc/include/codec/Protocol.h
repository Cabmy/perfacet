#pragma once
// twigrpc::Protocol —— 线上协议常量与帧结构（DESIGN.md §三）。
// rpc 层只依赖 netlib 公开接口，禁止出现 epoll/Socket 级系统调用。
//
// 帧格式（24B 定长头 + metadata + body，全大端）：
//   偏移 长度 字段
//   0    2   magic    0xC75A
//   2    1   version  =1
//   3    1   flags    bit0:body压缩 bit1:含trace bit2:body为JSON调试模式
//   4    1   msgType  0=REQUEST 1=RESPONSE 2=PING 3=PONG 4=GOAWAY 5=CANCEL
//   5    1   status   仅 RESPONSE
//   6    2   reserved
//   8    8   requestId u64
//   16   4   metaLen  u32
//   20   4   bodyLen  u32 ≤16MB
//   24   ..  metadata  TLV：1B类型 + 2B长度 + 值
//   ..   ..  body      Protobuf bytes
#include <cstdint>
#include <string>

namespace twigrpc {

inline constexpr uint16_t kMagic = 0xC75A;
inline constexpr uint8_t kVersion = 1;
inline constexpr uint32_t kMaxBody = 16u << 20; // 16MB 协议硬上限
inline constexpr uint32_t kMaxMeta = 64u << 10;  // 64KB metadata 硬上限（防内存放大）

enum class MsgType : uint8_t {
    REQUEST = 0,
    RESPONSE = 1,
    PING = 2,
    PONG = 3,
    GOAWAY = 4,
    CANCEL = 5, // payload 空；requestId = 要取消的那次 RPC
};

enum class Status : uint8_t {
    OK = 0,
    TIMEOUT = 1,
    NO_METHOD = 2,
    DECODE_ERROR = 3,
    HANDLER_EXCEPTION = 4,
    BUSY = 5,
    CONN_CLOSED = 6,
    INTERNAL = 7,
    CANCELLED = 8,
    REJECTED = 9, // 仅本地：Agent 准入拒绝，不上线
};

// flags 位定义
inline constexpr uint8_t kFlagBodyCompressed = 0x01;
inline constexpr uint8_t kFlagHasTrace = 0x02;
inline constexpr uint8_t kFlagBodyJson = 0x04;

// TLV metadata 类型。接收方遇到未知 TLV 类型直接跳过——协议天然向前演进。
enum MetaType : uint8_t {
    kMetaMethod = 0x01, // package.Service.Method 全名
    kMetaService = 0x02,
    kMetaTrace = 0x03,
    kMetaDeadlineUnixMs = 0x04, // 8B 大端：Unix 毫秒绝对期限
    kMetaTaskId = 0x05,        // 8B 大端：Agent 任务 id（rpc 不解释，仅透传）
};

struct Frame {
    MsgType msgType = MsgType::REQUEST;
    Status status = Status::OK;
    uint8_t flags = 0;
    uint64_t requestId = 0;
    uint64_t deadlineUnixMs = 0; // 0 = 缺省（无期限）
    uint64_t taskId = 0;         // 0 = 缺省（非 Agent 调用）
    std::string method; // TLV 中的方法名
    std::string body;
};

inline const char* statusName(Status s) {
    switch (s) {
        case Status::OK: return "OK";
        case Status::TIMEOUT: return "TIMEOUT";
        case Status::NO_METHOD: return "NO_METHOD";
        case Status::DECODE_ERROR: return "DECODE_ERROR";
        case Status::HANDLER_EXCEPTION: return "HANDLER_EXCEPTION";
        case Status::BUSY: return "BUSY";
        case Status::CONN_CLOSED: return "CONN_CLOSED";
        case Status::INTERNAL: return "INTERNAL";
        case Status::CANCELLED: return "CANCELLED";
        case Status::REJECTED: return "REJECTED";
    }
    return "UNKNOWN";
}

} // namespace twigrpc
