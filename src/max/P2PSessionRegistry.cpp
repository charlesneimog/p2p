#include "P2PSessionRegistry.hpp"

#include "P2PMainThreadDispatch.hpp"
#include "P2PSession.hpp"

#include <mutex>
#include <unordered_map>

#include <ext.h>

struct P2PSessionRegistry::State {
    std::mutex m_Mutex;
    std::unordered_map<std::string, std::weak_ptr<P2PSession>> m_Sessions;
};

// ─────────────────────────────────────
P2PSessionRegistry::State &P2PSessionRegistry::GetState() {
    // Max's symbol table shares the registry across independently loaded externals.
    // The host owns this anchor for the process lifetime.
    t_symbol *anchor = gensym("#p2p.session.registry");
    if (!anchor->s_thing) {
        anchor->s_thing = reinterpret_cast<t_object *>(new State());
    }
    return *reinterpret_cast<State *>(anchor->s_thing);
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
