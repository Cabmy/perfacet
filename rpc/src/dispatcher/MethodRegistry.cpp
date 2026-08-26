#include "dispatcher/MethodRegistry.h"

#include <cstdio>

namespace twigrpc {

bool MethodRegistry::dispatch(const Frame& req, const CancelToken& tok,
                              std::string* respBody, Status* st,
                              std::string* errDetail) {
    auto it = methods_.find(req.method);
    if (it == methods_.end()) {
        if (st) *st = Status::NO_METHOD;
        if (errDetail) *errDetail = "unknown method: " + req.method;
        return false;
    }
    // 已取消：不调 handler，直接 CANCELLED
    if (tok.cancelled()) {
        if (st) *st = Status::CANCELLED;
        if (errDetail) *errDetail = "cancelled";
        return false;
    }
    try {
        std::string out = it->second(req.body, tok);
        if (out == kDecodeErrorMark) {
            if (st) *st = Status::DECODE_ERROR;
            if (errDetail) *errDetail = "request ParseFromString failed";
            return false;
        }
        if (respBody) *respBody = std::move(out);
        if (st) *st = Status::OK;
        return true;
    } catch (const std::exception& e) {
        if (st) *st = Status::HANDLER_EXCEPTION;
        if (errDetail) *errDetail = e.what();
        return false;
    } catch (...) {
        if (st) *st = Status::HANDLER_EXCEPTION;
        if (errDetail) *errDetail = "unknown handler exception";
        return false;
    }
}

} // namespace twigrpc
