#pragma once

#include <memory>
#include <string>

class P2PSession;

class P2PSessionRegistry {
public:
    static std::shared_ptr<P2PSession> acquire(const std::string &id);
    static std::shared_ptr<P2PSession> find(const std::string &id);
    static void release(const std::string &id);
    static void release(const std::string &id, const std::shared_ptr<P2PSession> &session);
};
