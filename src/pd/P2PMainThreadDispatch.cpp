#include "P2PMainThreadDispatch.hpp"

#include <m_pd.h>

#include <memory>
#include <mutex>

// ─────────────────────────────────────
struct DispatchMessage {
    std::function<void()> function;
};

// ─────────────────────────────────────
struct DispatchReceiver {
    t_object object;
};

// ─────────────────────────────────────
t_class *dispatch_class = nullptr;
t_pd *dispatch_receiver = nullptr;

// TODO: Try to avoid global
std::mutex dispatch_mutex;

// ─────────────────────────────────────
void dispatch_message(t_pd *, void *data) {
    std::unique_ptr<DispatchMessage> message(static_cast<DispatchMessage *>(data));
    if (message->function) {
        message->function();
    }
}

// ─────────────────────────────────────
void P2PMainThreadDispatch::initialize() {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    if (dispatch_receiver) {
        return;
    }
    dispatch_class = class_new(gensym("_p2p.mainthread.dispatch"), nullptr, nullptr,
                               sizeof(DispatchReceiver), CLASS_PD, A_NULL, 0);
    dispatch_receiver = pd_new(dispatch_class);
}

// ─────────────────────────────────────
void P2PMainThreadDispatch::enqueue(std::function<void()> function) {
    initialize();
    auto *message = new DispatchMessage{std::move(function)};
    pd_queue_mess(&pd_maininstance, dispatch_receiver, message, dispatch_message);
}
