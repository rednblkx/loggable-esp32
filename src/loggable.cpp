#include "loggable.hpp"
#include <esp_log.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <memory>
#include <string_view>
#include <utility>
#include <charconv>

namespace loggable {

    // Global state for the C-style hook
    static vprintf_like_t original_vprintf = nullptr;
    static std::atomic<bool> esp_log_hook_installed{false};
    
    // Thread-local buffer state to avoid race conditions
    struct ThreadBufferState {
        std::string log_buffer;
    };
    
    static ThreadBufferState& get_thread_buffer() {
        static thread_local ThreadBufferState buffer_state;
        return buffer_state;
    }

    namespace {
    void cleanup_message(std::string& message) {
        // Check if any ANSI escape codes are likely present before attempting removal.
        if (message.find("\033[") != std::string::npos) {
            size_t start_pos = 0;
            while ((start_pos = message.find("\033[", start_pos)) != std::string::npos) {
                size_t end_pos = message.find('m', start_pos);
                if (end_pos == std::string::npos) {
                    break; // Malformed sequence
                }
                message.erase(start_pos, end_pos - start_pos + 1);
            }
        }

        if (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
    }
    } // namespace

    // The C-style vprintf function that will be hooked into ESP-IDF.
    int vprintf_hook(const char* format, va_list args) {
        // Use a thread-local guard to prevent recursive logging
        thread_local static bool is_logging = false;
        if (is_logging) {
            return 0;
        }
        is_logging = true;
        struct LoggingGuard {
            bool& flag;
            ~LoggingGuard() { flag = false; }
        } guard{is_logging};

        if (original_vprintf) {
            va_list args_copy;
            va_copy(args_copy, args);
            original_vprintf(format, args_copy);
            va_end(args_copy);
        }

        char static_buf[128];
        va_list args_copy;
        va_copy(args_copy, args);
        int size = std::vsnprintf(static_buf, sizeof(static_buf), format, args_copy);
        va_end(args_copy);

        if (size < 0) [[unlikely]] {
            return 0;
        }
        
        std::string_view formatted_message_view(static_buf, size);
        
        std::string dynamic_message;
        if (static_cast<size_t>(size) >= sizeof(static_buf)) {
            dynamic_message.resize(size);
            va_copy(args_copy, args);
            std::vsnprintf(dynamic_message.data(), dynamic_message.size() + 1, format, args_copy);
            va_end(args_copy);
            formatted_message_view = dynamic_message;
        }
        
        auto& buffer_state = get_thread_buffer();

        buffer_state.log_buffer.append(formatted_message_view);

        // Process the buffer only when a complete line is detected (ends with a newline).
        if (!buffer_state.log_buffer.empty() && buffer_state.log_buffer.back() == '\n') {
            std::string complete_message = std::move(buffer_state.log_buffer);
            
            buffer_state.log_buffer.clear();
            
            cleanup_message(complete_message);
            if (!complete_message.empty()) {
                Sinker::instance().dispatch_from_hook(complete_message);
            }
        }
        return size;
    }

    // --- Sinker Implementation ---

    Sinker& Sinker::instance() noexcept {
        static Sinker inst;
        return inst;
    }
    
    Sinker::~Sinker() noexcept {
        // Ensure clean shutdown by removing the hook if still installed
        if (esp_log_hook_installed.load(std::memory_order_acquire)) {
            hook_esp_log(false);
        }
    }

    void Sinker::add_sinker(std::shared_ptr<ISink> sinker) noexcept {
        if (sinker) {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            // In embedded systems without exceptions, we assume allocation succeeds
            // or the system handles allocation failures at a higher level
            _sinkers.push_back(std::move(sinker));
        }
    }

    void Sinker::remove_sinker(const std::shared_ptr<ISink>& sinker) noexcept {
        if (sinker) {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            std::erase(_sinkers, sinker);
        }
    }

    void Sinker::set_level(LogLevel level) noexcept {
        _global_level.store(level, std::memory_order_release);
    }

    LogLevel Sinker::get_level() const noexcept {
        return _global_level.load(std::memory_order_acquire);
    }

    void Sinker::dispatch(const LogMessage& message) noexcept {
        // This is the public dispatch method. We can add more logic here if needed.
        // For now, it directly calls the internal dispatcher.
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        // In embedded systems without exceptions, we assume operations succeed
        _dispatch_internal(message);
    }

    void Sinker::hook_esp_log(bool install) noexcept {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (install && !esp_log_hook_installed.load(std::memory_order_acquire)) {
            original_vprintf = esp_log_set_vprintf(&vprintf_hook);
            esp_log_hook_installed.store(true, std::memory_order_release);
        } else if (!install && esp_log_hook_installed.load(std::memory_order_acquire)) {
            esp_log_set_vprintf(original_vprintf);
            original_vprintf = nullptr;
            esp_log_hook_installed.store(false, std::memory_order_release);
        }
    }
 
