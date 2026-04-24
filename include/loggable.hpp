#pragma once

#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "loggable_os.hpp"
#include "loggable_ringbuffer.hpp"

namespace loggable {

// Forward declarations
class Sinker;

/**
 * @brief Defines the verbosity level of a log message.
 */
enum class LogLevel : std::uint8_t {
  None,
  Error,
  Warning,
  Info,
  Debug,
  Verbose
};

// Constexpr functions for LogLevel operations
[[nodiscard]] constexpr const char *
log_level_to_string(LogLevel level) noexcept {
  switch (level) {
  case LogLevel::Error:
    return "E";
  case LogLevel::Warning:
    return "W";
  case LogLevel::Info:
    return "I";
  case LogLevel::Debug:
    return "D";
  case LogLevel::Verbose:
    return "V";
  case LogLevel::None:
    return "N";
  }
  // All enum values handled above; this is unreachable
  static_assert(static_cast<int>(LogLevel::Verbose) == 5,
                "LogLevel enum changed, update switch");
  return "UNKNOWN"; // Satisfy compiler return requirement
}

[[nodiscard]] constexpr bool
is_log_level_enabled(LogLevel message_level, LogLevel global_level) noexcept {
  return message_level <= global_level;
}

/**
 * @brief A structure representing a single log entry.
 *
 * Owns its data by using std::string, ensuring lifetime correctness.
 */
class LogMessage {
public:
  LogMessage() noexcept = default;

  LogMessage(std::chrono::system_clock::time_point timestamp, LogLevel level,
             std::string tag, std::string message) noexcept
      : _timestamp(timestamp), _level(level), _tag(std::move(tag)),
        _message(std::move(message)) {}

  LogMessage(const LogMessage &) = default;
  LogMessage &operator=(const LogMessage &) = default;
  LogMessage(LogMessage &&) noexcept = default;
  LogMessage &operator=(LogMessage &&) noexcept = default;
  ~LogMessage() = default;

  [[nodiscard]] std::chrono::system_clock::time_point
  get_timestamp() const noexcept {
    return _timestamp;
  }
  [[nodiscard]] LogLevel get_level() const noexcept { return _level; }
  [[nodiscard]] const std::string &get_tag() const noexcept { return _tag; }
  [[nodiscard]] const std::string &get_message() const noexcept {
    return _message;
  }

private:
  std::chrono::system_clock::time_point _timestamp{};
  LogLevel _level{LogLevel::None};
  std::string _tag;
  std::string _message;
};

/**
 * @brief Abstract interface for a log message sink.
 *
 * Any class that wants to receive log messages must implement this interface
 * and register itself with the central Sinker.
 */
class ISink {
public:
  virtual ~ISink() = default;

  /**
   * @brief Processes and outputs a log message.
   * @param message The log message to append.
   */
  virtual void consume(const LogMessage &message) = 0;
};

/**
 * @brief Metrics for monitoring the async logging system.
 */
struct SinkerMetrics {
  size_t dropped_count{0}; ///< Messages dropped due to full queue
  size_t queued_count{0};  ///< Messages currently in queue
  size_t capacity{0};      ///< Queue capacity
  bool is_running{false};  ///< Whether async dispatch is active
};

/**
 * @brief Configuration for the async dispatch system.
 */
struct SinkerConfig {
  size_t task_stack_size = 4096;
  int task_priority = 5;
  int task_core = -1; ///< -1 = any core
};

/**
 * @brief The central hub for collecting and dispatching log messages.
 *
 * This class manages a list of sinkers and forwards each log message
 * to all registered sinkers, subject to level filtering.
 * It is implemented as a thread-safe singleton.
 *
 * Supports both synchronous and asynchronous dispatch modes:
 * - Synchronous (default): Messages are dispatched immediately to sinks
 * - Asynchronous: Messages are queued and dispatched by a worker task
 *
 * Call init() to enable async mode. If init() is not called or fails,
 * dispatch falls back to synchronous behavior.
 */
class Sinker {
public:
  Sinker(const Sinker &) = delete;
  Sinker &operator=(const Sinker &) = delete;
  ~Sinker() noexcept = default;

  /**
   * @brief Returns the singleton instance of the Sinker.
   */
  [[nodiscard]] static Sinker &instance() noexcept;

  /**
   * @brief Registers a new log sinker.
   *
   * The Sinker does not take ownership of the sinker. The caller
   * is responsible for managing the sinker's lifecycle.
   *
   * @param sinker A shared pointer to an ISinker implementation.
   */
  void add_sinker(std::shared_ptr<ISink> sinker) noexcept;

  /**
   * @brief Unregisters a log sinker.
   * @param sinker The sinker to remove.
   */
  void remove_sinker(const std::shared_ptr<ISink> &sinker) noexcept;

  /**
   * @brief Sets the global minimum log level.
   * Messages with a lower severity will be discarded.
   * @param level The minimum level to allow.
   */
  void set_level(LogLevel level) noexcept;

