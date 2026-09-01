#include "telemetry/linux/linux_psi_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace linux_telemetry = blackbox::telemetry::linux;

TEST_CASE("Linux PSI parser retains only exact cumulative stall totals",
          "[telemetry][linux][psi][privacy]") {
    const auto parsed =
        linux_telemetry::parse_linux_psi("some avg10=0.12 avg60=1.25 avg300=2.50 total=184467\n"
                                         "full avg10=0.00 avg60=0.01 avg300=0.02 total=42\n");
    REQUIRE(parsed.has_value());
    CHECK(parsed->some_total_microseconds == 184'467U);
    REQUIRE(parsed->full_total_microseconds.has_value());
    CHECK(*parsed->full_total_microseconds == 42U);
}

TEST_CASE("Linux PSI parser accepts a source with only the some dimension",
          "[telemetry][linux][psi][partial]") {
    const auto parsed =
        linux_telemetry::parse_linux_psi("some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->full_total_microseconds.has_value());
}

TEST_CASE("Linux PSI parser rejects missing duplicate unknown and malformed records",
          "[telemetry][linux][psi][robustness]") {
    using Error = linux_telemetry::LinuxPsiParseError;
    CHECK(linux_telemetry::parse_linux_psi("").error() == Error::empty);
    CHECK(linux_telemetry::parse_linux_psi("full avg10=0 avg60=0 avg300=0 total=1\n").error() ==
          Error::missing_some);
    CHECK(linux_telemetry::parse_linux_psi("some avg10=0 avg60=0 avg300=0 total=1\n"
                                           "some avg10=0 avg60=0 avg300=0 total=2\n")
              .error() == Error::duplicate_record);
    CHECK(linux_telemetry::parse_linux_psi("other avg10=0 avg60=0 avg300=0 total=1\n").error() ==
          Error::unexpected_record);
    CHECK(linux_telemetry::parse_linux_psi("some avg10=nan avg60=0 avg300=0 total=1\n").error() ==
          Error::malformed);
    CHECK(linux_telemetry::parse_linux_psi("some avg10=-1 avg60=0 avg300=0 total=1\n").error() ==
          Error::malformed);
    CHECK(
        linux_telemetry::parse_linux_psi("some avg10=100.01 avg60=0 avg300=0 total=1\n").error() ==
        Error::malformed);
    CHECK(linux_telemetry::parse_linux_psi("some avg10=0. avg60=0 avg300=0 total=1\n").error() ==
          Error::malformed);
    CHECK(linux_telemetry::parse_linux_psi("some avg10=0.0.0 avg60=0 avg300=0 total=1\n").error() ==
          Error::malformed);
    CHECK(linux_telemetry::parse_linux_psi(
              "some avg10=0 avg60=0 avg300=0 total=18446744073709551616\n")
              .error() == Error::malformed);
    CHECK(linux_telemetry::parse_linux_psi("some avg10=0 avg60=0 avg300=0 total=1 payload=secret\n")
              .error() == Error::malformed);
}

TEST_CASE("Linux PSI parser accepts fixed-point percentage boundaries",
          "[telemetry][linux][psi][robustness]") {
    CHECK(
        linux_telemetry::parse_linux_psi("some avg10=000.00 avg60=99.999 avg300=100.000 total=1\n")
            .has_value());
}
