#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "loggable.hpp"

using namespace loggable;

// Simple test framework for ESP-IDF environment
#define TEST_ASSERT_EQUAL(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("TEST FAILED: %s != %s at line %d\n", #expected, #actual, __LINE__); \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("TEST FAILED: '%s' != '%s' at line %d\n", (expected), (actual), __LINE__); \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("TEST FAILED: %s at line %d\n", #condition, __LINE__); \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_FALSE(condition) \
    do { \
        if (condition) { \
            printf("TEST FAILED: %s should be false at line %d\n", #condition, __LINE__); \
            return; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_PTR(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("TEST FAILED: pointers not equal at line %d\n", __LINE__); \
            return; \
        } \
    } while(0)

#define RUN_TEST(test_func) \
    do { \
        printf("Running %s... ", #test_func); \
        test_func(); \
        printf("PASSED\n"); \
    } while(0)

// Test sink for capturing log messages
class TestSink : public ISink {
public:
    static const int MAX_CAPTURED_MESSAGES = 100;
    
    struct CapturedMessage {
        LogLevel level;
        char tag[64];
        char message[256];
    };

    CapturedMessage captured_messages[MAX_CAPTURED_MESSAGES];
    int message_count;

    TestSink() : message_count(0) {}

    void consume(const LogMessage& msg) override {
        if (message_count < MAX_CAPTURED_MESSAGES) {
            captured_messages[message_count].level = msg.get_level();
            strncpy(captured_messages[message_count].tag, msg.get_tag().c_str(), sizeof(captured_messages[0].tag) - 1);
            captured_messages[message_count].tag[sizeof(captured_messages[0].tag) - 1] = '\0';
            strncpy(captured_messages[message_count].message, msg.get_message().c_str(), sizeof(captured_messages[0].message) - 1);
            captured_messages[message_count].message[sizeof(captured_messages[0].message) - 1] = '\0';
            message_count++;
        }
    }

    void clear() {
        message_count = 0;
    }
};

// Test loggable class
class TestLoggable : public Loggable {
public:
    TestLoggable(const char* name) : _name(name) {}
    
    void log_something(LogLevel level, const char* message) {
        logger().log(level, message);
    }

protected:
    std::string_view log_name() const noexcept override {
        return _name;
    }

private:
    const char* _name;
};

static TestSink* test_sink = nullptr;

void test_log_level_to_string() {
    TEST_ASSERT_EQUAL_STRING("ERROR", log_level_to_string(LogLevel::Error));
    TEST_ASSERT_EQUAL_STRING("WARNING", log_level_to_string(LogLevel::Warning));
    TEST_ASSERT_EQUAL_STRING("INFO", log_level_to_string(LogLevel::Info));
    TEST_ASSERT_EQUAL_STRING("DEBUG", log_level_to_string(LogLevel::Debug));
    TEST_ASSERT_EQUAL_STRING("VERBOSE", log_level_to_string(LogLevel::Verbose));
    TEST_ASSERT_EQUAL_STRING("NONE", log_level_to_string(LogLevel::None));
}

void test_is_log_level_enabled() {
    TEST_ASSERT_TRUE(is_log_level_enabled(LogLevel::Error, LogLevel::Verbose));
    TEST_ASSERT_TRUE(is_log_level_enabled(LogLevel::Verbose, LogLevel::Verbose));
    TEST_ASSERT_FALSE(is_log_level_enabled(LogLevel::Verbose, LogLevel::Error));
    TEST_ASSERT_TRUE(is_log_level_enabled(LogLevel::Info, LogLevel::Debug));
}

void test_sink_singleton() {
    Sinker& sinker1 = Sinker::instance();
    Sinker& sinker2 = Sinker::instance();
    
    TEST_ASSERT_EQUAL_PTR(&sinker1, &sinker2);
}

void test_basic_logging() {
    test_sink->clear();
    Sinker::instance().set_level(LogLevel::Verbose);
    
    TestLoggable test_obj("TestComponent");
    test_obj.log_something(LogLevel::Info, "Test message");
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL(LogLevel::Info, test_sink->captured_messages[0].level);
    TEST_ASSERT_EQUAL_STRING("TestComponent", test_sink->captured_messages[0].tag);
    TEST_ASSERT_EQUAL_STRING("Test message", test_sink->captured_messages[0].message);
}

void test_log_level_filtering() {
    test_sink->clear();
    Sinker::instance().set_level(LogLevel::Warning);
    
    TestLoggable test_obj("TestComponent");
    test_obj.log_something(LogLevel::Info, "Info message");
    test_obj.log_something(LogLevel::Warning, "Warning message");
    test_obj.log_something(LogLevel::Error, "Error message");
    
    TEST_ASSERT_EQUAL(2, test_sink->message_count);
    TEST_ASSERT_EQUAL(LogLevel::Warning, test_sink->captured_messages[0].level);
    TEST_ASSERT_EQUAL(LogLevel::Error, test_sink->captured_messages[1].level);
}

void test_multiple_sinks() {
    test_sink->clear();
    auto second_sink = std::make_shared<TestSink>();
    Sinker::instance().add_sinker(second_sink);
    
    TestLoggable test_obj("TestComponent");
    test_obj.log_something(LogLevel::Info, "Test message");
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL(1, second_sink->message_count);
    
    Sinker::instance().remove_sinker(second_sink);
}

void test_logger_log_method() {
    test_sink->clear();
    TestLoggable test_obj("TestComponent");
    test_obj.logger().log(LogLevel::Debug, "Debug message");
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL(LogLevel::Debug, test_sink->captured_messages[0].level);
    TEST_ASSERT_EQUAL_STRING("Debug message", test_sink->captured_messages[0].message);
}

void test_logger_logf_method() {
    test_sink->clear();
    TestLoggable test_obj("TestComponent");
    test_obj.logger().logf(LogLevel::Info, "Formatted message: %d %s", 42, "test");
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL_STRING("Formatted message: 42 test", test_sink->captured_messages[0].message);
}

void test_empty_message() {
    test_sink->clear();
    TestLoggable test_obj("TestComponent");
    test_obj.log_something(LogLevel::Info, "");
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL_STRING("", test_sink->captured_messages[0].message);
}

void test_large_message() {
    test_sink->clear();
    TestLoggable test_obj("TestComponent");
    
    // Create a large message
    char large_message[300];
    for (int i = 0; i < 250; i++) {
        large_message[i] = 'X';
    }
    large_message[250] = '\0';
    
    test_obj.log_something(LogLevel::Info, large_message);
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL(255, strlen(test_sink->captured_messages[0].message)); // Should be truncated
}

void test_sink_lifecycle() {
    test_sink->clear();
    
    // Create and register a temporary sink
    auto temp_sink = std::make_shared<TestSink>();
    Sinker::instance().add_sinker(temp_sink);
    
    TestLoggable test_obj("TestComponent");
    test_obj.log_something(LogLevel::Info, "Before removal");
    
    TEST_ASSERT_EQUAL(1, test_sink->message_count);
    TEST_ASSERT_EQUAL(1, temp_sink->message_count);
    
    // Remove the sink
    Sinker::instance().remove_sinker(temp_sink);
    
    test_obj.log_something(LogLevel::Info, "After removal");
    
    // Only the main sink should receive this message
    TEST_ASSERT_EQUAL(2, test_sink->message_count);
    TEST_ASSERT_EQUAL(1, temp_sink->message_count); // Should not increase
}

// Main test runner
extern "C" void app_main() {
    printf("Starting loggable component tests...\n");
    
    // Initialize test sink
    auto test_sink_ptr = std::make_shared<TestSink>();
    test_sink = test_sink_ptr.get();
    Sinker::instance().add_sinker(test_sink_ptr);
    Sinker::instance().set_level(LogLevel::Verbose);
    
    RUN_TEST(test_log_level_to_string);
    RUN_TEST(test_is_log_level_enabled);
    RUN_TEST(test_sink_singleton);
    RUN_TEST(test_basic_logging);
    RUN_TEST(test_log_level_filtering);
    RUN_TEST(test_multiple_sinks);
    RUN_TEST(test_logger_log_method);
    RUN_TEST(test_logger_logf_method);
    RUN_TEST(test_empty_message);
    RUN_TEST(test_large_message);
    RUN_TEST(test_sink_lifecycle);
    
    printf("All tests completed successfully!\n");
    
    // Cleanup
    Sinker::instance().remove_sinker(test_sink_ptr);
}