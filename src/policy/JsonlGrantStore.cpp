#include "perfacet/policy/JsonlGrantStore.h"
#include "detail/Random.h"
#include "detail/Time.h"

#include <nlohmann/json.hpp>
#include <sys/stat.h>

#include <fstream>
#include <vector>

namespace perfacet {

JsonlGrantStore::JsonlGrantStore(std::string path, const Taxonomy* tax, ir::Rank maxBump,
                                 uint64_t ttlMs)
    : path_(std::move(path)), tax_(tax), maxBump_(maxBump), ttlMs_(ttlMs),
      table_(std::make_shared<GrantTable>()) {}

std::shared_ptr<const GrantTable> JsonlGrantStore::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return table_;
}

std::shared_ptr<GrantTable> JsonlGrantStore::parseFile() const {
    auto next = std::make_shared<GrantTable>();
    std::ifstream in(path_);
    std::string line;
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
        next->byId[r.id] = std::move(r);
    }
    return next;
}

void JsonlGrantStore::refreshOnWorker() {
    struct stat st {};
    if (::stat(path_.c_str(), &st) != 0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (dirty_) return;
        if (lastMtimeNs_ != 0) {
            lastMtimeNs_ = 0;
            table_ = std::make_shared<GrantTable>();
        }
        return;
    }
    const int64_t mtime =
        static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (dirty_) return;
        if (mtime == lastMtimeNs_) return;
    }
    auto next = parseFile();
    std::vector<GrantRecord> expired;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (dirty_) return;
        lastMtimeNs_ = mtime;
        table_ = next;
        const uint64_t now = nowMs();
        for (const auto& kv : next->byId) {
            const auto& g = kv.second;
            if (g.status == "approved" && g.expiresAt > 0 && now >= g.expiresAt) {
                if (!expireEmitted_[g.id] && onExpire_) {
                    expireEmitted_[g.id] = true;
                    expired.push_back(g);
                }
            }
        }
    }
    for (const auto& g : expired) {
        if (onExpire_) onExpire_(g);
    }
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto next = std::make_shared<GrantTable>();
        if (table_) next->byId = table_->byId;
        next->byId[r.id] = r;
        table_ = std::move(next);
        lastMtimeNs_ = -1;
        dirty_ = true;
    }
    nlohmann::json j{
        {"id", r.id},
        {"agent", r.agent},
        {"bump_to", r.bumpTo},
        {"rank", r.rank},
        {"status", r.status},
        {"expiresAt", r.expiresAt},
        {"ts_ms", r.tsMs},
    };
    auto write = [path = path_, line = j.dump()]() {
        std::ofstream out(path, std::ios::app);
        out << line << '\n';
        out.flush();
    };
    auto clearDirty = [this]() {
        std::lock_guard<std::mutex> lk(mu_);
        dirty_ = false;
    };
    auto job = [write, clearDirty]() {
        write();
        clearDirty();
    };
    if (post_) {
        try {
            post_(std::move(job));
        } catch (...) {
            job();
        }
    } else {
        job();
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

bool JsonlGrantStore::approveById(const std::string& id, uint64_t now, uint64_t ttlOverride) {
    refreshOnWorker();
    auto snap = snapshot();
    auto it = snap->byId.find(id);
    if (it == snap->byId.end()) return false;
    GrantRecord r = it->second;
    r.status = "approved";
    const uint64_t ttl = ttlOverride > 0 ? ttlOverride : ttlMs_;
    r.expiresAt = now + ttl;
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
