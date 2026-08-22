#include "storage/test_incident.hpp"
#include "ui/dashboard.hpp"
#include "ui/incident_viewer.hpp"
#include "ui/product_ui_model.hpp"

#include <catch2/catch_test_macros.hpp>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>
#include <implot.h>

#include <bit>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace storage = blackbox::storage;
namespace ui = blackbox::ui;

#ifndef BLACKBOX_QUALIFICATION_SOURCE_REVISION
#error "BLACKBOX_QUALIFICATION_SOURCE_REVISION must identify the UI qualification build"
#endif

namespace {

[[nodiscard]] constexpr const char* page_name(const ui::ProductPage page) noexcept {
    switch (page) {
    case ui::ProductPage::live: return "live";
    case ui::ProductPage::incidents: return "incidents";
    case ui::ProductPage::detail: return "detail";
    case ui::ProductPage::patterns: return "patterns";
    case ui::ProductPage::settings: return "settings";
    case ui::ProductPage::diagnostics: return "diagnostics";
    }
    return "unknown";
}

[[nodiscard]] std::filesystem::path evidence_directory() {
    char* raw_value{};
    std::size_t value_size{};
    if (_dupenv_s(&raw_value, &value_size, "BLACKBOX_UI_EVIDENCE_DIR") != 0) {
        throw std::runtime_error{"cannot read BLACKBOX_UI_EVIDENCE_DIR"};
    }
    const std::unique_ptr<char, decltype(&std::free)> value{raw_value, &std::free};
    if (value_size == 0U || raw_value == nullptr || *raw_value == '\0') return {};
    const std::filesystem::path directory{raw_value};
    std::error_code issue;
    if (!directory.is_absolute() ||
        !std::filesystem::is_directory(directory, issue) || issue) {
        throw std::runtime_error{
            "BLACKBOX_UI_EVIDENCE_DIR must be an existing absolute directory"};
    }
    return directory;
}

void require_matching_evidence_revision(const std::filesystem::path& directory) {
    if (directory.empty()) return;
    char* raw_value{};
    std::size_t value_size{};
    if (_dupenv_s(&raw_value, &value_size, "BLACKBOX_UI_SOURCE_REVISION") != 0) {
        throw std::runtime_error{"cannot read BLACKBOX_UI_SOURCE_REVISION"};
    }
    const std::unique_ptr<char, decltype(&std::free)> value{raw_value, &std::free};
    if (value_size == 0U || raw_value == nullptr || *raw_value == '\0') {
        throw std::runtime_error{
            "BLACKBOX_UI_SOURCE_REVISION is required when publishing UI evidence"};
    }
    if (std::string_view{raw_value} != BLACKBOX_QUALIFICATION_SOURCE_REVISION) {
        throw std::runtime_error{
            "UI evidence revision does not match the compiled qualification executable"};
    }
}

void save_named_evidence(SDL_Surface& surface,
                         const std::filesystem::path& directory,
                         const std::string_view fixture,
                         const unsigned display_mode,
                         const std::string_view case_name) {
    if (directory.empty()) return;
    const auto filename = std::string{fixture} + '-' +
                          (display_mode == 0U ? "100pct" : "150pct-high-contrast") +
                          '-' + std::string{case_name} + ".bmp";
    const auto destination = directory / filename;
    std::error_code issue;
    if (std::filesystem::exists(destination, issue) || issue) {
        throw std::runtime_error{"UI evidence destination is occupied"};
    }
    const auto utf8 = destination.u8string();
    const std::string path{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    if (!SDL_SaveBMP(&surface, path.c_str())) {
        throw std::runtime_error{SDL_GetError()};
    }
}

void save_evidence(SDL_Surface& surface, const std::filesystem::path& directory,
                   const std::string_view fixture, const unsigned display_mode,
                   const ui::ProductPage page) {
    save_named_evidence(surface, directory, fixture, display_mode, page_name(page));
}

class ImGuiContextFixture final {
public:
    ImGuiContextFixture(const int physical_width, const int physical_height,
                        const float framebuffer_scale,
                        const bool high_contrast) {
        surface_ = SDL_CreateSurface(physical_width, physical_height,
                                     SDL_PIXELFORMAT_RGBA32);
        if (surface_ == nullptr) {
            throw std::runtime_error{SDL_GetError()};
        }
        renderer_ = SDL_CreateSoftwareRenderer(surface_);
        if (renderer_ == nullptr) {
            throw std::runtime_error{SDL_GetError()};
        }
        if (!SDL_SetRenderScale(renderer_, framebuffer_scale, framebuffer_scale)) {
            throw std::runtime_error{SDL_GetError()};
        }
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2{1'100.0F, 700.0F};
        io.DisplayFramebufferScale = ImVec2{framebuffer_scale, framebuffer_scale};
        io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->AddFontDefault();
        if (!ImGui_ImplSDLRenderer3_Init(renderer_)) {
            throw std::runtime_error{"cannot initialize ImGui software renderer"};
        }
        ui::apply_accessibility_style(high_contrast);
    }
    ~ImGuiContextFixture() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroySurface(surface_);
    }

    ImGuiContextFixture(const ImGuiContextFixture&) = delete;
    ImGuiContextFixture& operator=(const ImGuiContextFixture&) = delete;

    void render(const ui::DashboardState& dashboard,
                ui::IncidentViewerState& viewer,
                ui::ProductUiState& product) {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();
        static_cast<void>(ui::render_dashboard(dashboard, viewer, product));
        ImGui::Render();
        if (!SDL_SetRenderDrawColor(renderer_, 18U, 20U, 24U, 255U) ||
            !SDL_RenderClear(renderer_)) {
            throw std::runtime_error{SDL_GetError()};
        }
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        if (!SDL_RenderPresent(renderer_)) {
            throw std::runtime_error{SDL_GetError()};
        }
    }

    [[nodiscard]] SDL_Surface* surface() const noexcept { return surface_; }

    void scroll_main_window(const float wheel_delta) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(1'095.0F, 350.0F);
        io.AddMouseWheelEvent(0.0F, wheel_delta);
    }

private:
    SDL_Surface* surface_{};
    SDL_Renderer* renderer_{};
};

[[nodiscard]] std::uint64_t draw_signature(const ImDrawData& data) {
    std::uint64_t hash{1469598103934665603ULL};
    const auto add = [&hash](const std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) {
            hash ^= (value >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    add(static_cast<std::uint32_t>(data.TotalVtxCount));
    add(static_cast<std::uint32_t>(data.TotalIdxCount));
    for (int list_index = 0; list_index < data.CmdListsCount; ++list_index) {
        const auto* list = data.CmdLists[list_index];
        for (const auto& vertex : list->VtxBuffer) {
            add(std::bit_cast<std::uint32_t>(vertex.pos.x));
            add(std::bit_cast<std::uint32_t>(vertex.pos.y));
            add(vertex.col);
        }
    }
    return hash;
}

[[nodiscard]] std::uint64_t pixel_signature(SDL_Surface& surface,
                                            std::size_t& distinct_colors) {
    if (!SDL_LockSurface(&surface)) {
        throw std::runtime_error{SDL_GetError()};
    }
    std::uint64_t hash{1469598103934665603ULL};
    std::set<std::uint32_t> colors;
    const auto* pixels = static_cast<const std::uint8_t*>(surface.pixels);
    for (int y = 0; y < surface.h; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y * surface.pitch);
        for (int x = 0; x < surface.w; ++x) {
            const auto offset = static_cast<std::size_t>(x) * 4U;
            std::uint32_t color{};
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                const auto value = row[offset + channel];
                hash ^= value;
                hash *= 1099511628211ULL;
                color |= static_cast<std::uint32_t>(value) << (channel * 8U);
            }
            if (colors.size() < 256U) colors.insert(color);
        }
    }
    SDL_UnlockSurface(&surface);
    distinct_colors = colors.size();
    return hash;
}

void render_fixture(const std::shared_ptr<const blackbox::core::IncidentSnapshot>& incident,
                    const std::string_view fixture_name) {
    auto dashboard = std::make_unique<ui::DashboardState>();
    dashboard->recorder_status = "Recording";
    dashboard->incident_capture_enabled = true;
    dashboard->incident_capture_status = "Ready";
    dashboard->hotkey_status = "Ctrl+Shift+F12";
    dashboard->storage_status = "Ready";
    auto content = std::make_shared<ui::IncidentViewerContent>();
    content->state = ui::IncidentViewerLoadState::ready;
    content->status = "1 matching incident";
    content->generation = 1U;
    content->total_matching = 1U;
    content->incidents.push_back({1, 0, "1970-01-01 00:00:00.000 UTC", 10.0,
                                  "Fixture", "Smoke test", incident->system_samples().size(),
                                  incident->process_samples().size()});
    content->detail = ui::build_incident_detail(1, 0, "Fixture", "Smoke test", *incident);
    content->detail->analysis.state = ui::IncidentAnalysisViewState::ready;
    content->detail->analysis.status = "Explainable incident-local statistical ranking";
    content->detail->analysis.baseline_start_seconds = -90.0;
    content->detail->analysis.baseline_end_seconds = -30.0;
    content->detail->analysis.evaluation_start_seconds = -30.0;
    content->detail->analysis.evaluation_end_seconds = 30.0;
    content->detail->analysis.resources.push_back(
        {"CPU", 0.94, "High", "CPU higher: 95.0% vs median 20.0% (|z| 25.0, n=60)"});
    content->detail->analysis.resources.push_back(
        {"Disk", 0.12, "High", "disk read unchanged"});
    content->detail->analysis.processes.push_back(
        {"fixture.exe (PID 42)", 0.91, "High", "CPU higher than baseline"});
    ui::IncidentContributorRow contributor{
        "fixture.exe (PID 42)", 0.78,
        "Likely contributor (correlation only)",
        "Preceding activity began 4.0 s before the marker",
        "anomaly 100%; timing 100%; resource match 90%; duration 80%; coverage 75%"};
    contributor.executable_key = "path:c:/fixtures/fixture.exe";
    contributor.resource = ui::IncidentContributorRow::Resource::cpu;
    contributor.attribution =
        ui::IncidentContributorRow::Attribution::not_a_contributor;
    contributor.score_before_feedback = 0.91;
    contributor.feedback_multiplier = 0.857;
    contributor.feedback_matching_observations = 5U;
    contributor.feedback_confirmed_observations = 1U;
    contributor.feedback_rejected_observations = 4U;
    contributor.feedback_status =
        "Bounded reduction x0.857 from 1 confirmed / 4 rejected exact prior attributions";
    content->detail->analysis.contributors.push_back(contributor);
    auto ambiguous = contributor;
    ambiguous.name = "spanning.exe (PID 84)";
    ambiguous.score = 0.61;
    ambiguous.assessment = "Ambiguous correlate across marker";
    ambiguous.timing =
        "Marker-spanning activity began 1.0 s before the marker; most anomalous samples followed it (2 pre / 8 post)";
    ambiguous.executable_key = "path:c:/fixtures/spanning.exe";
    ambiguous.attribution = ui::IncidentContributorRow::Attribution::unsure;
    ambiguous.temporal_relationship =
        ui::IncidentContributorRow::TemporalRelationship::
            marker_spanning_ambiguous;
    ambiguous.score_before_feedback = ambiguous.score;
    ambiguous.feedback_multiplier = 1.0;
    ambiguous.feedback_matching_observations = 0U;
    ambiguous.feedback_confirmed_observations = 0U;
    ambiguous.feedback_rejected_observations = 0U;
    ambiguous.feedback_status =
        "No prior explicit attribution applies; positive learning requires preceding activity";
    content->detail->analysis.contributors.push_back(std::move(ambiguous));
    auto reaction = contributor;
    reaction.name = "reaction.exe (PID 126)";
    reaction.score = 0.39;
    reaction.assessment = "Possible victim/reaction (not a causal rank)";
    reaction.timing =
        "Possible victim/reaction began 2.0 s after the marker; anomalous span 5.0 s (0 pre / 5 post samples)";
    reaction.executable_key = "path:c:/fixtures/reaction.exe";
    reaction.attribution = ui::IncidentContributorRow::Attribution::unsure;
    reaction.temporal_relationship =
        ui::IncidentContributorRow::TemporalRelationship::post_marker_reaction;
    reaction.score_before_feedback = reaction.score;
    reaction.feedback_multiplier = 1.0;
    reaction.feedback_matching_observations = 0U;
    reaction.feedback_confirmed_observations = 0U;
    reaction.feedback_rejected_observations = 0U;
    reaction.feedback_status =
        "Confirmation can be saved for this incident but cannot teach positive uplift";
    content->detail->analysis.contributors.push_back(std::move(reaction));
    if (fixture_name == "representative") {
        auto& event = content->detail->system_events.front();
        event.source = "Storage";
        event.event = "Storage I/O retry reported";
        event.level = "Warning";
        event.native_event_id = 153U;
        auto& analysis = content->detail->analysis;
        analysis.diagnosis.pipeline_version = 13U;
        analysis.diagnosis.evidence_model_version = 12U;
        analysis.diagnosis.configuration_fingerprint = 6'701'770'989'141'957'614ULL;
        analysis.diagnosis.inference = "Local statistical analysis; native ML not adopted";
        analysis.diagnosis.suppressed_by_feedback = true;
        analysis.feedback.applicable = true;
        analysis.feedback.ready = true;
        analysis.feedback.suppressing = true;
        analysis.feedback.observations_considered = 7U;
        analysis.feedback.matching_observations = 5U;
        analysis.feedback.confirmed_problem_observations = 1U;
        analysis.feedback.false_positive_observations = 4U;
        analysis.feedback.false_positive_fraction = 4.0 / 5.0;
        analysis.feedback.confidence_multiplier = 0.56;
        analysis.feedback.profile_revision = 3U;
        analysis.feedback.reset_after_utc_milliseconds = 1'799'000'000'000LL;
        analysis.feedback.rollback_available = true;
        analysis.feedback.status =
            "4 of 5 exact prior trigger signatures were not noticed; automatic-trigger confidence reduced to 56% of its unadjusted value";
        analysis.similar_incidents.applicable = true;
        analysis.similar_incidents.ready = true;
        analysis.similar_incidents.symptom = "Game stutter";
        analysis.similar_incidents.observations_considered = 5U;
        analysis.similar_incidents.answered_observations = 4U;
        analysis.similar_incidents.confirmed_problem_observations = 4U;
        analysis.similar_incidents.categorized_confirmations = 4U;
        analysis.similar_incidents.matching_confirmations = 3U;
        analysis.similar_incidents.problem_fraction = 1.0;
        analysis.similar_incidents.category_consensus = 0.75;
        analysis.similar_incidents.status =
            "3 of 4 categorized prior confirmations agree on Game stutter; 4 of 4 answered similar incidents confirmed a noticed problem";
    }
    auto viewer = std::make_unique<ui::IncidentViewerState>();
    viewer->content = std::move(content);
    auto product = std::make_unique<ui::ProductUiState>();
    product->onboarding_open = false;
    std::set<std::uint64_t> draw_signatures;
    std::set<std::uint64_t> raster_signatures;
    const auto evidence = evidence_directory();
    require_matching_evidence_revision(evidence);
    for (unsigned display_mode = 0U; display_mode < 2U; ++display_mode) {
        const int physical_width = display_mode == 0U ? 1'100 : 1'650;
        const int physical_height = display_mode == 0U ? 700 : 1'050;
        const float scale = display_mode == 0U ? 1.0F : 1.5F;
        ImGuiContextFixture context{physical_width, physical_height, scale,
                                    display_mode == 1U};
        for (unsigned page = 0U; page < 6U; ++page) {
            product->page = static_cast<ui::ProductPage>(page);
            if (product->page == ui::ProductPage::detail) {
                // First render initializes this incident's synchronized plot range.
                context.render(*dashboard, *viewer, *product);
                REQUIRE(ui::set_timeline_cursor(
                    *product, -15.0, product->timeline_min, product->timeline_max));
            }
            context.render(*dashboard, *viewer, *product);
            REQUIRE(ImGui::GetDrawData() != nullptr);
            INFO("display mode " << display_mode << ", product page " << page);
            CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
            draw_signatures.insert(draw_signature(*ImGui::GetDrawData()));
            std::size_t distinct_colors{};
            raster_signatures.insert(pixel_signature(*context.surface(), distinct_colors));
            CHECK(distinct_colors >= 4U);
            save_evidence(*context.surface(), evidence, fixture_name,
                          display_mode, product->page);
            if (product->page == ui::ProductPage::detail) {
                if (fixture_name == "representative") {
                    for (unsigned step = 0U; step < 2U; ++step) {
                        context.scroll_main_window(-5.0F);
                        context.render(*dashboard, *viewer, *product);
                    }
                    save_named_evidence(*context.surface(), evidence, fixture_name,
                                        display_mode, "detail-feedback-controls");
                    for (unsigned step = 0U; step < 2U; ++step) {
                        context.scroll_main_window(5.0F);
                        context.render(*dashboard, *viewer, *product);
                    }
                }
                for (unsigned step = 0U; step < 3U; ++step) {
                    context.scroll_main_window(-20.0F);
                    context.render(*dashboard, *viewer, *product);
                }
                save_named_evidence(*context.surface(), evidence, fixture_name,
                                    display_mode, "detail-timeline-cursor");
                for (unsigned step = 0U; step < 3U; ++step) {
                    context.scroll_main_window(20.0F);
                    context.render(*dashboard, *viewer, *product);
                }
            }
        }
    }
    CHECK(draw_signatures.size() == 12U);
    CHECK(raster_signatures.size() == 12U);
}

} // namespace

TEST_CASE("incident viewer renders representative and large archive fixtures",
          "[ui][viewer][smoke][interaction][screenshot]") {
    SECTION("representative") {
        render_fixture(storage::test::representative_incident(), "representative");
    }
    SECTION("large") {
        render_fixture(storage::test::scaled_incident(500U, 150U), "large");
    }
}
