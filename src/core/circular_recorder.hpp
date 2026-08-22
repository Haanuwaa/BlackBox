#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace blackbox::core {

struct RecorderStatistics {
    std::uint64_t epoch{};
    std::size_t capacity{};
    std::size_t size{};
    std::uint64_t total_appends{};
    std::uint64_t overwritten_samples{};
    std::uint64_t discarded_samples{};

    [[nodiscard]] constexpr double utilization() const noexcept {
        if (capacity == 0U) {
            return 0.0;
        }
        return static_cast<double>(size) / static_cast<double>(capacity);
    }

    friend constexpr bool operator==(const RecorderStatistics&,
                                     const RecorderStatistics&) = default;
};

template <typename T>
class RecorderSnapshot final {
public:
    RecorderSnapshot() = default;

    RecorderSnapshot(const std::uint64_t epoch, std::vector<T> samples) noexcept
        : epoch_{epoch}, samples_{std::move(samples)} {}

    [[nodiscard]] std::uint64_t epoch() const noexcept {
        return epoch_;
    }

    [[nodiscard]] std::span<const T> samples() const noexcept {
        return samples_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return samples_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return samples_.empty();
    }

private:
    std::uint64_t epoch_{};
    std::vector<T> samples_{};
};

// A mutex is intentionally preferred to a lock-free structure. Appends are
// constant-time, storage is allocated once per configuration epoch, and readers
// receive bounded copies rather than retaining the recorder lock.
template <typename T>
class CircularRecorder final {
public:
    explicit CircularRecorder(const std::size_t capacity)
        : storage_(capacity) {}

    CircularRecorder(const CircularRecorder&) = delete;
    CircularRecorder& operator=(const CircularRecorder&) = delete;
    CircularRecorder(CircularRecorder&&) = delete;
    CircularRecorder& operator=(CircularRecorder&&) = delete;

    void append(const T& sample) {
        append_impl(sample);
    }

    void append(T&& sample) {
        append_impl(std::move(sample));
    }

private:
    template <typename U>
    void append_impl(U&& sample) {
        const std::scoped_lock lock{mutex_};
        ++statistics_.total_appends;

        if (storage_.empty()) {
            ++statistics_.discarded_samples;
            return;
        }

        if (size_ < storage_.size()) {
            const auto destination = (oldest_ + size_) % storage_.size();
            storage_[destination] = std::forward<U>(sample);
            ++size_;
            return;
        }

        storage_[oldest_] = std::forward<U>(sample);
        oldest_ = (oldest_ + 1U) % storage_.size();
        ++statistics_.overwritten_samples;
    }

public:
    [[nodiscard]] RecorderSnapshot<T> snapshot(
        const std::size_t maximum_samples = std::numeric_limits<std::size_t>::max()) const {
        const std::scoped_lock lock{mutex_};
        const auto count = std::min(size_, maximum_samples);
        std::vector<T> copied;
        copied.reserve(count);

        if (!storage_.empty()) {
            const auto first = (oldest_ + (size_ - count)) % storage_.size();
            for (std::size_t index = 0U; index < count; ++index) {
                copied.push_back(storage_[(first + index) % storage_.size()]);
            }
        }

        return RecorderSnapshot<T>{statistics_.epoch, std::move(copied)};
    }

    [[nodiscard]] RecorderStatistics statistics() const noexcept {
        const std::scoped_lock lock{mutex_};
        auto result = statistics_;
        result.capacity = storage_.size();
        result.size = size_;
        return result;
    }

    // Reconfiguration begins a new epoch and deliberately does not retain old
    // samples whose cadence belongs to a different configuration.
    void reconfigure(const std::size_t capacity) {
        std::vector<T> replacement(capacity);
        const std::scoped_lock lock{mutex_};
        storage_.swap(replacement);
        oldest_ = 0U;
        size_ = 0U;
        const auto next_epoch = statistics_.epoch + 1U;
        statistics_ = RecorderStatistics{};
        statistics_.epoch = next_epoch;
    }

private:
    mutable std::mutex mutex_{};
    std::vector<T> storage_{};
    std::size_t oldest_{};
    std::size_t size_{};
    RecorderStatistics statistics_{};
};

} // namespace blackbox::core
