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

namespace loggable {

    // Global state for the C-style hook
    static vprintf_like_t original_vprintf = nullptr;
    static std::atomic<bool> esp_log_hook_installed{false};
    
    // Thread-local buffer state to avoid race conditions
    struct ThreadBufferState {
        std::string log_buffer;
        bool buffering_log{false};
    };
    
    static ThreadBufferState& get_thread_buffer() {
        static thread_local ThreadBufferState buffer_state;
        return buffer_state;
    }

    // The C-style vprintf function that will be hooked into ESP-IDF.
    // It is a friend of the Sinker class, allowing it to call the private `dispatch_from_hook` method.
    int vprintf_hook(const char* format, va_list args) {
        // Use a thread-local guard to prevent recursive logging
        thread_local bool is_logging = false;
        if (is_logging) {
            return 0; // Recursive call detected, abort immediately.
        }
        
        is_logging = true;
        // RAII guard to ensure is_logging is reset
        struct LoggingGuard {
            bool& flag;
            ~LoggingGuard() { flag = false; }
        } guard{is_logging};

        // First, pass the log to the original vprintf function to maintain default serial output.
        if (original_vprintf) {
            original_vprintf(format, args);
        }

        // Use a stack buffer for small messages to avoid heap allocation for performance.
        char static_buf[128];
        int size = std::vsnprintf(static_buf, sizeof(static_buf), format, args);

        if (size < 0) {
            return 0; // Formatting error
        }

        std::string formatted_message;
        if (static_cast<size_t>(size) < sizeof(static_buf)) {
            // The message fit into the stack buffer.
            formatted_message = std::string(static_buf);
        } else {
            // The message was too large; allocate a buffer on the heap.
            std::vector<char> dynamic_buf(static_cast<size_t>(size) + 1);
            std::vsnprintf(dynamic_buf.data(), dynamic_buf.size(), format, args);
            formatted_message = std::string(dynamic_buf.data());
        }

        // Check if this is a color encoding start (beginning of a log message)
        bool is_color_start = (formatted_message.length() >= 2 && formatted_message[0] == '\033');
        // Check if this is a color encoding end (end of a log message)
        bool is_color_end = (formatted_message.find("\033[0m") != std::string::npos) ||
                           (formatted_message.length() > 0 && formatted_message.back() == '\n');

        // Get the distributor instance
        auto& distributor = Sinker::instance();
        auto& buffer_state = get_thread_buffer();

        // If we're buffering and this is the end color, finalize the message
        if (buffer_state.buffering_log && is_color_end) {
            // Add this part to the buffer
            buffer_state.log_buffer += formatted_message;
            
            // Process the complete buffered message
            std::string complete_message = std::move(buffer_state.log_buffer);
            buffer_state.log_buffer.clear();
            buffer_state.buffering_log = false;
            
            // Remove color encoding from the complete message
            // Handle multiple color encoding sequences
            while (true) {
                // Start color encoding pattern: \033[0;3xm where x is the color code
                size_t start_pos = complete_message.find("\033[0;");
                if (start_pos == std::string::npos) {
                    break;
                }
                // Find the end of the start color encoding (after the 'm')
                size_t end_pos = complete_message.find('m', start_pos);
                if (end_pos == std::string::npos) {
                    break;
                }
                // Remove the start color encoding
                complete_message.erase(start_pos, end_pos - start_pos + 1);
            }

            // Remove end color encoding
            while (true) {
                size_t reset_pos = complete_message.find("\033[0m");
                if (reset_pos == std::string::npos) {
                    break;
                }
                complete_message.erase(reset_pos, 4); // 4 is the length of "\033[0m"
            }

            // Remove trailing newline if present
            if (!complete_message.empty() && complete_message.back() == '\n') {
                complete_message.pop_back();
            }

            // Process non-empty messages
            if (!complete_message.empty()) {
                distributor.dispatch_from_hook(complete_message);
            }
        }
        // If we're already buffering, add this message to the buffer
        else if (buffer_state.buffering_log) {
            buffer_state.log_buffer += formatted_message;
        }
        // If this is a start color, begin buffering
        else if (is_color_start) {
            buffer_state.log_buffer = formatted_message;
            buffer_state.buffering_log = true;
        }
        // Handle normal single-part messages
        else {
            // Remove color encoding from the message
            // Handle multiple color encoding sequences
            while (true) {
                // Start color encoding pattern: \033[0;3xm where x is the color code
                size_t start_pos = formatted_message.find("\033[0;");
                if (start_pos == std::string::npos) {
                    break;
                }
                // Find the end of the start color encoding (after the 'm')
                size_t end_pos = formatted_message.find('m', start_pos);
                if (end_pos == std::string::npos) {
                    break;
                }
                // Remove the start color encoding
                formatted_message.erase(start_pos, end_pos - start_pos + 1);
            }

            // Remove end color encoding
            while (true) {
                size_t reset_pos = formatted_message.find("\033[0m");
                if (reset_pos == std::string::npos) {
                    break;
                }
                formatted_message.erase(reset_pos, 4); // 4 is the length of "\033[0m"
            }

            // Remove trailing newline if present
            if (!formatted_message.empty() && formatted_message.back() == '\n') {
                formatted_message.pop_back();
            }

            // Process non-empty messages
            if (!formatted_message.empty()) {
                distributor.dispatch_from_hook(formatted_message);
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
            std::lock_guard<std::mutex> lock(_mutex);
            // In embedded systems without exceptions, we assume allocation succeeds
            // or the system handles allocation failures at a higher level
            _sinkers.push_back(std::move(sinker));
        }
    }

    void Sinker::remove_sinker(const std::shared_ptr<ISink>& sinker) noexcept {
        if (sinker) {
            std::lock_guard<std::mutex> lock(_mutex);
            // std::erase doesn't throw in C++17
            std::erase(_sinkers, sinker);
        }
    }

    void Sinker::set_level(LogLevel level) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        _global_level = level;
    }

    LogLevel Sinker::get_level() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _global_level;
    }

    void Sinker::dispatch(const LogMessage& message) noexcept {
        // This is the public dispatch method. We can add more logic here if needed.
        // For now, it directly calls the internal dispatcher.
        std::lock_guard<std::mutex> lock(_mutex);
        // In embedded systems without exceptions, we assume operations succeed
        _dispatch_internal(message);
    }

    void Sinker::hook_esp_log(bool install) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
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
        // Internal method to dispatch messages to sinkers, guarded by the caller.
        if (is_log_level_enabled(message.get_level(), _global_level)) {
            for (const auto& sinker : _sinkers) {
                if (sinker) {
                    // In embedded systems without exceptions, we assume consume() succeeds
                    sinker->consume(message);
                }
            }
        }
    }

    void Sinker::dispatch_from_hook(std::string_view message) {
        // This private method is called by the friendly C-style hook.
        // It's responsible for parsing the raw string and dispatching it safely.
        
        LogLevel level = LogLevel::Info;
        std::string tag;  // Empty by default
        std::string payload;

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
        
        LogMessage log_msg(std::chrono::system_clock::now(), level, std::move(tag), std::move(payload));
        // Dispatch to sinkers under lock
        _dispatch_internal(log_msg);
    }

    // --- Logger Implementation ---

    void Logger::log(LogLevel level, std::string_view message) noexcept {
        if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const char* tag = _owner.log_name();
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

        if (size < 0) {
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