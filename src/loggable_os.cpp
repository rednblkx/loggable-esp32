#include "loggable_os.hpp"
#include <atomic>

namespace loggable::os {

namespace {
std::atomic<IAsyncBackend*> g_backend{nullptr};
} // namespace

void set_backend(IAsyncBackend* backend) noexcept {
    g_backend.store(backend, std::memory_order_release);
}

IAsyncBackend* get_backend() noexcept {
    return g_backend.load(std::memory_order_acquire);
}

} // namespace loggable::os

