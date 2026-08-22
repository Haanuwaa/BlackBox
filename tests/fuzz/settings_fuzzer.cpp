#include "app/product_settings.hpp"
#include "app/recorder_settings.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
    if (data == nullptr || size > 20'000U) return 0;
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const auto product = blackbox::app::parse_product_settings_text(text);
    if (product) {
        static_cast<void>(blackbox::app::validate_product_settings(*product));
    }
    const auto recorder = blackbox::app::parse_recorder_settings_text(text);
    if (recorder) {
        static_cast<void>(blackbox::telemetry::validate_recorder_configuration(
            recorder->values));
    }
    return 0;
}
