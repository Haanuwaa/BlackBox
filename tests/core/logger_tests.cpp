#include "core/logger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace core = blackbox::core;

namespace {

struct ResetLoggerSink final {
    ~ResetLoggerSink() { core::Logger::set_sink({}); }
};

} // namespace

TEST_CASE("logger emits bounded single-line component records",
          "[core][logger]") {
    ResetLoggerSink reset{};
    core::LogLevel observed_level{};
    std::string observed_component{};
    std::string observed_message{};
    core::Logger::set_sink(
        [&](const core::LogLevel level, const std::string_view component,
            const std::string_view message) {
            observed_level = level;
            observed_component = component;
            observed_message = message;
        });

    core::Logger::write(core::LogLevel::warning,
                        std::string(64U, 'c'),
                        "  first\r\nsecond\tthird\x01  ");
    CHECK(observed_level == core::LogLevel::warning);
    CHECK(observed_component == std::string(29U, 'c') + "...");
    CHECK(observed_message == "first second third?");

    core::Logger::write(core::LogLevel::info, {}, std::string(2'100U, 'x'));
    CHECK(observed_component == "general");
    REQUIRE(observed_message.size() == 2'048U);
    CHECK(observed_message.ends_with("..."));
}

TEST_CASE("logger sink may reenter without deadlocking",
          "[core][logger]") {
    ResetLoggerSink reset{};
    std::vector<std::string> messages{};
    bool nested{};
    core::Logger::set_sink(
        [&](const core::LogLevel, const std::string_view component,
            const std::string_view message) {
            messages.emplace_back(std::string{component} + ":" +
                                  std::string{message});
            if (!nested) {
                nested = true;
                core::Logger::write(core::LogLevel::debug, "nested", "record");
            }
        });

    core::Logger::write(core::LogLevel::info, "outer", "record");
    REQUIRE(messages.size() == 2U);
    CHECK(messages[0] == "outer:record");
    CHECK(messages[1] == "nested:record");
}
