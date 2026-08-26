#pragma once
#include "perfacet/ir/Request.h"

#include <functional>
#include <memory>

namespace perfacet {

class Governor {
public:
    enum class Admit { Go, Queue, Reject };

    class Permit {
        struct Impl;
        std::unique_ptr<Impl> impl_;

    public:
        Permit(); // 仅作为「未持有」；析构无操作
        Permit(Permit&&) noexcept;
        Permit& operator=(Permit&&) noexcept;
        Permit(const Permit&) = delete;
        ~Permit(); // 唯一归还路径；无公开 release
        bool held() const;

        static Permit make(Governor* gov, ir::ToolKey key, std::string agentId);

    private:
        friend class LocalGovernor;
    };

    virtual ~Governor() = default;
    virtual void acquire(const ir::Principal&, const ir::ToolKey&,
                         uint64_t waitDeadlineMs,
                         std::function<void(Admit, Permit)> onAdmit) = 0;

protected:
    virtual void releaseSlot(const ir::ToolKey&, std::string_view agentId) = 0;
    friend class Permit;
};

} // namespace perfacet
