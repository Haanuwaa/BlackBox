add_executable(blackbox_tests
    core/circular_recorder_tests.cpp
    core/incident_tests.cpp
    core/logger_tests.cpp
    core/version_tests.cpp
    telemetry/collector_tests.cpp
    telemetry/collection_timing_tests.cpp
    telemetry/disk_quality_tracker_tests.cpp
    telemetry/event_collector_tests.cpp
    telemetry/incident_snapshot_builder_tests.cpp
    telemetry/linux_proc_parser_tests.cpp
    telemetry/linux_uevent_parser_tests.cpp
    telemetry/mock_telemetry_provider_tests.cpp
    telemetry/network_interface_tracker_tests.cpp
    telemetry/normalizer_tests.cpp
    telemetry/process_metadata_cache_tests.cpp
    telemetry/process_normalizer_tests.cpp
    telemetry/public_headers_tests.cpp
    telemetry/types_tests.cpp
    app/product_settings_tests.cpp
    app/recorder_settings_tests.cpp
    app/support_bundle_tests.cpp
    app/wall_clock_report_tests.cpp
    property/strict_v1_input_property_tests.cpp
)
target_compile_features(blackbox_tests PRIVATE cxx_std_23)
target_compile_definitions(blackbox_tests PRIVATE
    BLACKBOX_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    BLACKBOX_QUALIFICATION_SOURCE_REVISION="${BLACKBOX_EFFECTIVE_SOURCE_REVISION}"
)
target_link_libraries(blackbox_tests PRIVATE
    BlackBox::Core
    BlackBox::Telemetry
    BlackBox::AppSupport
    Catch2::Catch2WithMain
)
blackbox_copy_address_sanitizer_runtime(blackbox_tests)

