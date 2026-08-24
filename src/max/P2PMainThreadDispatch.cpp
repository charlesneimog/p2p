#include "P2PMainThreadDispatch.hpp"

#include <ext.h>

#include <deque>
#include <mutex>

namespace {
std::mutex queue_mutex;
std::deque<std::function<void()>> queue;
void *queue_qelem = nullptr;

void drain(void *) {
    for (;;) {
        std::function<void()> function;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (queue.empty()) {
                break;
            }
            function = std::move(queue.front());
            queue.pop_front();
        }
        if (function) {
            function();
        }
    }
}
} // namespace

void P2PMainThreadDispatch::initialize() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!queue_qelem) {
        queue_qelem = qelem_new(nullptr, reinterpret_cast<method>(drain));
    }
}

void P2PMainThreadDispatch::enqueue(std::function<void()> function) {
    initialize();
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.push_back(std::move(function));
    }
    qelem_set(queue_qelem);
}
