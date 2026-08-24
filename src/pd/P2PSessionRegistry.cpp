#include "P2PSessionRegistry.hpp"

#include "P2PMainThreadDispatch.hpp"
#include "P2PSession.hpp"

#include <mutex>
#include <unordered_map>

// GLOBAL
std::mutex registry_mutex;
std::unordered_map<std::string, std::weak_ptr<P2PSession>> sessions;

// ─────────────────────────────────────
std::shared_ptr<P2PSession> P2PSessionRegistry::acquire(const std::string &id, int sample_rate) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto &entry = sessions[id];
    auto session = entry.lock();
    if (!session) {
        session = P2PSession::create(id, sample_rate, P2PMainThreadDispatch::enqueue);
        entry = session;
    }
    return session;
}

// ─────────────────────────────────────
std::shared_ptr<P2PSession> P2PSessionRegistry::find(const std::string &id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto iterator = sessions.find(id);
    if (iterator == sessions.end()) {
        return {};
    }
    auto session = iterator->second.lock();
    if (!session || !session->available()) {
        return {};
    }
    return session;
}

// ─────────────────────────────────────
void P2PSessionRegistry::release(const std::string &id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    sessions.erase(id);
}

// ─────────────────────────────────────
void P2PSessionRegistry::release(const std::string &id,
                                 const std::shared_ptr<P2PSession> &session) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto iterator = sessions.find(id);
    if (iterator != sessions.end() && iterator->second.lock() == session) {
        sessions.erase(iterator);
    }
}
