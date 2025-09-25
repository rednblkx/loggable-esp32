#pragma once

#include <memory>
#include <string_view>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdarg>

namespace loggable {

    // Forward declaration for the C-style hook function
    int vprintf_hook(const char* format, va_list args);

    // Forward declarations
    class Logger;
    class Loggable;
    class Sinker;

    /**
     * @brief Defines the verbosity level of a log message.
     */
    enum class LogLevel {
        None,
        Error,
        Warning,
        Info,
        Debug,
        Verbose
    };

    /**
     * @brief A structure representing a single log entry.
     *
     * Owns its data by using std::string, ensuring lifetime correctness.
     */
    class LogMessage {
    public:
        LogMessage(std::chrono::system_clock::time_point timestamp, LogLevel level, std::string tag, std::string message)
            : _timestamp(timestamp), _level(level), _tag(std::move(tag)), _message(std::move(message)) {}

        LogMessage(const LogMessage&) = default;
        LogMessage& operator=(const LogMessage&) = default;

        [[nodiscard]] std::chrono::system_clock::time_point get_timestamp() const noexcept { return _timestamp; }
        [[nodiscard]] LogLevel get_level() const noexcept { return _level; }
        [[nodiscard]] const std::string& get_tag() const noexcept { return _tag; }
        [[nodiscard]] const std::string& get_message() const noexcept { return _message; }

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
         * @param message The log message to append.
         */
        virtual void consume(const LogMessage& message) = 0;
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
        Sinker(const Sinker&) = delete;
        Sinker& operator=(const Sinker&) = delete;

        /**
         * @brief Returns the singleton instance of the Sinker.
         */
        static Sinker& instance();

        /**
         * @brief Registers a new log sinker.
         *
         * The Sinker does not take ownership of the sinker. The caller
         * is responsible for managing the sinker's lifecycle.
         *
         * @param sinker A shared pointer to an ISinker implementation.
         */
        void add_sinker(std::shared_ptr<ISink> sinker);
        
        /**
         * @brief Unregisters a log sinker.
         * @param sinker The sinker to remove.
         */
        void remove_sinker(const std::shared_ptr<ISink>& sinker);

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
        void dispatch(const LogMessage& message);

        /**
         * @brief Hooks into the ESP-IDF logging framework.
         *
         * When installed, all logs made via `ESP_LOGx` macros will be redirected
         * through this distributor.
         *
         * @param install Set to true to install the hook, false to uninstall.
         */
        void hook_esp_log(bool install);

    private:
        friend int vprintf_hook(const char* format, va_list args);

        Sinker() = default;

        /**
         * @brief Internal, thread-safe method to process a raw string from a C-style hook.
         * @param message The raw, formatted log message.
         */
        void dispatch_from_hook(std::string_view message);

        LogLevel _global_level{LogLevel::Info};
        std::vector<std::shared_ptr<ISink>> _sinkers;
        mutable std::mutex _mutex;
        
        // Buffer for multi-part log messages
        std::string _log_buffer;
        bool _buffering_log{false};

        /**
         * @brief Internal implementation of the dispatch logic.
         * @param message The message to dispatch.
         */
        void _dispatch_internal(const LogMessage& message);
    };

    /**
     * @brief Provides a logging interface for sending formatted log messages.
     *
     * Each Loggable object contains a Logger. The Logger formats the
     * message and forwards it to the central Sinker.
     */
    class Logger {
    public:
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        /**
         * @brief Logs a pre-formatted message.
         * @param level The message's severity level.
         * @param message The message content.
         */
        void log(LogLevel level, std::string_view message);

        /**
         * @brief Logs a printf-style formatted message.
         * @param level The message's severity level.
         * @param format The printf-style format string.
         * @param ... Arguments for the format string.
         */
        void logf(LogLevel level, const char* format, ...) __attribute__((format(printf, 3, 4)));
        
        /**
         * @brief Logs a printf-style formatted message using va_list.
         * @param level The message's severity level.
         * @param format The printf-style format string.
         * @param args The va_list of arguments.
         */
        void vlogf(LogLevel level, const char* format, va_list args);

    private:
        // Private constructor, only Loggable can create it.
        explicit Logger(Loggable& owner) : _owner(owner) {}

        Loggable& _owner;
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
        /**
         * @brief Returns a reference to the Logger instance for this object.
         * The Logger is created on first access.
         */
        Logger& logger();

        virtual ~Loggable() = default;

    protected:
        /**
         * @brief Implement this to provide a name for the log source.
         * @return A C-style string representing the component's name.
         */
        [[nodiscard]] virtual const char* log_name() const noexcept = 0;
        friend class Logger;

    private:
        std::unique_ptr<Logger> _logger;
    };

} // namespace loggable