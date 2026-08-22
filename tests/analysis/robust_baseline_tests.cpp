#include "analysis/robust_baseline.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

namespace analysis = blackbox::analysis;

TEST_CASE("rolling robust baseline evicts old values and reports robust percentiles",
          "[analysis][baseline]") {
    analysis::RollingRobustBaseline baseline{5U};
    for (int value = 1; value <= 7; ++value) baseline.add(static_cast<double>(value));

    const auto summary = baseline.summarize();
    CHECK(baseline.size() == 5U);
    CHECK(summary.minimum == 3.0);
    CHECK(summary.p05 == 3.0);
    CHECK(summary.p25 == 4.0);
    CHECK(summary.median == 5.0);
    CHECK(summary.p75 == 6.0);
    CHECK(summary.p95 == 7.0);
    CHECK(summary.maximum == 7.0);
    CHECK(summary.median_absolute_deviation == 1.0);
    CHECK(baseline.percentile_rank(6.0) == 0.8);
    CHECK(analysis::robust_z_score(summary, 5.0) == 0.0);
    CHECK(analysis::anomaly_score(3.5) == 0.0);
    CHECK(analysis::anomaly_score(20.0) > 0.99);

    baseline.add(std::numeric_limits<double>::quiet_NaN());
    CHECK(baseline.size() == 5U);
    baseline.reset();
    CHECK(baseline.size() == 0U);
}
