#pragma once

#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace loggable {

// Forward declarations
class Logger;
class Loggable;
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
  std::chrono::system_clock::time_point _timestamp;
  LogLevel _level;
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
   * CAREFUL: This should not block.
   * @param message The log message to append.
   */
  virtual void consume(const LogMessage &message) = 0;
};

/**
 * @brief The central hub for collecting and dispatching log messages.
 *
 * This class manages a list of sinkers and forwards each log message
 * to all registered sinkers, subject to level filtering.
 * It is implemented as a thread-safe singleton.
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
   * This is intended for internal use by the Logger class.
   * @param message The message to dispatch.
   */
  void dispatch(const LogMessage &message) noexcept;

private:
  Sinker() = default;

  std::atomic<LogLevel> _global_level{LogLevel::Info};
  std::vector<std::shared_ptr<ISink>> _sinkers;
  mutable std::recursive_mutex _mutex;

  /**
   * @brief Internal implementation of the dispatch logic.
   * @param message The message to dispatch.
   */
  void _dispatch_internal(const LogMessage &message) noexcept;
};

/**
 * @brief Provides a logging interface for sending formatted log messages.
 *
 * Each Loggable object contains a Logger. The Logger formats the
 * message and forwards it to the central Sinker.
 */
class Logger {
public:
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;
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
   * @note Use the LOGF macro to auto-prepend function name.
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

  // Private constructor, only Loggable can create it.
  explicit Logger(Loggable &owner) : _owner(owner) {}

  Loggable &_owner;
  friend class Loggable;
};

/**
 * @brief Base class for any object that wishes to generate logs.
 *
 * Inheriting from this class provides a `logger()` method to access a
 * dedicated Logger instance.
 */
class Loggable {
public:
  explicit Loggable(std::string_view name) : _name(name) {}
  Loggable(const Loggable &) = delete;
  Loggable &operator=(const Loggable &) = delete;
  Loggable(Loggable &&) noexcept = delete;
  Loggable &operator=(Loggable &&) noexcept = delete;
  virtual ~Loggable() = default;

  /**
   * @brief Returns a reference to the Logger instance for this object.
   * The Logger is created on first access.
   */
  [[nodiscard]] Logger &logger() noexcept;

private:
  friend class Logger;
  std::string_view _name;
  mutable std::mutex _logger_mutex;
  mutable std::unique_ptr<Logger> _logger;
  mutable std::once_flag _logger_init_flag;
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
#define LOGF(level, format_str, ...)                                           \
  logger().logf(level, "{}: " format_str, __func__ __VA_OPT__(, ) __VA_ARGS__)

/**
 * @brief Macro for logging with automatic function name prefix.
 * @param level The log level (e.g., LogLevel::Info).
 * @param message The message to log.
 *
 * Usage: LOG(LogLevel::Info, "Hello, world!");
 */
#define LOG(level, message) logger().log(level, message)
