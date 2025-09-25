# ESP-IDF Logging Component

A modern, C++ logging framework for ESP-IDF that provides type-safe, thread-safe logging with multiple sink support and ESP-IDF integration.

## Features

- **Modern C++ Design**: Uses C++17 features, RAII, and smart pointers for memory safety
- **Type-Safe Logging**: Strongly typed log levels and message handling
- **Multiple Sink Support**: Register multiple log sinks (console, file, network, custom)
- **ESP-IDF Integration**: Seamlessly hooks into ESP-IDF's built-in logging system
- **Thread-Safe**: Full thread safety using C++ standard library synchronization primitives
- **Memory Efficient**: Smart memory management with automatic cleanup
- **Namespace Organization**: Clean API organized under `loggable` namespace
- **Color Code Handling**: Automatically strips ANSI color codes from ESP-IDF logs
- **Multi-part Message Support**: Handles complex log messages that span multiple parts

## Architecture

The component follows a clean architecture with three main classes:

1. **Loggable**: Base class for objects that want to generate logs
2. **Logger**: Provides the actual logging interface with formatting capabilities
3. **Sinker**: Central hub that manages log sinks and dispatches messages

## API Reference

### Core Classes

```cpp
namespace loggable {

    enum class LogLevel {
        None,
        Error,
        Warning,
        Info,
        Debug,
        Verbose
    };

    class LogMessage {
    public:
        std::chrono::system_clock::time_point get_timestamp() const noexcept;
        LogLevel get_level() const noexcept;
        const std::string& get_tag() const noexcept;
        const std::string& get_message() const noexcept;
    };

    class ISink {
    public:
        virtual ~ISink() = default;
        virtual void consume(const LogMessage& message) = 0;
    };

    class Sinker {
    public:
        static Sinker& instance();
        void add_sinker(std::shared_ptr<ISink> sinker);
        void remove_sinker(const std::shared_ptr<ISink>& sinker);
        void set_level(LogLevel level) noexcept;
        LogLevel get_level() const noexcept;
        void hook_esp_log(bool install);
    };

    class Logger {
    public:
        void log(LogLevel level, std::string_view message);
        void logf(LogLevel level, const char* format, ...) __attribute__((format(printf, 3, 4)));
        void vlogf(LogLevel level, const char* format, va_list args);
    };

    class Loggable {
    public:
        Logger& logger();
    protected:
        virtual const char* log_name() const noexcept = 0;
    };

} // namespace loggable
```

### Core Functions

#### Sinker Management

- `Sinker::instance()` - Get the singleton instance
- `add_sinker(std::shared_ptr<ISink> sinker)` - Register a log sink
- `remove_sinker(const std::shared_ptr<ISink>& sinker)` - Unregister a log sink
- `set_level(LogLevel level)` - Set global minimum log level
- `get_level()` - Get current global log level
- `hook_esp_log(bool install)` - Enable/disable ESP-IDF log hooking

#### Logging Functions

- `Logger::log(LogLevel level, std::string_view message)` - Log a pre-formatted message
- `Logger::logf(LogLevel level, const char* format, ...)` - Log with printf-style formatting
- `Logger::vlogf(LogLevel level, const char* format, va_list args)` - Log with va_list formatting

## Usage Example

```cpp
#include "loggable.hpp"
using namespace loggable;

// Example sink implementation
class ConsoleSink : public ISink {
public:
    void consume(const LogMessage& msg) override {
        auto level_str = "UNKNOWN";
        switch (msg.get_level()) {
            case LogLevel::Error:   level_str = "ERROR";   break;
            case LogLevel::Warning: level_str = "WARNING"; break;
            case LogLevel::Info:    level_str = "INFO";    break;
            case LogLevel::Debug:   level_str = "DEBUG";   break;
            case LogLevel::Verbose: level_str = "VERBOSE"; break;
            default: break;
        }
        
        auto time_t = std::chrono::system_clock::to_time_t(msg.get_timestamp());
        std::cout << "[" << time_t << "] "
                  << "[" << level_str << "] "
                  << "[" << msg.get_tag() << "] "
                  << msg.get_message() << std::endl;
    }
};

// Example loggable component
class MyComponent : public Loggable {
public:
    void do_work() {
        logger().logf(LogLevel::Info, "Starting work...");
        
        for (int i = 0; i < 3; ++i) {
            logger().logf(LogLevel::Debug, "Processing item #%d", i + 1);
        }
        
        logger().log(LogLevel::Warning, "Work completed with warnings.");
    }

protected:
    const char* log_name() const noexcept override {
        return "MyComponent";
    }
};

extern "C" void app_main(void) {
    // Get the distributor instance
    auto& sinker = Sinker::instance();
    
    // Set the global log level
    sinker.set_level(LogLevel::Debug);
    
    // Create and register a sink
    auto console_sink = std::make_shared<ConsoleSink>();
    sinker.add_sinker(console_sink);
    
    // Create and use a loggable component
    MyComponent component;
    component.do_work();
    
    // Hook into ESP-IDF logging
    sinker.hook_esp_log(true);
    ESP_LOGI("ESP_TEST", "This will be captured by our framework");
    
    // Cleanup
    sinker.hook_esp_log(false);
    sinker.remove_sinker(console_sink);
}
```

