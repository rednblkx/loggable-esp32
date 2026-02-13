#include "loggable.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>

#include "fmt/color.h"
#include "loggable_os.hpp"

namespace loggable {

// --- Sinker Implementation ---

Sinker &Sinker::instance() noexcept {
    static Sinker inst;
    return inst;
}

void Sinker::add_sinker(std::shared_ptr<ISink> sinker) noexcept {
    if (sinker) {
        std::lock_guard<std::mutex> lock(_sinkers_mutex);
        _sinkers.push_back(std::move(sinker));
    }
}

void Sinker::remove_sinker(const std::shared_ptr<ISink> &sinker) noexcept {
    if (sinker) {
        std::lock_guard<std::mutex> lock(_sinkers_mutex);
#if __cplusplus >= 202002L
        std::erase(_sinkers, sinker);
#else
        _sinkers.erase(std::remove(_sinkers.begin(), _sinkers.end(), sinker), _sinkers.end());
#endif

    }
}

void Sinker::set_level(LogLevel level) noexcept {
    _global_level.store(level, std::memory_order_release);
}

LogLevel Sinker::get_level() const noexcept {
    return _global_level.load(std::memory_order_acquire);
}

void Sinker::dispatch(const LogMessage &message) noexcept {
    if (_running.load(std::memory_order_acquire) && _queue) {
        // Async path: enqueue (drops oldest if full)
        _queue->push(message);
    } else {
        // Sync fallback
        std::lock_guard<std::mutex> lock(_sinkers_mutex);
        _dispatch_internal(message);
    }
}

void Sinker::_dispatch_internal(const LogMessage &message) noexcept {
    for (const auto &sinker : _sinkers) {
        if (sinker) [[likely]] {
            sinker->consume(message);
        }
    }
}

void Sinker::init(const SinkerConfig &config) noexcept {
    auto *backend = os::get_backend();
    if (!backend) {
        // No backend registered - stay in sync mode
        return;
    }

    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return; // Already running
    }

    _shutdown_requested.store(false, std::memory_order_release);
    _queue = std::make_unique<RingBuffer<LogMessage, QUEUE_CAPACITY>>(backend);

    os::TaskConfig task_cfg{
        .name = "log_dispatch",
        .stack_size = config.task_stack_size,
        .priority = config.task_priority,
        .core = config.task_core};

    _task = backend->task_create(task_cfg, &Sinker::_task_entry, this);

    if (!_task) {
        _queue.reset();
        _running.store(false, std::memory_order_release);
    }
}

void Sinker::shutdown() noexcept {
    auto *backend = os::get_backend();
    if (!backend || !_running.load(std::memory_order_acquire)) {
        return;
    }

    _shutdown_requested.store(true, std::memory_order_release);

    if (_queue) {
        _queue->signal();
    }

    (void)flush(5000);

    _running.store(false, std::memory_order_release);

    if (_queue) {
        _queue->signal();
    }

    backend->delay_ms(100);

    _queue.reset();
    _task = os::TaskHandle{};
}

bool Sinker::flush(uint32_t timeout_ms) noexcept {
    auto *backend = os::get_backend();
    if (!_queue) {
        return true;
    }

    constexpr uint32_t poll_interval = 10;
    uint32_t elapsed = 0;

    while (!_queue->empty()) {
        if (timeout_ms > 0 && elapsed >= timeout_ms) {
            return false;
        }
        if (backend) {
            backend->delay_ms(poll_interval);
        }
        elapsed += poll_interval;
    }

    return true;
}

bool Sinker::is_running() const noexcept {
    return _running.load(std::memory_order_acquire);
}

SinkerMetrics Sinker::get_metrics() const noexcept {
    return SinkerMetrics{
        .dropped_count = _queue ? _queue->dropped_count() : 0,
        .queued_count = _queue ? _queue->size() : 0,
        .capacity = QUEUE_CAPACITY,
        .is_running = _running.load(std::memory_order_acquire)};
}

void Sinker::_task_entry(void *arg) noexcept {
    auto *self = static_cast<Sinker *>(arg);
    self->_process_queue();

    auto *backend = os::get_backend();
    if (backend) {
        backend->task_delete(os::TaskHandle{});
    }
}

void Sinker::_process_queue() noexcept {
    while (_running.load(std::memory_order_acquire)) {
        auto msg = _queue->pop(100); // 100ms timeout for shutdown check

        if (msg) {
            std::lock_guard<std::mutex> lock(_sinkers_mutex);
            _dispatch_internal(*msg);
        }

        auto metrics = get_metrics();
        if (metrics.dropped_count > 0) {
            fmt::print(fg(fmt::color::orange), "[{}][W][{}][{}:{}] Dropped {} log messages\n", os::get_backend()->get_time_ms(), "Loggable::Sinker", __func__, __LINE__, metrics.dropped_count);
        }

        if (_shutdown_requested.load(std::memory_order_acquire) &&
            _queue->empty()) {
            break;
        }
    }

    // Drain remaining on shutdown
    while (auto msg = _queue->pop(0)) {
        std::lock_guard<std::mutex> lock(_sinkers_mutex);
        _dispatch_internal(*msg);
    }
}

// --- Logger Implementation ---

void Logger::log(LogLevel level, std::string_view message) noexcept {
    if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
        return;
    }
    auto now = std::chrono::system_clock::now();
    auto *backend = os::get_backend();
    if (backend) {
        now = std::chrono::system_clock::time_point(std::chrono::milliseconds(backend->get_time_ms()));
    }
    LogMessage msg(now, level, std::string(_tag), std::string(message));
    Sinker::instance().dispatch(msg);
}

} // namespace loggable
