#include "loggable.hpp"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <string_view>
#include <utility>
#include <fmt/core.h>
#include <fmt/format.h>

namespace loggable {

    // --- Sinker Implementation ---

    Sinker& Sinker::instance() noexcept {
        static Sinker inst;
        return inst;
    }

    void Sinker::add_sinker(std::shared_ptr<ISink> sinker) noexcept {
        if (sinker) {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _dispatch_internal(message);
    }

    void Sinker::_dispatch_internal(const LogMessage& message) noexcept {
        for (const auto& sinker : _sinkers) {
            if (sinker) [[likely]] {
                sinker->consume(message);
            }
        }
    }

    // --- Logger Implementation ---

    void Logger::log(LogLevel level, std::string_view message) noexcept {
        if (!is_log_level_enabled(level, Sinker::instance().get_level())) {
            return;
        }
        const auto now = std::chrono::system_clock::now();
        std::string_view tag = _owner._name;
        LogMessage msg(now, level, std::string(tag), std::string(message));
        Sinker::instance().dispatch(msg);
    }

    // --- Loggable Implementation ---

    Logger& Loggable::logger() noexcept {
        std::call_once(_logger_init_flag, [this]() {
            _logger = std::make_unique<Logger>(*this);
        });
        return *_logger;
    }

} // namespace loggable
