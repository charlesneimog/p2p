#pragma once

#include <functional>

class P2PMainThreadDispatch {
public:
    static void Initialize();
    static void Enqueue(std::function<void()> function);

private:
    struct State;
    static State &GetState();
};
