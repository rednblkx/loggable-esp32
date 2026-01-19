# Loggable

A modern, cross-platform C++20 logging framework with multiple sink support.

## Features

- **Cross-Platform**: Pure C++ with no platform dependencies in core library
- **Type-Safe Logging**: Strongly typed log levels and message handling
- **Multiple Sink Support**: Register multiple log sinks (console, file, network, custom)
- **Thread-Safe**: Full thread safety using C++ standard library synchronization primitives
- **Namespace Organization**: Clean API organized under `loggable` namespace

## Platform Adapters

For platform-specific integrations, use separate adapter components:

| Platform | Component | Description |
|----------|-----------|-------------|
| ESP-IDF  | [`loggable_espidf`](https://github.com/rednblkx/loggable-espidf) | Hooks into `ESP_LOGx` macros via `esp_log_set_vprintf` |

## API Reference

```cpp
namespace loggable {

    enum class LogLevel { None, Error, Warning, Info, Debug, Verbose };

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
        // Careful: this is called from a loop, so make sure this doesn't block
        virtual void consume(const LogMessage& message) = 0;
    };

    class Sinker {
    public:
        static Sinker& instance();
        void add_sinker(std::shared_ptr<ISink> sinker);
        void remove_sinker(const std::shared_ptr<ISink>& sinker);
        void set_level(LogLevel level) noexcept;
        LogLevel get_level() const noexcept;
        void dispatch(const LogMessage& message) noexcept;
    };

    class Logger {
    public:
        void log(LogLevel level, std::string_view message);
        void logf(LogLevel level, const char* format, ...);
        void vlogf(LogLevel level, const char* format, va_list args);
    };

    class Loggable {
    public:
        Logger& logger();
    protected:
        virtual std::string_view log_name() const noexcept = 0;
    };

}
```

## Usage Example

```cpp
#include "loggable.hpp"
using namespace loggable;

class ConsoleSink : public ISink {
public:
    void consume(const LogMessage& msg) override {
        std::cout << "[" << log_level_to_string(msg.get_level()) << "] "
                  << "[" << msg.get_tag() << "] "
                  << msg.get_message() << std::endl;
    }
};

class MyComponent : public Loggable {
public:
    void do_work() {
        logger().logf(LogLevel::Info, "Processing %d items", 42);
    }

protected:
    std::string_view log_name() const noexcept override { return "MyComponent"; }
};

int main() {
    auto& sinker = Sinker::instance();
    sinker.set_level(LogLevel::Debug);
    
    auto console_sink = std::make_shared<ConsoleSink>();
    sinker.add_sinker(console_sink);
    
    MyComponent component;
    component.do_work();
    
    sinker.remove_sinker(console_sink);
}
```

## ESP-IDF Integration

To capture ESP-IDF logs (`ESP_LOGx` macros), use the `loggable_espidf` adapter:

```cpp
#include "loggable.hpp"
#include "loggable_espidf.hpp"

extern "C" void app_main(void) {
    auto& sinker = loggable::Sinker::instance();
    sinker.add_sinker(std::make_shared<MySink>());
    
    // Install ESP-IDF log hook
    loggable::espidf::LogHook::install();
    
    ESP_LOGI("TAG", "This will be captured by loggable sinks");
    
    // Uninstall when done
    loggable::espidf::LogHook::uninstall();
}
```

Add to your component's `CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES loggable loggable_espidf)
```

## Building

### ESP-IDF

Place in the `components/` directory and add `loggable` to your `REQUIRES`.

### Standard CMake

```cmake
add_subdirectory(loggable)
target_link_libraries(your_target PRIVATE loggable)
```

## Requirements

- C++20 or later
- For ESP-IDF: v5.x

## License

MIT License
