#pragma once

#include <functional>

class P2PMainThreadDispatch {
public:
    static void initialize();
    static void enqueue(std::function<void()> function);
};
