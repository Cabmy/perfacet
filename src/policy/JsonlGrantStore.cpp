#include "perfacet/policy/JsonlGrantStore.h"
#include "detail/Random.h"
#include "detail/Time.h"

#include <nlohmann/json.hpp>
#include <sys/stat.h>

#include <fstream>
#include <sstream>

namespace perfacet {

JsonlGrantStore::JsonlGrantStore(std::string path, const Taxonomy* tax, ir::Rank maxBump,
                                 uint64_t ttlMs)
    : path_(std::move(path)), tax_(tax), maxBump_(maxBump), ttlMs_(ttlMs),
      table_(std::make_shared<GrantTable>()) {}

std::shared_ptr<const GrantTable> JsonlGrantStore::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return table_;
}

void JsonlGrantStore::refreshOnWorker() {
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0) {
        // 文件尚不存在：空表
        std::lock_guard<std::mutex> lk(mu_);
        if (lastMtimeNs_ != 0) {
            lastMtimeNs_ = 0;
            table_ = std::make_shared<GrantTable>();
        }
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    lastMtimeNs_ =
        static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    parseFileUnlocked();
}

void JsonlGrantStore::parseFileUnlocked() {
    auto next = std::make_shared<GrantTable>();
    std::ifstream in(path_);
    std::string line;
    const uint64_t now = nowMs();
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        GrantRecord r;
        r.id = j.value("id", "");
        r.agent = j.value("agent", "");
        r.bumpTo = j.value("bump_to", "");
        r.rank = static_cast<ir::Rank>(j.value("rank", 0));
        r.status = j.value("status", "");
        r.expiresAt = j.value("expiresAt", uint64_t{0});
        r.tsMs = j.value("ts_ms", uint64_t{0});
        if (r.id.empty()) continue;
        next->byId[r.id] = r;
        if (r.status == "approved" && r.expiresAt > 0 && now >= r.expiresAt) {
            if (!expireEmitted_[r.id] && onExpire_) {
                expireEmitted_[r.id] = true;
                onExpire_(r);
            }
        }
    }
    table_ = std::move(next);
}

ir::Rank JsonlGrantStore::effectiveBump(std::string_view agentId, uint64_t nowMs) const {
    auto snap = snapshot();
    ir::Rank bump = 0;
    for (const auto& kv : snap->byId) {
        const auto& g = kv.second;
        if (g.agent != agentId) continue;
        if (g.status != "approved") continue;
        if (nowMs >= g.expiresAt) continue;
        if (g.rank > bump) bump = g.rank;
    }
    return bump;
}

uint64_t JsonlGrantStore::shortestRemainingMs(std::string_view agentId,
                                              uint64_t now) const {
    auto snap = snapshot();
    uint64_t best = UINT64_MAX;
    for (const auto& kv : snap->byId) {
        const auto& g = kv.second;
        if (g.agent != agentId || g.status != "approved") continue;
        if (now >= g.expiresAt) continue;
        const uint64_t rem = g.expiresAt - now;
        if (rem < best) best = rem;
    }
    return best;
}

void JsonlGrantStore::appendLine(const GrantRecord& r) {
    nlohmann::json j{
        {"id", r.id},
        {"agent", r.agent},
        {"bump_to", r.bumpTo},
        {"rank", r.rank},
        {"status", r.status},
        {"expiresAt", r.expiresAt},
        {"ts_ms", r.tsMs},
    };
    std::ofstream out(path_, std::ios::app);
    out << j.dump() << '\n';
    out.flush();
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto next = std::make_shared<GrantTable>();
        if (table_) next->byId = table_->byId;
        next->byId[r.id] = r;
        table_ = std::move(next);
        lastMtimeNs_ = -1;
    }
}

std::string JsonlGrantStore::appendPending(const std::string& agent,
                                           const std::string& bumpTo, ir::Rank rank,
                                           uint64_t now) {
    GrantRecord r;
    r.id = "g_" + randomHex(8);
    r.agent = agent;
    r.bumpTo = bumpTo;
    r.rank = rank;
    r.status = "pending";
    r.expiresAt = 0;
    r.tsMs = now;
    appendLine(r);
    return r.id;
}

bool JsonlGrantStore::approveById(const std::string& id, uint64_t now) {
    refreshOnWorker();
    auto snap = snapshot();
    auto it = snap->byId.find(id);
    if (it == snap->byId.end()) return false;
    GrantRecord r = it->second;
    r.status = "approved";
    r.expiresAt = now + ttlMs_;
    r.tsMs = now;
    appendLine(r);
    return true;
}

bool JsonlGrantStore::approveDirect(const std::string& agent, ir::Rank rank,
                                    const std::string& bumpTo, uint64_t now) {
    GrantRecord r;
    r.id = "g_" + randomHex(8);
    r.agent = agent;
    r.bumpTo = bumpTo;
    r.rank = rank;
    r.status = "approved";
    r.expiresAt = now + ttlMs_;
    r.tsMs = now;
    appendLine(r);
    return true;
}

} // namespace perfacet
