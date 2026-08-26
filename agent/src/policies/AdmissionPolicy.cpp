#include "agent/policies/AdmissionPolicy.h"

#include <string>

namespace agent {

Decision AdmissionPolicy::evaluate(const TaskContext&, const RpcRequest&) {
    if (inFlight_.fetch_add(1) + 1 > max_) {
        inFlight_.fetch_sub(1); // 回滚：Reject 不占槽
        return Decision::Reject("max concurrent " + std::to_string(max_) +
                                " exceeded");
    }
    return Decision::Admit();
}

void AdmissionPolicy::onComplete(const TaskContext&, twigrpc::Status) {
    inFlight_.fetch_sub(1);
}

} // namespace agent
