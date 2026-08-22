#include "storage/incident_archive.hpp"
#include "storage/test_incident.hpp"
#include "ui/dashboard.hpp"
#include "ui/incident_viewer.hpp"

#include <imgui.h>
#include <implot.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

namespace storage = blackbox::storage;
namespace ui = blackbox::ui;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::uint64_t working_set_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != FALSE) {
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
#endif
    return 0U;
}

struct Timing {
    double average_ms{};
    double p95_ms{};
    double p99_ms{};
    double maximum_ms{};
};

[[nodiscard]] Timing summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto percentile = [&](const std::size_t numerator) {
        const auto rank = (values.size() * numerator + 99U) / 100U;
        return values[std::max<std::size_t>(1U, rank) - 1U];
    };
    return {std::accumulate(values.begin(), values.end(), 0.0) /
                static_cast<double>(values.size()),
            percentile(95U), percentile(99U), values.back()};
}

template <typename Operation>
[[nodiscard]] Timing measure(const std::size_t trials, Operation operation) {
    std::vector<double> values;
    values.reserve(trials);
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        const auto started = std::chrono::steady_clock::now();
        operation();
        values.push_back(std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - started}.count());
    }
    return summarize(std::move(values));
}

void print(const char* name, const Timing timing) {
    std::cout << name << ",average_ms=" << timing.average_ms
              << ",p95_ms=" << timing.p95_ms << ",p99_ms=" << timing.p99_ms
              << ",maximum_ms=" << timing.maximum_ms << '\n';
}

class ImGuiFixture final {
public:
    ImGuiFixture() {
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2{1'920.0F, 1'080.0F};
        io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ImGuiFixture() {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
};

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    storage::SqliteIncidentArchive archive{{std::filesystem::path{":memory:"}}};
    if (!archive.open()) return 1;
    const auto representative = storage::test::representative_incident();
    for (std::size_t index = 0U; index < 200U; ++index) {
        const auto stored = archive.store(*representative);
        if (!stored) return 2;
        if (index % 10U == 0U) {
            static_cast<void>(archive.update_annotation(
                *stored, {"Network", "searchable packet loss fixture"}));
        }
    }
    const auto large = storage::test::scaled_incident(500U, 150U);
    const auto large_id = archive.store(*large);
    if (!large_id) return 3;

    storage::IncidentListQuery list_query{};
    list_query.limit = 50U;
    print("list_50_of_201", measure(100U, [&] {
        static_cast<void>(archive.list_page(list_query));
    }));
    list_query.search = "packet";
    print("search_20_of_201", measure(100U, [&] {
        static_cast<void>(archive.list_page(list_query));
    }));
    print("load_75000_process_rows", measure(5U, [&] {
        static_cast<void>(archive.load(*large_id));
    }));

    for (const auto process_count : {50U, 200U, 500U}) {
        const auto incident = storage::test::scaled_incident(process_count, 150U);
        const auto timing = measure(5U, [&] {
            static_cast<void>(ui::build_incident_detail(1, 0, {}, {}, *incident));
        });
        const auto name = "build_" + std::to_string(process_count) + "_processes";
        print(name.c_str(), timing);
    }

    const auto memory_before = working_set_bytes();
    std::atomic<bool> monitoring{true};
    std::atomic<std::uint64_t> peak{memory_before};
    std::jthread monitor{[&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested() && monitoring.load()) {
            auto observed = working_set_bytes();
            auto previous = peak.load();
            while (observed > previous && !peak.compare_exchange_weak(previous, observed)) {}
            std::this_thread::sleep_for(250us);
        }
    }};
    auto large_detail = ui::build_incident_detail(1, 0, {}, {}, *large);
    monitoring.store(false);
    monitor.request_stop();
    monitor.join();
    peak.store((std::max)(peak.load(), working_set_bytes()));
    std::cout << "build_500_peak_temporary_working_set_bytes="
              << (peak.load() > memory_before ? peak.load() - memory_before : 0U) << '\n';

    auto imgui = std::make_unique<ImGuiFixture>();
    auto dashboard = std::make_unique<ui::DashboardState>();
    dashboard->incident_capture_enabled = true;
    auto content = std::make_shared<ui::IncidentViewerContent>();
    content->state = ui::IncidentViewerLoadState::ready;
    content->generation = 1U;
    content->status = "201 matching incidents";
    content->total_matching = 201U;
    content->incidents.push_back({1, 0, "1970-01-01 00:00:00.000 UTC", 149.0,
                                  "Large", "75,000 process rows", 150U, 75'000U});
    content->detail = std::move(large_detail);
    auto viewer = std::make_unique<ui::IncidentViewerState>();
    viewer->content = std::move(content);
    auto product = std::make_unique<ui::ProductUiState>();
    product->page = ui::ProductPage::detail;
    product->onboarding_open = false;
    ImGui::NewFrame();
    static_cast<void>(ui::render_dashboard(*dashboard, *viewer, *product));
    ImGui::Render();
    print("viewer_frame_500_processes", measure(100U, [&] {
        ImGui::NewFrame();
        static_cast<void>(ui::render_dashboard(*dashboard, *viewer, *product));
        ImGui::Render();
    }));
    return 0;
}
