#include "P2PMainThreadDispatch.hpp"

#include <m_pd.h>

#include <memory>
#include <mutex>

struct DispatchMessage {
    std::function<void()> m_Function;
};

struct DispatchReceiver {
    t_object m_Object;
};

struct P2PMainThreadDispatch::State {
    std::mutex m_Mutex;
    t_pd *m_Receiver{nullptr};
};

// ─────────────────────────────────────
P2PMainThreadDispatch::State &P2PMainThreadDispatch::GetState() {
    static State state;
    return state;
}

// ─────────────────────────────────────
static void DispatchMessageCallback(t_pd *, void *data) {
    std::unique_ptr<DispatchMessage> message(static_cast<DispatchMessage *>(data));
    if (message->m_Function) {
        message->m_Function();
    }
}

// ─────────────────────────────────────
void P2PMainThreadDispatch::Initialize() {
    State &state = GetState();
    std::lock_guard<std::mutex> lock(state.m_Mutex);
    if (state.m_Receiver) {
        return;
    }
    t_class *dispatch_class = class_new(gensym("_p2p.mainthread.dispatch"), nullptr, nullptr,
                                        sizeof(DispatchReceiver), CLASS_PD, A_NULL, 0);
    state.m_Receiver = pd_new(dispatch_class);
}

// ─────────────────────────────────────
void P2PMainThreadDispatch::Enqueue(std::function<void()> function) {
    Initialize();
    State &state = GetState();
    DispatchMessage *message = new DispatchMessage{std::move(function)};
    pd_queue_mess(&pd_maininstance, state.m_Receiver, message, DispatchMessageCallback);
}
