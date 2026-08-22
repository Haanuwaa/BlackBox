#pragma once

#include "analysis/incident_analyzer.hpp"

#include <cstddef>
#include <vector>

namespace blackbox::analysis {

class RollingRobustBaseline final {
public:
    explicit RollingRobustBaseline(std::size_t capacity);

    void add(double value) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] RobustBaselineSummary summarize() const;
    [[nodiscard]] double percentile_rank(double value) const noexcept;

private:
    std::vector<double> values_{};
    std::size_t size_{};
    std::size_t next_{};
};

[[nodiscard]] double robust_z_score(const RobustBaselineSummary& baseline,
                                    double value) noexcept;
[[nodiscard]] double anomaly_score(double absolute_robust_z) noexcept;
[[nodiscard]] double baseline_percentile(const RollingRobustBaseline& baseline,
                                         double value);

} // namespace blackbox::analysis
