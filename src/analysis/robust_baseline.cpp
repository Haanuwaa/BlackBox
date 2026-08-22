#include "analysis/robust_baseline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace blackbox::analysis {
namespace {

[[nodiscard]] double percentile(const std::vector<double>& sorted,
                                const std::size_t numerator) noexcept {
    if (sorted.empty()) return 0.0;
    const auto rank = (sorted.size() * numerator + 99U) / 100U;
    return sorted[(std::max<std::size_t>)(1U, rank) - 1U];
}

[[nodiscard]] std::vector<double> sorted_values(
    const std::vector<double>& values, const std::size_t size) {
    std::vector<double> result(values.begin(),
                               values.begin() + static_cast<std::ptrdiff_t>(size));
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace

RollingRobustBaseline::RollingRobustBaseline(const std::size_t capacity)
    : values_(capacity) {}

void RollingRobustBaseline::add(const double value) noexcept {
    if (values_.empty() || !std::isfinite(value)) return;
    values_[next_] = value;
    next_ = (next_ + 1U) % values_.size();
    size_ = (std::min)(size_ + 1U, values_.size());
}

void RollingRobustBaseline::reset() noexcept {
    size_ = 0U;
    next_ = 0U;
}

std::size_t RollingRobustBaseline::size() const noexcept { return size_; }
std::size_t RollingRobustBaseline::capacity() const noexcept { return values_.size(); }

RobustBaselineSummary RollingRobustBaseline::summarize() const {
    RobustBaselineSummary result{};
    result.sample_count = size_;
    if (size_ == 0U) return result;
    const auto sorted = sorted_values(values_, size_);
    result.minimum = sorted.front();
    result.p05 = percentile(sorted, 5U);
    result.p25 = percentile(sorted, 25U);
    result.median = percentile(sorted, 50U);
    result.p75 = percentile(sorted, 75U);
    result.p95 = percentile(sorted, 95U);
    result.maximum = sorted.back();

    std::vector<double> deviations;
    deviations.reserve(sorted.size());
    for (const auto value : sorted) {
        deviations.push_back(std::abs(value - result.median));
    }
    std::sort(deviations.begin(), deviations.end());
    result.median_absolute_deviation = percentile(deviations, 50U);
    const auto mad_scale = result.median_absolute_deviation * 1.4826;
    const auto iqr_scale = (result.p75 - result.p25) / 1.349;
    const auto relative_floor = std::abs(result.median) * 0.01;
    result.robust_scale = (std::max)({mad_scale, iqr_scale, relative_floor, 1.0e-9});
    return result;
}

double RollingRobustBaseline::percentile_rank(const double value) const noexcept {
    if (size_ == 0U || !std::isfinite(value)) return 0.0;
    std::size_t at_or_below{};
    for (std::size_t index = 0U; index < size_; ++index) {
        if (values_[index] <= value) ++at_or_below;
    }
    return static_cast<double>(at_or_below) / static_cast<double>(size_);
}

double robust_z_score(const RobustBaselineSummary& baseline,
                      const double value) noexcept {
    if (baseline.sample_count == 0U || !std::isfinite(value) ||
        baseline.robust_scale <= 0.0) {
        return 0.0;
    }
    return (value - baseline.median) / baseline.robust_scale;
}

double anomaly_score(const double absolute_robust_z) noexcept {
    constexpr double threshold = 3.5;
    if (!std::isfinite(absolute_robust_z) || absolute_robust_z <= threshold) {
        return 0.0;
    }
    return std::clamp(1.0 - std::exp(-(absolute_robust_z - threshold) / 3.0),
                      0.0, 1.0);
}

double baseline_percentile(const RollingRobustBaseline& baseline,
                           const double value) {
    return baseline.percentile_rank(value);
}

} // namespace blackbox::analysis
