#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <utility>

namespace nskry {
// Capture delegates own a shared gate, not a raw owner lifetime. Queued calls
// after Close never dereference the owner. Close must not run inside Run.
template<class Owner> class CallbackGate {
public:
    explicit CallbackGate(Owner* owner) : m_owner(owner) {}
    template<class Function> void Run(Function&& function) {
        Owner* owner{};
        {
            std::lock_guard lock(m_mutex);
            if (!m_owner) return;
            owner = m_owner; ++m_active;
        }
        struct Lease {
            CallbackGate& gate;
            ~Lease() {
                std::lock_guard lock(gate.m_mutex);
                if (--gate.m_active == 0) gate.m_drained.notify_all();
            }
        } lease{*this};
        std::invoke(std::forward<Function>(function), *owner);
    }
    void Close() {
        std::unique_lock lock(m_mutex);
        m_owner = nullptr;
        m_drained.wait(lock, [this] { return m_active == 0; });
    }
private:
    Owner* m_owner{};
    unsigned m_active{};
    std::mutex m_mutex;
    std::condition_variable m_drained;
};
}
