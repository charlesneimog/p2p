#include "P2PSessionRegistry.hpp"

#include "P2PSession.hpp"

#include <ext.h>

#include <mutex>
#include <unordered_map>

namespace {
struct RegistryState {
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<P2PSession>> sessions;
};

RegistryState &registry() {
    // Every .mxo contains its own static copy of this function. Max's symbol
    // table belongs to the host process, however, so s_thing provides one
    // shared anchor for all four independently loaded externals.
    t_symbol *anchor = gensym("#p2p.session.registry");
    if (!anchor->s_thing) {
        anchor->s_thing = reinterpret_cast<t_object *>(new RegistryState());
    }
    return *reinterpret_cast<RegistryState *>(anchor->s_thing);
}
}

std::shared_ptr<P2PSession> P2PSessionRegistry::acquire(const std::string &id) {
    auto &state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto &entry = state.sessions[id];
    auto session = entry.lock();
    if (!session) {
        session = P2PSession::create(id);
        entry = session;
    }
    return session;
}

std::shared_ptr<P2PSession> P2PSessionRegistry::find(const std::string &id) {
    auto &state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto iterator = state.sessions.find(id);
    if (iterator == state.sessions.end()) {
        return {};
    }
    auto session = iterator->second.lock();
    if (!session || !session->available()) {
        return {};
    }
    return session;
}

void P2PSessionRegistry::release(const std::string &id) {
    auto &state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sessions.erase(id);
}

void P2PSessionRegistry::release(const std::string &id,
                                 const std::shared_ptr<P2PSession> &session) {
    auto &state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto iterator = state.sessions.find(id);
    if (iterator != state.sessions.end() && iterator->second.lock() == session) {
        state.sessions.erase(iterator);
    }
}
