#pragma once
#include <cstddef>
#include <cstdint>

namespace loggable {
namespace os {

/**
 * @brief Opaque handle for a binary semaphore.
 */
struct SemaphoreHandle {
    void* _handle{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return _handle != nullptr;
    }

    [[nodiscard]] bool operator==(const SemaphoreHandle&) const noexcept = default;
};

/**
 * @brief Opaque handle for a task.
 */
struct TaskHandle {
    void* _handle{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return _handle != nullptr;
    }

    [[nodiscard]] bool operator==(const TaskHandle&) const noexcept = default;
};

/**
 * @brief Configuration for task creation.
 */
struct TaskConfig {
    const char* name = "log_dispatch";
    size_t stack_size = 4096;
    int priority = 5;
    int core = -1; ///< -1 = any core
};

/// Task entry function signature
using TaskFunction = void (*)(void* arg);

/// Infinite wait timeout
constexpr uint32_t WAIT_FOREVER = 0xFFFFFFFF;

/**
 * @brief Abstract interface for OS operations required by async logging.
 *
 * Platform-specific implementations (e.g., FreeRTOS) inherit from this
 * and register themselves via set_backend() before Sinker::init().
 *
 * If no backend is registered, async logging is disabled and Sinker
 * operates in synchronous mode.
 */
class IAsyncBackend {
public:
    virtual ~IAsyncBackend() = default;

    // --- Semaphore operations ---

    /**
     * @brief Create a binary semaphore.
     * @return Handle to the semaphore, or invalid handle on failure.
     */
    [[nodiscard]] virtual SemaphoreHandle semaphore_create_binary() noexcept = 0;

    /**
     * @brief Destroy a semaphore.
     * @param sem Handle to destroy.
     */
    virtual void semaphore_destroy(SemaphoreHandle sem) noexcept = 0;

    /**
     * @brief Give (signal) a semaphore.
     * @param sem Handle to signal.
     */
    virtual void semaphore_give(SemaphoreHandle sem) noexcept = 0;

    /**
     * @brief Take (wait on) a semaphore.
     * @param sem Handle to wait on.
     * @param timeout_ms Timeout in milliseconds, or WAIT_FOREVER.
     * @return true if semaphore was taken, false on timeout.
     */
    [[nodiscard]] virtual bool semaphore_take(SemaphoreHandle sem, uint32_t timeout_ms) noexcept = 0;

    // --- Task operations ---

    /**
     * @brief Create a task.
     * @param config Task configuration.
     * @param fn Task entry function.
     * @param arg Argument passed to task function.
     * @return Handle to the task, or invalid handle on failure.
     */
    [[nodiscard]] virtual TaskHandle task_create(const TaskConfig& config,
                                                 TaskFunction fn,
                                                 void* arg) noexcept = 0;

    /**
     * @brief Delete a task.
     * @param task Handle to delete. Pass invalid handle to delete current task.
     */
    virtual void task_delete(TaskHandle task) noexcept = 0;

    // --- Timing ---

    /**
     * @brief Delay the current task.
     * @param ms Delay in milliseconds.
     */
    virtual void delay_ms(uint32_t ms) noexcept = 0;

    /**
     * @brief Get the current time in milliseconds.
     * @return Current time in milliseconds.
     */
    virtual uint32_t get_time_ms() noexcept = 0;
};

/**
 * @brief Set the async backend implementation.
 *
 * Must be called before Sinker::init() to enable async dispatch.
 * Thread-safe (uses atomic pointer).
 *
 * @param backend Pointer to backend implementation. Pass nullptr to disable.
 */
void set_backend(IAsyncBackend* backend) noexcept;

/**
 * @brief Get the current async backend.
 * @return Pointer to backend, or nullptr if none set.
 */
[[nodiscard]] IAsyncBackend* get_backend() noexcept;

} // namespace os
} // namespace loggable
