#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>

#include "loggable_os.hpp"

namespace loggable {

/**
 * @brief Thread-safe ring buffer with "drop oldest" overflow policy.
 *
 * When buffer is full, push() overwrites the oldest entry.
 * If a backend is provided, pop() blocks until data is available using
 * semaphore signaling. Without a backend, pop() returns immediately if empty.
 *
 * @tparam T Element type (must be move-constructible)
 * @tparam Capacity Fixed buffer capacity
 */
template <typename T, size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0, "Capacity must be greater than 0");

public:
    /**
     * @brief Construct a ring buffer.
     * @param backend Optional async backend for semaphore operations.
     *                If nullptr, blocking operations are disabled.
     */
    explicit RingBuffer(os::IAsyncBackend* backend = nullptr) noexcept
        : _backend(backend) {
        if (_backend) {
            _sem = _backend->semaphore_create_binary();
        }
    }

    ~RingBuffer() {
        if (_backend && _sem) {
            _backend->semaphore_destroy(_sem);
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /**
     * @brief Push an item, dropping oldest if full.
     * @param item Item to push (moved into buffer)
     * @return true if space was available, false if oldest was dropped
     */
    bool push(T item) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);

        bool dropped = false;
        if (_count == Capacity) {
            // Buffer full - drop oldest by advancing tail
            _tail = (_tail + 1) % Capacity;
            --_count;
            _dropped_count.fetch_add(1, std::memory_order_relaxed);
            dropped = true;
        }

        _buffer[_head] = std::move(item);
        _head = (_head + 1) % Capacity;
        ++_count;

        // Signal waiting consumer
        if (_backend && _sem) {
            _backend->semaphore_give(_sem);
        }

        return !dropped;
    }

    /**
     * @brief Pop an item, blocking until available or timeout.
     * @param timeout_ms Timeout in milliseconds (WAIT_FOREVER for infinite)
     * @return Item if available within timeout, nullopt otherwise
     */
    std::optional<T> pop(uint32_t timeout_ms = os::WAIT_FOREVER) noexcept {
        // Wait for signal that data is available (if backend exists)
        if (_backend && _sem && !_backend->semaphore_take(_sem, timeout_ms)) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(_mutex);

        if (_count == 0) {
            return std::nullopt;
        }

        T item = std::move(_buffer[_tail]);
        _tail = (_tail + 1) % Capacity;
        --_count;

        // If more items remain, re-signal for next pop
        if (_count > 0 && _backend && _sem) {
            _backend->semaphore_give(_sem);
        }

        return item;
    }

    /**
     * @brief Check if buffer is empty.
     */
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _count == 0;
    }

    /**
     * @brief Get current number of items in buffer.
     */
    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _count;
    }

    /**
     * @brief Get total number of dropped messages since creation.
     */
    [[nodiscard]] size_t dropped_count() const noexcept {
        return _dropped_count.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get buffer capacity.
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /**
     * @brief Signal to unblock any waiting pop() calls.
     */
    void signal() noexcept {
        if (_backend && _sem) {
            _backend->semaphore_give(_sem);
        }
    }

private:
    std::array<T, Capacity> _buffer{};
    size_t _head{0};
    size_t _tail{0};
    size_t _count{0};
    mutable std::mutex _mutex;
    std::atomic<size_t> _dropped_count{0};

    os::IAsyncBackend* _backend{nullptr};
    os::SemaphoreHandle _sem{};
};

} // namespace loggable
