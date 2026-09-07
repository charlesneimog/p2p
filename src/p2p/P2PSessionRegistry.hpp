#pragma once

#include <memory>
#include <string>

class P2PSession;

class P2PSessionRegistry {
public:
    static std::shared_ptr<P2PSession> Acquire(const std::string &id, int sample_rate);
    static std::shared_ptr<P2PSession> Find(const std::string &id);
    static void Release(const std::string &id);
    static void Release(const std::string &id, const std::shared_ptr<P2PSession> &session);

private:
    struct State;
    static State &GetState();
};
