#include "core/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the compiled version matches the V0.26 engineering line", "[core][version]") {
    CHECK(blackbox::core::version == "0.26.0");
}
