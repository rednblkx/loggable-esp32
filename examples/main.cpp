#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <esp_log.h>
#include "loggable.hpp"
#include "loggable_espidf.hpp"

using namespace loggable;

/**
 * @brief An example sinker that prints formatted log messages to the console.
 */
class ConsoleSinker : public ISink {
public:
    void consume(const LogMessage& msg) override {
        // Basic formatting for the example
        auto level_str = "UNKNOWN";
        switch (msg.get_level()) {
            case LogLevel::Error:   level_str = "ERROR";   break;
            case LogLevel::Warning: level_str = "WARNING"; break;
            case LogLevel::Info:    level_str = "INFO";    break;
            case LogLevel::Debug:   level_str = "DEBUG";   break;
            case LogLevel::Verbose: level_str = "VERBOSE"; break;
            default: break;
        }
        
        // Simple time formatting for the example
        auto time_t = std::chrono::system_clock::to_time_t(msg.get_timestamp());
        std::cout << "[" << time_t << "] "
                  << "[" << level_str << "] "
                  << "[" << msg.get_tag() << "] "
                  << msg.get_message() << std::endl;
    }
};

/**
 * @brief An example loggable object that generates log messages.
 */
class MyAppComponent : public Loggable {
public:
    void do_something() {
        // Use the logger() method provided by the Loggable base class
        logger().logf(LogLevel::Info, "Starting operation...");
        
        for (int i = 0; i < 3; ++i) {
            logger().logf(LogLevel::Debug, "Processing item #%d", i + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        logger().log(LogLevel::Warning, "Operation completed with a minor issue.");
    }

protected:
    // Implement the log_name() method to provide a tag for the logger
    std::string_view log_name() const noexcept override {
        return "MyAppComponent";
    }
};

// --- Main application entry point ---
extern "C" void app_main(void) {
    std::cout << "--- Logging Sinker Example ---" << std::endl;

    // 1. Get the distributor instance
    auto& distributor = Sinker::instance();

    // 2. Set the global log level
    distributor.set_level(LogLevel::Debug);

    // 3. Create and register an sinker
    ConsoleSinker console_sinker;
    auto console_sinker_ptr = std::make_shared<ConsoleSinker>();
    distributor.add_sinker(console_sinker_ptr);

    // 4. Create a loggable component and use it
    MyAppComponent my_app;
    my_app.do_something();

    // 5. Demonstrate direct logging with a different logger
    struct AnotherComponent : Loggable {
        std::string_view log_name() const noexcept override { return "AnotherComponent"; }
    } another_app;
    
    another_app.logger().logf(LogLevel::Error, "This is a critical error from another component!");

    // 6. Hook into ESP_LOGI to prove it works (using the new loggable_espidf component)
    std::cout << "\n--- Installing ESP-IDF Log Hook ---" << std::endl;
    espidf::LogHook::install();

    // This log will be captured by our distributor and printed by ConsoleSinker
    ESP_LOGI("ESP_LOG_TEST", "This message from ESP_LOGI should be captured.");

    // 7. Unhook and unregister
    espidf::LogHook::uninstall();
    distributor.remove_sinker(console_sinker_ptr);
    
    std::cout << "\n--- Example Finished ---" << std::endl;
    std::cout << "Sinker removed. Subsequent logs will not be printed." << std::endl;

    // This log will not appear because the sinker was removed
    my_app.logger().log(LogLevel::Info, "This message should not be visible.");
}