if(BLACKBOX_BUILD_FUZZERS)
    add_executable(blackbox_settings_fuzzer
        fuzz/settings_fuzzer.cpp)
    target_include_directories(blackbox_settings_fuzzer PRIVATE
        ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_settings_fuzzer PRIVATE cxx_std_23)
    target_compile_options(blackbox_settings_fuzzer PRIVATE -fsanitize=fuzzer)
    target_link_options(blackbox_settings_fuzzer PRIVATE -fsanitize=fuzzer)
    target_link_libraries(blackbox_settings_fuzzer PRIVATE BlackBox::AppSupport)
    blackbox_set_project_warnings(blackbox_settings_fuzzer)

    file(COPY fuzz/corpus/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus)
    add_test(
        NAME settings_native_fuzz_smoke
        COMMAND blackbox_settings_fuzzer
            -runs=2000 -max_len=20000 -timeout=5
            ${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus)
    set_tests_properties(settings_native_fuzz_smoke PROPERTIES LABELS "fuzz")
endif()

if(BLACKBOX_ENABLE_AUTOMATIC_DETECTION)
    target_sources(blackbox_tests PRIVATE
        telemetry/automatic_incident_detector_tests.cpp
    )
    add_executable(blackbox_automatic_detector_benchmark
        benchmarks/automatic_detector_benchmark.cpp
    )
    target_compile_features(blackbox_automatic_detector_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_automatic_detector_benchmark PRIVATE BlackBox::Telemetry)
    blackbox_set_project_warnings(blackbox_automatic_detector_benchmark)
endif()

if(BLACKBOX_ENABLE_ANALYSIS)
    target_sources(blackbox_tests PRIVATE
        analysis/contributor_feedback_calibration_tests.cpp
        analysis/contributor_ranker_tests.cpp
        analysis/feedback_calibration_tests.cpp
        analysis/incident_clustering_tests.cpp
        analysis/intelligent_incident_analyzer_tests.cpp
        analysis/personalized_process_analyzer_tests.cpp
        analysis/robust_baseline_tests.cpp
        analysis/similar_incident_evidence_tests.cpp
        analysis/statistical_incident_analyzer_tests.cpp
        analysis/workload_context_tests.cpp
        evaluation/campaign_status_tests.cpp
        evaluation/confidence_calibration_artifact_tests.cpp
        evaluation/diagnostic_evaluation_tests.cpp
        evaluation/diagnostic_evaluation_artifact_tests.cpp
        evaluation/dogfood_corpus_tests.cpp
        evaluation/evaluation_run_transaction_tests.cpp
        evaluation/offline_model_harness_tests.cpp
        evaluation/truth_review_tests.cpp
    )
    target_link_libraries(blackbox_tests PRIVATE
        BlackBox::Analysis
        BlackBox::Evaluation
    )

    add_executable(blackbox_analysis_benchmark
        benchmarks/analysis_benchmark.cpp
    )
    target_include_directories(blackbox_analysis_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(blackbox_analysis_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_analysis_benchmark PRIVATE BlackBox::Analysis)
    if(WIN32)
        target_link_libraries(blackbox_analysis_benchmark PRIVATE psapi)
    endif()
    blackbox_set_project_warnings(blackbox_analysis_benchmark)

    add_executable(blackbox_contributor_ranking_benchmark
        benchmarks/contributor_ranking_benchmark.cpp
    )
    target_compile_features(blackbox_contributor_ranking_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_contributor_ranking_benchmark PRIVATE BlackBox::Analysis)
    blackbox_set_project_warnings(blackbox_contributor_ranking_benchmark)

    add_executable(blackbox_incident_clustering_benchmark
        benchmarks/incident_clustering_benchmark.cpp
    )
    target_compile_features(blackbox_incident_clustering_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_incident_clustering_benchmark PRIVATE BlackBox::Analysis)
    if(WIN32)
        target_link_libraries(blackbox_incident_clustering_benchmark PRIVATE psapi)
    endif()
    blackbox_set_project_warnings(blackbox_incident_clustering_benchmark)

    add_executable(blackbox_context_recognition_benchmark
        benchmarks/context_recognition_benchmark.cpp
    )
    target_include_directories(blackbox_context_recognition_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(blackbox_context_recognition_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_context_recognition_benchmark PRIVATE BlackBox::Analysis)
    blackbox_set_project_warnings(blackbox_context_recognition_benchmark)

    add_executable(blackbox_intelligent_analysis_benchmark
        benchmarks/intelligent_analysis_benchmark.cpp
    )
    target_include_directories(blackbox_intelligent_analysis_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(blackbox_intelligent_analysis_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_intelligent_analysis_benchmark PRIVATE BlackBox::Analysis)
    if(WIN32)
        target_link_libraries(blackbox_intelligent_analysis_benchmark PRIVATE psapi)
    endif()
    blackbox_set_project_warnings(blackbox_intelligent_analysis_benchmark)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(blackbox_tests PRIVATE
        telemetry/linux_system_event_provider_tests.cpp
        telemetry/linux_telemetry_provider_tests.cpp)
    target_link_libraries(blackbox_tests PRIVATE BlackBox::TelemetryLinux)
    if(TARGET PkgConfig::BLACKBOX_DBUS)
        target_compile_definitions(blackbox_tests PRIVATE
            BLACKBOX_TEST_HAS_DBUS=1)
    endif()
    if(BLACKBOX_BUILD_APP)
        add_executable(blackbox_posix_crash_probe
            fixtures/posix_crash_probe.cpp)
        target_compile_features(blackbox_posix_crash_probe PRIVATE cxx_std_23)
        target_link_libraries(blackbox_posix_crash_probe PRIVATE
            BlackBox::PlatformLinux)
        blackbox_set_project_warnings(blackbox_posix_crash_probe)
        target_sources(blackbox_tests PRIVATE
            platform/linux_accessibility_tests.cpp
            platform/linux_background_shell_tests.cpp
            platform/linux_global_hotkey_manager_tests.cpp
            platform/posix_crash_diagnostics_tests.cpp)
        target_link_libraries(blackbox_tests PRIVATE BlackBox::PlatformLinux)
        add_dependencies(blackbox_tests blackbox_posix_crash_probe)
    endif()
endif()

if(APPLE)
    target_sources(blackbox_tests PRIVATE
        telemetry/macos_system_event_provider_tests.cpp
        telemetry/macos_telemetry_provider_tests.cpp)
    target_link_libraries(blackbox_tests PRIVATE BlackBox::TelemetryMacos)

    if(BLACKBOX_BUILD_APP)
        add_executable(blackbox_posix_crash_probe
            fixtures/posix_crash_probe.cpp)
        target_compile_features(blackbox_posix_crash_probe PRIVATE cxx_std_23)
        target_link_libraries(blackbox_posix_crash_probe PRIVATE
            BlackBox::PlatformMacos)
        blackbox_set_project_warnings(blackbox_posix_crash_probe)
        target_sources(blackbox_tests PRIVATE
            platform/macos_background_shell_tests.cpp
            platform/posix_crash_diagnostics_tests.cpp)
        target_link_libraries(blackbox_tests PRIVATE BlackBox::PlatformMacos)
        add_dependencies(blackbox_tests blackbox_posix_crash_probe)
    endif()

    add_executable(blackbox_macos_provider_benchmark
        benchmarks/macos_provider_benchmark.cpp)
    target_compile_features(blackbox_macos_provider_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_macos_provider_benchmark PRIVATE
        BlackBox::TelemetryMacos)
    blackbox_set_project_warnings(blackbox_macos_provider_benchmark)
    add_test(
        NAME macos_native_provider_overhead_contract
        COMMAND blackbox_macos_provider_benchmark 16)
    set_tests_properties(macos_native_provider_overhead_contract
        PROPERTIES LABELS "macos;performance")
endif()

if(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES)
    target_sources(blackbox_tests PRIVATE
        storage/incident_dataset_tests.cpp
        storage/incident_writer_tests.cpp
        storage/sqlite_incident_archive_tests.cpp
        property/corrupt_archive_property_tests.cpp
    )
    target_include_directories(blackbox_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(blackbox_tests PRIVATE BlackBox::Storage)

    if(BLACKBOX_BUILD_APP)
        target_sources(blackbox_tests PRIVATE
            app/archive_maintenance_service_tests.cpp
            app/incident_viewer_service_tests.cpp
            ui/incident_view_model_tests.cpp
            ui/incident_viewer_smoke_tests.cpp
            ui/product_ui_model_tests.cpp
        )
        target_link_libraries(blackbox_tests PRIVATE
            BlackBox::IncidentViewerService
            BlackBox::AppSupport
            BlackBox::UI
            SDL3::SDL3
        )

        add_executable(blackbox_viewer_benchmark
            benchmarks/viewer_benchmark.cpp
        )
        target_include_directories(blackbox_viewer_benchmark PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_features(blackbox_viewer_benchmark PRIVATE cxx_std_23)
        target_link_libraries(blackbox_viewer_benchmark PRIVATE
            BlackBox::Storage
            BlackBox::UI
        )
        if(WIN32)
            target_link_libraries(blackbox_viewer_benchmark PRIVATE psapi)
        endif()
        blackbox_set_project_warnings(blackbox_viewer_benchmark)
    endif()

    add_executable(blackbox_storage_benchmark
        benchmarks/storage_benchmark.cpp
    )
    target_compile_features(blackbox_storage_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_storage_benchmark PRIVATE
        BlackBox::Storage
        BlackBox::Telemetry
    )
    if(WIN32)
        target_link_libraries(blackbox_storage_benchmark PRIVATE psapi)
    endif()
    blackbox_set_project_warnings(blackbox_storage_benchmark)

    add_executable(blackbox_personalized_profile_benchmark
        benchmarks/personalized_profile_benchmark.cpp
    )
    target_include_directories(blackbox_personalized_profile_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(blackbox_personalized_profile_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_personalized_profile_benchmark PRIVATE
        BlackBox::Storage)
    blackbox_set_project_warnings(blackbox_personalized_profile_benchmark)

    add_executable(blackbox_fixture_generator
        fixtures/generate_representative_archive.cpp
    )
    target_include_directories(blackbox_fixture_generator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(blackbox_fixture_generator PRIVATE cxx_std_23)
    target_link_libraries(blackbox_fixture_generator PRIVATE BlackBox::Storage)
    blackbox_set_project_warnings(blackbox_fixture_generator)

    add_test(
        NAME representative_fixture_generation
        COMMAND blackbox_fixture_generator
            ${CMAKE_CURRENT_BINARY_DIR}/representative-incidents.sqlite3
    )
    set_tests_properties(representative_fixture_generation PROPERTIES LABELS "fixture")
endif()
blackbox_set_project_warnings(blackbox_tests)

if(WIN32)
    find_program(BLACKBOX_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(BLACKBOX_POWERSHELL_EXECUTABLE)
        add_test(
            NAME client_qualification_script_contracts
            COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
                -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/client_qualification_contract_tests.ps1
                -SourceRoot ${PROJECT_SOURCE_DIR})
        set_tests_properties(client_qualification_script_contracts
            PROPERTIES LABELS "release;script")
        set(BLACKBOX_WALL_CLOCK_SOAK_CONTRACT_COMMAND
            ${BLACKBOX_POWERSHELL_EXECUTABLE}
            -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/wall_clock_soak_contract_tests.ps1
            -SourceRoot ${PROJECT_SOURCE_DIR})
        if(TARGET blackbox)
            list(APPEND BLACKBOX_WALL_CLOCK_SOAK_CONTRACT_COMMAND
                -ApplicationPath $<TARGET_FILE:blackbox>)
        endif()
        add_test(
            NAME wall_clock_soak_script_contracts
            COMMAND ${BLACKBOX_WALL_CLOCK_SOAK_CONTRACT_COMMAND})
        set_tests_properties(wall_clock_soak_script_contracts
            PROPERTIES LABELS "release;script;soak")
        add_test(
            NAME hosted_ci_attestation_script_contracts
            COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
                -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/hosted_ci_attestation_contract_tests.ps1
                -SourceRoot ${PROJECT_SOURCE_DIR})
        set_tests_properties(hosted_ci_attestation_script_contracts
            PROPERTIES LABELS "release;script;hosted-ci")
        add_test(
            NAME ui_qualification_script_contracts
            COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
                -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui_qualification_contract_tests.ps1
                -SourceRoot ${PROJECT_SOURCE_DIR}
                -TestExecutable $<TARGET_FILE:blackbox_tests>
                -ExpectedSourceRevision ${BLACKBOX_EFFECTIVE_SOURCE_REVISION})
        set_tests_properties(ui_qualification_script_contracts
            PROPERTIES LABELS "release;script;ui")
        add_test(
            NAME v017_release_evidence_script_contracts
            COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
                -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/v017_release_evidence_contract_tests.ps1
                -SourceRoot ${PROJECT_SOURCE_DIR})
        set_tests_properties(v017_release_evidence_script_contracts
            PROPERTIES LABELS "release;script;release-evidence")
        add_test(
            NAME v1_release_evidence_script_contracts
            COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
                -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/v1_release_evidence_contract_tests.ps1
                -SourceRoot ${PROJECT_SOURCE_DIR})
        set_tests_properties(v1_release_evidence_script_contracts
            PROPERTIES LABELS "release;script;release-evidence;v1")
    endif()
endif()

if(WIN32 AND BLACKBOX_BUILD_APP AND BLACKBOX_ENABLE_ANALYSIS AND
   BLACKBOX_ENABLE_STORAGE_DEPENDENCIES AND BLACKBOX_POWERSHELL_EXECUTABLE)
    add_test(
        NAME release_binary_identity_script_contracts
        COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
            -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_binary_identity_contract_tests.ps1
            -SourceRoot ${PROJECT_SOURCE_DIR}
            -BinaryDirectory $<TARGET_FILE_DIR:blackbox>
            -ExpectedVersion ${PROJECT_VERSION}
            -ExpectedSourceRevision ${BLACKBOX_EFFECTIVE_SOURCE_REVISION})
    set_tests_properties(release_binary_identity_script_contracts
        PROPERTIES LABELS "release;script;version;windows")
    add_test(
        NAME consented_quiet_session_packet_script_contracts
        COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
            -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/quiet_session_packet_script_contract_tests.ps1
            -SourceRoot ${PROJECT_SOURCE_DIR}
            -DogfoodTool $<TARGET_FILE:blackbox_dogfood_tool>)
    set_tests_properties(consented_quiet_session_packet_script_contracts
        PROPERTIES LABELS "evaluation;privacy;script")
    add_test(
        NAME consented_incident_session_packet_script_contracts
        COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
            -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/incident_session_packet_script_contract_tests.ps1
            -SourceRoot ${PROJECT_SOURCE_DIR}
            -FixtureGenerator $<TARGET_FILE:blackbox_fixture_generator>
            -DogfoodTool $<TARGET_FILE:blackbox_dogfood_tool>
            -DatasetTool $<TARGET_FILE:blackbox_dataset_tool>)
    set_tests_properties(consented_incident_session_packet_script_contracts
        PROPERTIES LABELS "evaluation;privacy;release;script")
    add_test(
        NAME dogfood_acquisition_cli_contracts
        COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
            -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/dogfood_acquisition_cli_contract_tests.ps1
            -FixtureGenerator $<TARGET_FILE:blackbox_fixture_generator>
            -DogfoodTool $<TARGET_FILE:blackbox_dogfood_tool>
            -DatasetTool $<TARGET_FILE:blackbox_dataset_tool>)
    set_tests_properties(dogfood_acquisition_cli_contracts
        PROPERTIES LABELS "evaluation;release;script")
endif()

if(WIN32 AND BLACKBOX_POWERSHELL_EXECUTABLE)
    foreach(versioned_target IN ITEMS
            blackbox
            blackbox_dataset_tool
            blackbox_dogfood_tool
            blackbox_dogfood_capture
            blackbox_soak_archive_fault)
        if(TARGET ${versioned_target})
            add_test(
                NAME windows_executable_version_resource_${versioned_target}
                COMMAND ${BLACKBOX_POWERSHELL_EXECUTABLE}
                    -NoProfile -NonInteractive -ExecutionPolicy Bypass
                    -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/windows_executable_version_tests.ps1
                    -ExpectedVersion ${PROJECT_VERSION}
                    -ExpectedSourceRevision ${BLACKBOX_EFFECTIVE_SOURCE_REVISION}
                    -BinaryPath $<TARGET_FILE:${versioned_target}>)
            set_tests_properties(
                windows_executable_version_resource_${versioned_target}
                PROPERTIES LABELS "release;version;windows")
        endif()
    endforeach()
endif()

if(WIN32)
    add_executable(blackbox_process_fixture
        fixtures/process_workload.cpp
    )
    target_compile_features(blackbox_process_fixture PRIVATE cxx_std_23)
    blackbox_set_project_warnings(blackbox_process_fixture)

    add_executable(blackbox_dogfood_workload
        fixtures/windows_dogfood_workload.cpp
    )
    target_compile_features(blackbox_dogfood_workload PRIVATE cxx_std_23)
    target_compile_definitions(blackbox_dogfood_workload PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(blackbox_dogfood_workload PRIVATE ws2_32 user32 gdi32 winmm)
    blackbox_set_project_warnings(blackbox_dogfood_workload)

    add_executable(blackbox_crash_probe
        fixtures/windows_crash_probe.cpp)
    target_include_directories(blackbox_crash_probe PRIVATE
        ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_crash_probe PRIVATE cxx_std_23)
    target_link_libraries(blackbox_crash_probe PRIVATE BlackBox::PlatformWindows)
    blackbox_set_project_warnings(blackbox_crash_probe)

    target_sources(blackbox_tests PRIVATE
        platform/windows/windows_background_shell_tests.cpp
        platform/windows/windows_crash_diagnostics_tests.cpp
        platform/windows/windows_global_hotkey_manager_tests.cpp
        telemetry/windows/io_counter_tracker_tests.cpp
        telemetry/windows/windows_counter_conversion_tests.cpp
        telemetry/windows/windows_io_workload_tests.cpp
        telemetry/windows/windows_process_workload_tests.cpp
        telemetry/windows/windows_telemetry_provider_tests.cpp
        telemetry/windows/windows_system_event_provider_tests.cpp
    )
    target_link_libraries(blackbox_tests PRIVATE
        BlackBox::PlatformWindows
        BlackBox::TelemetryWindows
        iphlpapi
        ws2_32
    )
    add_dependencies(blackbox_tests blackbox_process_fixture)
    add_dependencies(blackbox_tests blackbox_crash_probe)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(blackbox_tests PRIVATE
        telemetry/linux_telemetry_provider_tests.cpp)
    target_link_libraries(blackbox_tests PRIVATE BlackBox::TelemetryLinux)

    add_executable(blackbox_linux_provider_benchmark
        benchmarks/linux_provider_benchmark.cpp)
    target_compile_features(blackbox_linux_provider_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_linux_provider_benchmark PRIVATE
        BlackBox::TelemetryLinux)
    blackbox_set_project_warnings(blackbox_linux_provider_benchmark)
    add_test(
        NAME linux_native_provider_overhead_contract
        COMMAND blackbox_linux_provider_benchmark 16)
    set_tests_properties(linux_native_provider_overhead_contract
        PROPERTIES LABELS "linux;performance")
endif()

include(Catch)
catch_discover_tests(blackbox_tests)

add_test(
    NAME architecture_platform_boundaries
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_platform_boundaries.cmake
)
set_tests_properties(architecture_platform_boundaries PROPERTIES LABELS "architecture")

add_test(
    NAME pre_release_direct_v1_contract
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_direct_v1_contract.cmake
)
set_tests_properties(pre_release_direct_v1_contract PROPERTIES LABELS "architecture;format")

add_test(
    NAME release_claims_documentation_contract
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_release_claims.cmake
)
set_tests_properties(release_claims_documentation_contract
    PROPERTIES LABELS "architecture;documentation;release")

add_test(
    NAME repository_hygiene_contract
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_repository_hygiene.cmake
)
set_tests_properties(repository_hygiene_contract
    PROPERTIES LABELS "architecture;hosted-ci;release;repository")

add_test(
    NAME source_revision_contract
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_source_revision_contract.cmake
)
set_tests_properties(source_revision_contract
    PROPERTIES LABELS "architecture;hosted-ci;release;repository;version")

add_test(
    NAME prediction_free_dogfood_cli_contract
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_ROOT=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_prediction_free_dogfood_cli.cmake
)
set_tests_properties(prediction_free_dogfood_cli_contract
    PROPERTIES LABELS "architecture;evaluation;privacy")

add_executable(blackbox_normalization_benchmark
    benchmarks/normalization_benchmark.cpp
)
target_compile_features(blackbox_normalization_benchmark PRIVATE cxx_std_23)
target_link_libraries(blackbox_normalization_benchmark PRIVATE BlackBox::Telemetry)
blackbox_set_project_warnings(blackbox_normalization_benchmark)

add_executable(blackbox_recorder_benchmark
    benchmarks/recorder_benchmark.cpp
)
target_compile_features(blackbox_recorder_benchmark PRIVATE cxx_std_23)
target_link_libraries(blackbox_recorder_benchmark PRIVATE BlackBox::Telemetry)
blackbox_set_project_warnings(blackbox_recorder_benchmark)

add_executable(blackbox_incident_snapshot_benchmark
    benchmarks/incident_snapshot_benchmark.cpp
)
target_compile_features(blackbox_incident_snapshot_benchmark PRIVATE cxx_std_23)
target_link_libraries(blackbox_incident_snapshot_benchmark PRIVATE BlackBox::Telemetry)
blackbox_set_project_warnings(blackbox_incident_snapshot_benchmark)

if(WIN32)
    add_executable(blackbox_windows_provider_benchmark
        benchmarks/windows_provider_benchmark.cpp
    )
    target_compile_features(blackbox_windows_provider_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_windows_provider_benchmark PRIVATE
        BlackBox::TelemetryWindows psapi advapi32
    )
    blackbox_set_project_warnings(blackbox_windows_provider_benchmark)

    add_executable(blackbox_windows_event_provider_benchmark
        benchmarks/windows_event_provider_benchmark.cpp
    )
    target_compile_features(blackbox_windows_event_provider_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_windows_event_provider_benchmark PRIVATE
        BlackBox::TelemetryWindows
    )
    blackbox_set_project_warnings(blackbox_windows_event_provider_benchmark)

    add_executable(blackbox_windows_disk_source_benchmark
        benchmarks/windows_disk_source_benchmark.cpp
    )
    target_compile_features(blackbox_windows_disk_source_benchmark PRIVATE cxx_std_23)
    target_link_libraries(blackbox_windows_disk_source_benchmark PRIVATE
        BlackBox::TelemetryWindows
    )
    blackbox_set_project_warnings(blackbox_windows_disk_source_benchmark)

    add_executable(blackbox_windows_process_benchmark
        benchmarks/windows_process_benchmark.cpp
    )
    target_compile_features(blackbox_windows_process_benchmark PRIVATE cxx_std_23)
    target_compile_definitions(blackbox_windows_process_benchmark PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(blackbox_windows_process_benchmark PRIVATE
        BlackBox::TelemetryWindows pdh psapi
    )
    blackbox_set_project_warnings(blackbox_windows_process_benchmark)
endif()