    void Sinker::_dispatch_internal(const LogMessage& message) noexcept {
        // Internal method to dispatch messages to sinkers.
        // Level filtering is done by caller; this method just dispatches.
        for (const auto& sinker : _sinkers) {
            if (sinker) [[likely]] {
                sinker->consume(message);
            }
        }
    }

    void Sinker::dispatch_from_hook(std::string_view message) {
        // This private method is called by the friendly C-style hook.
        // It's responsible for parsing the raw string and dispatching it safely.
        
        LogLevel level = LogLevel::Info;
        std::string tag;  // Empty by default
        std::string payload;
        std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();

        // A typical ESP-IDF log looks like: "L (TIME) TAG: MESSAGE"
        // We only parse if it looks like it has the right structure.
        if (message.length() > 4 && message[1] == ' ' && (message[0] == 'E' || message[0] == 'W' || message[0] == 'I' || message[0] == 'D' || message[0] == 'V')) {
            switch(message[0]) {
                case 'E': level = LogLevel::Error; break;
                case 'W': level = LogLevel::Warning; break;
                case 'I': level = LogLevel::Info; break;
                case 'D': level = LogLevel::Debug; break;
                case 'V': level = LogLevel::Verbose; break;
            }

            const size_t tag_start = message.find('(');
            const size_t tag_end = message.find(')', tag_start);
            const size_t payload_start = message.find(':', tag_end);

            if (tag_end != std::string_view::npos && payload_start != std::string_view::npos && tag_end + 1 < message.length()) {
                // Extract timestamp (milliseconds since boot) from between '(' and ')'
                if (tag_start != std::string_view::npos && tag_end > tag_start + 1) {
                    const size_t timestamp_start = tag_start + 1;
                    const size_t timestamp_length = tag_end - timestamp_start;
                    std::string_view timestamp_str = message.substr(timestamp_start, timestamp_length);
                    
                    // Parse the timestamp (milliseconds since boot)
                    unsigned long millis_since_boot = 0;
                    auto [ptr, ec] = std::from_chars(timestamp_str.data(), timestamp_str.data() + timestamp_str.size(), millis_since_boot);
                    
                    if (ec == std::errc()) {
                        // Use the raw milliseconds value as the timestamp
                        timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(millis_since_boot));
                    }
                }
                
                // Check if there's a space after the closing parenthesis
                if (message[tag_end + 1] == ' ') {
                    // Extract tag between ') ' and ':'
                    const size_t tag_text_start = tag_end + 2;
                    if (tag_text_start < payload_start) {
                        tag = std::string(message.substr(tag_text_start, payload_start - tag_text_start));
                    }
                }

                // Extract payload after ': '
                const size_t payload_text_start = payload_start + 2;
                if (payload_text_start < message.length()) {
                    payload = std::string(message.substr(payload_text_start));
                } else {
                    // This handles the case where the log is "TAG:" with no message.
                    payload = "";
                }
            } else {
                // If we can't parse the tag and payload, use the whole message as payload
                payload = std::string(message);
            }
        } else {
            // If the message doesn't match the expected format, use the whole message as payload
            payload = std::string(message);
        }
        
        LogMessage log_msg(timestamp, level, std::move(tag), std::move(payload));
        // Dispatch to sinkers under lock
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _dispatch_internal(log_msg);
    }

    // --- Logger Implementation ---

    void Logger::log(LogLevel level, std::string_view message) noexcept {
        if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        std::string_view tag = _owner.log_name();
        LogMessage msg(now, level, std::string(tag), std::string(message));
        Sinker::instance().dispatch(msg);
    }
    
    void Logger::logf(LogLevel level, const char* format, ...) noexcept {
        if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
            return;
        }
        va_list args;
        va_start(args, format);
        vlogf(level, format, args);
        va_end(args);
    }

    void Logger::vlogf(LogLevel level, const char* format, va_list args) noexcept {
        if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
            return;
        }
        va_list args_copy;
        va_copy(args_copy, args);
        int size = std::vsnprintf(nullptr, 0, format, args_copy);
        va_end(args_copy);

        if (size < 0) [[unlikely]] {
            return; // Encoding error
        }

        std::vector<char> buf(static_cast<size_t>(size) + 1);
        std::vsnprintf(buf.data(), buf.size(), format, args);
        log(level, std::string_view(buf.data(), static_cast<size_t>(size)));
    }

    // --- Loggable Implementation ---

    Logger& Loggable::logger() noexcept {
        std::call_once(_logger_init_flag, [this]() {
            _logger = std::unique_ptr<Logger>(new Logger(*this));
        });
        return *_logger;
    }

} // namespace loggable