  /**
   * @brief Gets the current global minimum log level.
   */
  [[nodiscard]] LogLevel get_level() const noexcept;

  /**
   * @brief Forwards a log message to all registered sinkers.
   *
   * If async mode is active (init() was called), messages are queued
   * for the worker task. Otherwise, dispatch is synchronous.
   *
   * @param message The message to dispatch.
   */
  void dispatch(const LogMessage &message) noexcept;

  // --- Async API ---

  /**
   * @brief Initialize the async dispatch system.
   *
   * Must be called before logging if async behavior is desired.
   * If not called (or on platforms without OS support), dispatch()
   * uses synchronous behavior.
   *
   * @param config Optional configuration for the worker task.
   */
  void init(const SinkerConfig &config = {}) noexcept;

  /**
   * @brief Shutdown the async dispatch system.
   *
   * Flushes all queued messages to sinks before stopping.
   */
  void shutdown() noexcept;

  /**
   * @brief Flush all queued messages synchronously.
   *
   * Blocks until the queue is empty or timeout expires.
   *
   * @param timeout_ms Maximum time to wait in milliseconds.
   * @return true if queue is empty, false if timeout expired.
   */
  [[nodiscard]] bool flush(uint32_t timeout_ms = 5000) noexcept;

  /**
   * @brief Check if async dispatch is running.
   */
  [[nodiscard]] bool is_running() const noexcept;

  /**
   * @brief Get current metrics for monitoring.
   */
  [[nodiscard]] SinkerMetrics get_metrics() const noexcept;

private:
  Sinker() = default;

  std::atomic<LogLevel> _global_level{LogLevel::Info};
  std::vector<std::shared_ptr<ISink>> _sinkers;
  mutable std::mutex _sinkers_mutex;

  // Async infrastructure
  static constexpr size_t QUEUE_CAPACITY = 64;
  std::unique_ptr<RingBuffer<LogMessage, QUEUE_CAPACITY>> _queue;
  std::atomic<bool> _running{false};
  std::atomic<bool> _shutdown_requested{false};

  os::TaskHandle _task{};
  static void _task_entry(void *arg) noexcept;
  void _process_queue() noexcept;

  /**
   * @brief Internal implementation of the dispatch logic.
   * @param message The message to dispatch.
   */
  void _dispatch_internal(const LogMessage &message) noexcept;
};

/**
 * @brief Lightweight logger that formats and dispatches log messages.
 *
 * Logger is a simple value type holding only a tag string. It can be
 * used standalone or as a member of Loggable-derived classes.
 */
class Logger {
public:
  explicit Logger(std::string_view tag) noexcept : _tag(tag) {}

  Logger(const Logger &) = default;
  Logger &operator=(const Logger &) = default;
  Logger(Logger &&) noexcept = default;
  Logger &operator=(Logger &&) noexcept = default;
  ~Logger() noexcept = default;

  /**
   * @brief Logs a pre-formatted message.
   * @param level The message's severity level.
   * @param message The message content.
   */
  void log(LogLevel level, std::string_view message) noexcept;

  /**
   * @brief Logs a fmt-style formatted message.
   * @param level The message's severity level.
   * @param format_str The fmt-style format string.
   * @param args Arguments for the format string.
   * @note Use the LOG macro to auto-prepend function name.
   */
  template <typename... Args>
  void logf(LogLevel level, fmt::format_string<Args...> format_str,
            Args &&...args) noexcept {
    if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
      return;
    }
    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), format_str,
                   std::forward<Args>(args)...);
    log(level, std::string_view(buf.data(), buf.size()));
  }

private:
  std::string_view _tag;
};

/**
 * @brief Base class for any object that wishes to generate logs.
 *
 * Inheriting from this class provides a `logger()` method to access a
 * dedicated Logger instance.
 */
class Loggable {
public:
  explicit Loggable(std::string_view name) noexcept : _logger(name) {}
  Loggable(const Loggable &) = delete;
  Loggable &operator=(const Loggable &) = delete;
  Loggable(Loggable &&) noexcept = delete;
  Loggable &operator=(Loggable &&) noexcept = delete;
  virtual ~Loggable() = default;

  /**
   * @brief Returns a reference to the Logger instance for this object.
   */
  [[nodiscard]] Logger &logger() noexcept { return _logger; }

private:
  Logger _logger;
};

} // namespace loggable

/**
 * @brief Macro for logging with automatic function name prefix.
 * @param level The log level (e.g., LogLevel::Info).
 * @param format_str The fmt-style format string.
 * @param ... Arguments for the format string.
 *
 * Usage: LOGF(LogLevel::Info, "Hello, {}!", name);
 */
#if __cplusplus >= 202002L
#define LOG(level, format_str, ...)                                            \
  logger().logf(level, "{}: " format_str, __func__ __VA_OPT__(, ) __VA_ARGS__)
#else
// C++17 fallback: use ## to swallow the comma when __VA_ARGS__ is empty
#define LOG(level, format_str, ...)                                            \
  logger().logf(level, "{}: " format_str, __func__, ##__VA_ARGS__)
#endif
