#pragma once
// twigrpc::MethodRegistry —— 服务端方法表，类型擦除注册。
// handler 签名固定为 proto message 进出（跨语言对齐）；序列化由 bind 模板内完成。
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "codec/Protocol.h"
#include "server/CancelToken.h"

namespace twigrpc {

// 坏请求标记：dispatch 检测到后转 DECODE_ERROR。
// 首字节 \x00 不可能是合法 protobuf 序列化的开头（field tag ≥ 0x08），
// 且必须用显式长度构造——const char* 构造遇 \x00 会截断成空串（曾导致
// 空 body 的合法响应被误判为解码错误）。
inline const std::string kDecodeErrorMark =
    std::string("\x00__TWIGRPC_DECODE_ERROR__", 25);

class MethodRegistry {
public:
    // 入：req body bytes + 取消令牌；出：resp body bytes
    using Handler = std::function<std::string(const std::string&, const CancelToken&)>;

    // 注册：ParseFromString(req) → 调用（慢 handler 轮询 tok.cancelled()）
    // → SerializeToString(resp)
    template <typename Req, typename Resp>
    void bind(const std::string& name,
              std::function<Resp(const Req&, const CancelToken&)> f) {
        methods_[name] = [f = std::move(f)](
                               const std::string& reqBody,
                               const CancelToken& tok) -> std::string {
            Req req;
            if (!req.ParseFromString(reqBody)) {
                return kDecodeErrorMark; // → DECODE_ERROR
            }
            Resp resp = f(req, tok);
            std::string out;
            if (!resp.SerializeToString(&out)) {
                throw std::runtime_error("response serialize failed");
            }
            return out;
        };
    }

    bool has(const std::string& name) const { return methods_.count(name) > 0; }
    size_t size() const { return methods_.size(); }
    // 已注册方法名列表（Collector 预注册分片条目用；serve 后只读）
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(methods_.size());
        for (auto& [n, _] : methods_) out.push_back(n);
        return out;
    }

    // 分发入口：查表（不命中 NO_METHOD）、调 Handler、
    // 解析失败转 DECODE_ERROR、异常转 HANDLER_EXCEPTION。
    // 已取消（tok.cancelled()）则不调 handler，直接 CANCELLED。
    bool dispatch(const Frame& req, const CancelToken& tok, std::string* respBody,
                  Status* st, std::string* errDetail = nullptr);

private:
    std::unordered_map<std::string, Handler> methods_;
};

} // namespace twigrpc
