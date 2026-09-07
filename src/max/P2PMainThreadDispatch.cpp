#include "P2PMainThreadDispatch.hpp"

#include <ext.h>

#include <deque>
#include <mutex>

struct P2PMainThreadDispatch::State {
    std::mutex m_Mutex;
    std::deque<std::function<void()>> m_Queue;
    void *m_Qelem{nullptr};

    static void Drain(State *state) {
        for (;;) {
            std::function<void()> function;
            {
                std::lock_guard<std::mutex> lock(state->m_Mutex);
                if (state->m_Queue.empty()) {
                    break;
                }
                function = std::move(state->m_Queue.front());
                state->m_Queue.pop_front();
            }
            if (function) {
                function();
            }
        }
    }
};

// ─────────────────────────────────────
P2PMainThreadDispatch::State &P2PMainThreadDispatch::GetState() {
    static State state;
    return state;
}

// ─────────────────────────────────────
void P2PMainThreadDispatch::Initialize() {
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.m_Mutex);
    if (!state.m_Qelem) {
        state.m_Qelem = qelem_new(&state, reinterpret_cast<method>(State::Drain));
    }
}

// ─────────────────────────────────────
void P2PMainThreadDispatch::Enqueue(std::function<void()> function) {
    Initialize();
    State &state = GetState();
    {
        std::lock_guard<std::mutex> lock(state.m_Mutex);
        state.m_Queue.push_back(std::move(function));
    }
    qelem_set(state.m_Qelem);
}