## Sink Implementation Examples

### File Storage Sink

```cpp
class FileSink : public ISink {
private:
    std::string _filename;
    
public:
    FileSink(const std::string& filename) : _filename(filename) {}
    
    void consume(const LogMessage& msg) override {
        FILE* file = fopen(_filename.c_str(), "a");
        if (file) {
            auto time_t = std::chrono::system_clock::to_time_t(msg.get_timestamp());
            fprintf(file, "[%ld] [%s] %s: %s\n", 
                    time_t, 
                    level_to_string(msg.get_level()),
                    msg.get_tag().c_str(), 
                    msg.get_message().c_str());
            fclose(file);
        }
    }
    
private:
    const char* level_to_string(LogLevel level) {
        switch(level) {
            case LogLevel::Error: return "ERROR";
            case LogLevel::Warning: return "WARNING";
            case LogLevel::Info: return "INFO";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Verbose: return "VERBOSE";
            default: return "UNKNOWN";
        }
    }
};
```

### WebSocket Sink

```cpp
class WebSocketSink : public ISink {
private:
    // Your WebSocket client implementation
    WebSocketClient& _client;
    
public:
    WebSocketSink(WebSocketClient& client) : _client(client) {}
    
    void consume(const LogMessage& msg) override {
        json log_entry = {
            {"timestamp", std::chrono::system_clock::to_time_t(msg.get_timestamp())},
            {"level", static_cast<int>(msg.get_level())},
            {"tag", msg.get_tag()},
            {"message", msg.get_message()}
        };
        
        _client.send(log_entry.dump());
    }
};
```

## Integration with ESP-IDF Project

Add to your project's `CMakeLists.txt`:

```cmake
# In your main component's CMakeLists.txt
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES loggable)
```

The component will be automatically found if placed in the `components/` directory of your ESP-IDF project.

## Thread Safety

All component operations are thread-safe:
- Uses `std::mutex` for synchronization
- Safe for multi-threaded environments
- No race conditions in log message processing
- Atomic operations for hook state management

## Memory Management

- Uses smart pointers (`std::shared_ptr`, `std::unique_ptr`) for automatic memory management
- No manual memory allocation required
- Automatic cleanup when objects go out of scope
- Efficient string handling with move semantics

## Performance

- Early filtering based on log level to avoid unnecessary processing
- Efficient message dispatch to multiple sinks
- Minimal overhead for disabled log levels
- Stack-allocated buffers for small messages
- Move semantics for efficient string handling

## Advanced Features

### ESP-IDF Log Hook

The component can intercept ESP-IDF logs by hooking into the `esp_log_set_vprintf` function. This allows you to capture and redirect all ESP-IDF logs through your custom sinks.

### Color Code Stripping

Automatically removes ANSI color codes from ESP-IDF logs, providing clean message content for processing.

### Multi-part Message Handling

Handles complex log messages that are split across multiple `vprintf` calls, ensuring complete message reconstruction.

## Limitations

- Requires C++17 or later
- Designed for ESP-IDF v5.x
- Log message parsing assumes standard ESP-IDF log format when using the hook
- Sink implementations should be fast to avoid blocking the logging thread

## License

This component follows the same license as your ESP-IDF project.

## Version Compatibility

- Compatible with ESP-IDF v5.x
- Requires C++17 support
- Uses standard C++ library features
- No external dependencies beyond ESP-IDF