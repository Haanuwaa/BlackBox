#pragma once

#include "telemetry/event_collector.hpp"
#include <optional>

namespace blackbox::telemetry {

// Resetting cumulative rates after a long gap is always safe. Exempting that gap
// from reliability evidence requires a timestamped native resume notification.
class SamplingGapAccounting final {
public:
    explicit SamplingGapAccounting(SamplingCadenceState initial = {}) noexcept
        : observed_resumes_{initial.native_resumes} {}

    void note_gap(core::MonotonicTimePoint scheduled, core::MonotonicTimePoint actual,
                  std::uint64_t skipped) noexcept {
        ++unclassified_gaps;
        unclassified_skipped += skipped;
        pending_ = Gap{scheduled, actual, skipped};
    }

    void observe(const SamplingCadenceState state) noexcept {
        if (state.native_resumes <= observed_resumes_) return;
        native_resumes += state.native_resumes - observed_resumes_;
        observed_resumes_ = state.native_resumes;
        if (pending_ && state.last_resume_at >= pending_->scheduled &&
            state.last_resume_at <= pending_->actual + std::chrono::seconds{5}) {
            --unclassified_gaps;
            unclassified_skipped -= pending_->skipped;
            resume_skipped += pending_->skipped;
            pending_.reset();
        }
    }

    std::uint64_t native_resumes{};
    std::uint64_t unclassified_gaps{};
    std::uint64_t unclassified_skipped{};
    std::uint64_t resume_skipped{};

private:
    struct Gap {
        core::MonotonicTimePoint scheduled, actual;
        std::uint64_t skipped;
    };
    std::uint64_t observed_resumes_{};
    std::optional<Gap> pending_{};
};

} // namespace blackbox::telemetry
