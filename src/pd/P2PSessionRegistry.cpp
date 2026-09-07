#include "P2PSessionRegistry.hpp"

#include "P2PMainThreadDispatch.hpp"
#include "P2PSession.hpp"

#include <mutex>
#include <unordered_map>

struct P2PSessionRegistry::State {
    std::mutex m_Mutex;
    std::unordered_map<std::string, std::weak_ptr<P2PSession>> m_Sessions;
};

// ─────────────────────────────────────
P2PSessionRegistry::State &P2PSessionRegistry::GetState() {
    static State state;
    return state;
}

// ─────────────────────────────────────
std::shared_ptr<P2PSession> P2PSessionRegistry::Acquire(const std::string &id, int sample_rate) {
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.m_Mutex);
    std::weak_ptr<P2PSession> &entry = state.m_Sessions[id];
    std::shared_ptr<P2PSession> session = entry.lock();
    if (!session) {
        session = P2PSession::Create(id, sample_rate, P2PMainThreadDispatch::Enqueue);
        entry = session;
    }
    return session;
}

// ─────────────────────────────────────
std::shared_ptr<P2PSession> P2PSessionRegistry::Find(const std::string &id) {
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.m_Mutex);
    auto iterator = state.m_Sessions.find(id);
    if (iterator == state.m_Sessions.end()) {
        return {};
    }
    std::shared_ptr<P2PSession> session = iterator->second.lock();
    if (!session || !session->Available()) {
        return {};
    }
    return session;
}

// ─────────────────────────────────────
void P2PSessionRegistry::Release(const std::string &id) {
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.m_Mutex);
    state.m_Sessions.erase(id);
}

// ─────────────────────────────────────
void P2PSessionRegistry::Release(const std::string &id,
                                 const std::shared_ptr<P2PSession> &session) {
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.m_Mutex);
    auto iterator = state.m_Sessions.find(id);
    if (iterator != state.m_Sessions.end() && iterator->second.lock() == session) {
        state.m_Sessions.erase(iterator);
    }
}
