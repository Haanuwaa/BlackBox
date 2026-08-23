function(blackbox_set_project_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus)
        if(BLACKBOX_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(BLACKBOX_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(blackbox_add_windows_version_resource target_name file_description)
    if(NOT WIN32)
        return()
    endif()

    set(version_tweak "${PROJECT_VERSION_TWEAK}")
    if(version_tweak STREQUAL "")
        set(version_tweak 0)
    endif()
    set(BLACKBOX_RESOURCE_VERSION_COMMA
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${version_tweak}")
    set(BLACKBOX_RESOURCE_VERSION_STRING "${PROJECT_VERSION}")
    set(BLACKBOX_RESOURCE_DESCRIPTION "${file_description}")
    set(BLACKBOX_RESOURCE_INTERNAL_NAME "${target_name}")
    set(BLACKBOX_RESOURCE_ORIGINAL_FILENAME "${target_name}.exe")
    set(BLACKBOX_RESOURCE_SOURCE_REVISION "${BLACKBOX_EFFECTIVE_SOURCE_REVISION}")
    set(resource_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/version")
    file(MAKE_DIRECTORY "${resource_directory}")
    set(resource_file "${resource_directory}/${target_name}_version.rc")
    configure_file(
        "${PROJECT_SOURCE_DIR}/src/app/windows/executable_version.rc.in"
        "${resource_file}"
        @ONLY
    )
    target_sources(${target_name} PRIVATE "${resource_file}")
endfunction()

set(BLACKBOX_RUNTIME_INSTALL_DESTINATION ".")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BLACKBOX_RUNTIME_INSTALL_DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

add_library(blackbox_core STATIC
    core/incident.cpp
    core/logger.cpp
)
add_library(BlackBox::Core ALIAS blackbox_core)
target_include_directories(blackbox_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(blackbox_core PUBLIC cxx_std_23)
target_compile_definitions(blackbox_core PUBLIC
    BLACKBOX_VERSION="${PROJECT_VERSION}"
    BLACKBOX_SOURCE_REVISION="${BLACKBOX_EFFECTIVE_SOURCE_REVISION}"
)
blackbox_set_project_warnings(blackbox_core)

if(BLACKBOX_ENABLE_ANALYSIS)
    add_library(blackbox_analysis STATIC
        analysis/contributor_feedback_calibration.cpp
        analysis/contributor_ranker.cpp
        analysis/feedback_calibration.cpp
        analysis/incident_clustering.cpp
        analysis/intelligent_incident_analyzer.cpp
        analysis/personalized_process_analyzer.cpp
        analysis/robust_baseline.cpp
        analysis/similar_incident_evidence.cpp
        analysis/statistical_incident_analyzer.cpp
        analysis/workload_context.cpp
    )
    add_library(BlackBox::Analysis ALIAS blackbox_analysis)
    target_include_directories(blackbox_analysis PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_analysis PUBLIC cxx_std_23)
    target_link_libraries(blackbox_analysis PUBLIC BlackBox::Core)
    blackbox_set_project_warnings(blackbox_analysis)

    # Offline-only corpus/evaluation code. This target is deliberately absent
    # from the desktop application and every recorder/provider target.
    add_library(blackbox_evaluation STATIC
        evaluation/campaign_status.cpp
        evaluation/confidence_calibration_artifact.cpp
        evaluation/diagnostic_evaluation.cpp
        evaluation/diagnostic_evaluation_artifact.cpp
        evaluation/dogfood_corpus.cpp
        evaluation/evaluation_run_transaction.cpp
        evaluation/offline_model_harness.cpp
        evaluation/truth_review.cpp
    )
    add_library(BlackBox::Evaluation ALIAS blackbox_evaluation)
    target_include_directories(blackbox_evaluation PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_evaluation PUBLIC cxx_std_23)
    target_link_libraries(blackbox_evaluation PUBLIC BlackBox::Analysis)
    blackbox_set_project_warnings(blackbox_evaluation)
endif()

add_library(blackbox_telemetry STATIC
    telemetry/collector.cpp
    telemetry/collection_timing.cpp
    telemetry/event_collector.cpp
    telemetry/incident_snapshot_builder.cpp
    telemetry/linux/linux_proc_parser.cpp
    telemetry/mock/mock_telemetry_provider.cpp
    telemetry/normalizer.cpp
    telemetry/process_metadata_cache.cpp
    telemetry/process_normalizer.cpp
    telemetry/provider_contract.cpp
)
if(BLACKBOX_ENABLE_AUTOMATIC_DETECTION)
    target_sources(blackbox_telemetry PRIVATE
        telemetry/automatic_incident_detector.cpp
    )
    target_compile_definitions(blackbox_telemetry PUBLIC
        BLACKBOX_AUTOMATIC_DETECTION_ENABLED=1)
else()
    target_compile_definitions(blackbox_telemetry PUBLIC
        BLACKBOX_AUTOMATIC_DETECTION_ENABLED=0)
endif()
add_library(BlackBox::Telemetry ALIAS blackbox_telemetry)
target_include_directories(blackbox_telemetry PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(blackbox_telemetry PUBLIC cxx_std_23)
target_link_libraries(blackbox_telemetry PUBLIC BlackBox::Core)
blackbox_set_project_warnings(blackbox_telemetry)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(blackbox_telemetry_linux STATIC
        telemetry/linux/linux_process_collector.cpp
        telemetry/linux/linux_telemetry_provider.cpp
    )
    add_library(BlackBox::TelemetryLinux ALIAS blackbox_telemetry_linux)
    target_include_directories(blackbox_telemetry_linux PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_telemetry_linux PUBLIC cxx_std_23)
    target_link_libraries(blackbox_telemetry_linux PUBLIC BlackBox::Telemetry)
    blackbox_set_project_warnings(blackbox_telemetry_linux)
endif()

if(APPLE)
    find_library(BLACKBOX_LIBPROC_LIBRARY proc REQUIRED)
    add_library(blackbox_telemetry_macos STATIC
        telemetry/macos/macos_process_collector.cpp
        telemetry/macos/macos_telemetry_provider.cpp
    )
    add_library(BlackBox::TelemetryMacos ALIAS blackbox_telemetry_macos)
    target_include_directories(blackbox_telemetry_macos PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_telemetry_macos PUBLIC cxx_std_23)
    target_link_libraries(blackbox_telemetry_macos PUBLIC BlackBox::Telemetry)
    target_link_libraries(blackbox_telemetry_macos PRIVATE ${BLACKBOX_LIBPROC_LIBRARY})
    blackbox_set_project_warnings(blackbox_telemetry_macos)
endif()

add_library(blackbox_platform INTERFACE)
add_library(BlackBox::Platform ALIAS blackbox_platform)
target_include_directories(blackbox_platform INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(blackbox_platform INTERFACE cxx_std_23)
target_link_libraries(blackbox_platform INTERFACE BlackBox::Core)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BLACKBOX_BUILD_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BLACKBOX_DBUS REQUIRED IMPORTED_TARGET dbus-1)
    add_library(blackbox_platform_linux STATIC
        platform/linux/linux_background_shell.cpp
        platform/linux/linux_global_hotkey_manager.cpp
    )
    add_library(BlackBox::PlatformLinux ALIAS blackbox_platform_linux)
    target_include_directories(blackbox_platform_linux PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_platform_linux PUBLIC cxx_std_23)
    target_link_libraries(blackbox_platform_linux PUBLIC BlackBox::Platform)
    target_link_libraries(blackbox_platform_linux PRIVATE
        SDL3::SDL3
        PkgConfig::BLACKBOX_DBUS)
    blackbox_set_project_warnings(blackbox_platform_linux)
endif()

if(APPLE AND BLACKBOX_BUILD_APP)
    find_library(BLACKBOX_APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(BLACKBOX_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(BLACKBOX_SERVICE_MANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
    find_library(BLACKBOX_USER_NOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
    add_library(blackbox_platform_macos STATIC
        platform/macos/macos_accessibility.mm
        platform/macos/macos_background_shell.mm
    )
    add_library(BlackBox::PlatformMacos ALIAS blackbox_platform_macos)
    target_include_directories(blackbox_platform_macos PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_platform_macos PUBLIC cxx_std_23)
    target_compile_options(blackbox_platform_macos PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(blackbox_platform_macos PUBLIC BlackBox::Platform)
    target_link_libraries(blackbox_platform_macos PRIVATE
        SDL3::SDL3
        ${BLACKBOX_APPKIT_FRAMEWORK}
        ${BLACKBOX_FOUNDATION_FRAMEWORK}
        ${BLACKBOX_SERVICE_MANAGEMENT_FRAMEWORK}
        ${BLACKBOX_USER_NOTIFICATIONS_FRAMEWORK})
    blackbox_set_project_warnings(blackbox_platform_macos)
endif()

if(WIN32)
    add_library(blackbox_platform_windows STATIC
        platform/windows/windows_background_shell.cpp
        platform/windows/windows_accessibility.cpp
        platform/windows/windows_crash_diagnostics.cpp
        platform/windows/windows_global_hotkey_manager.cpp
    )
    add_library(BlackBox::PlatformWindows ALIAS blackbox_platform_windows)
    target_include_directories(blackbox_platform_windows PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_platform_windows PUBLIC cxx_std_23)
    target_compile_definitions(blackbox_platform_windows PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(blackbox_platform_windows PUBLIC BlackBox::Platform)
    target_link_libraries(blackbox_platform_windows PRIVATE
        user32 shell32 advapi32 dbghelp wtsapi32)
    blackbox_set_project_warnings(blackbox_platform_windows)

    add_library(blackbox_telemetry_windows STATIC
        telemetry/windows/windows_counter_conversion.cpp
        telemetry/windows/windows_process_collector.cpp
        telemetry/windows/windows_system_event_provider.cpp
        telemetry/windows/windows_telemetry_provider.cpp
    )
    add_library(BlackBox::TelemetryWindows ALIAS blackbox_telemetry_windows)
    target_include_directories(blackbox_telemetry_windows PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_telemetry_windows PUBLIC cxx_std_23)
    target_compile_definitions(blackbox_telemetry_windows PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(blackbox_telemetry_windows PUBLIC BlackBox::Telemetry)
    target_link_libraries(blackbox_telemetry_windows PRIVATE
        pdh iphlpapi user32 powrprof cfgmgr32 ole32 uuid wevtapi)
    blackbox_set_project_warnings(blackbox_telemetry_windows)
endif()

if(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES)
    add_library(blackbox_storage_dependencies INTERFACE)
    add_library(BlackBox::StorageDependencies ALIAS blackbox_storage_dependencies)
    target_link_libraries(blackbox_storage_dependencies
        INTERFACE ${BLACKBOX_SQLITE_TARGET})

    add_library(blackbox_storage STATIC
        storage/incident_dataset.cpp
        storage/incident_writer.cpp
        storage/sqlite_incident_archive.cpp
        storage/sqlite_incident_archive_maintenance.cpp
    )
    add_library(BlackBox::Storage ALIAS blackbox_storage)
    target_include_directories(blackbox_storage PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_storage PUBLIC cxx_std_23)
    target_link_libraries(blackbox_storage PUBLIC
        BlackBox::Core
        BlackBox::StorageDependencies
    )
    blackbox_set_project_warnings(blackbox_storage)

    add_executable(blackbox_dataset_tool app/dataset_tool.cpp)
    blackbox_add_windows_version_resource(blackbox_dataset_tool
        "BlackBox Incident Dataset Tool")
    target_include_directories(blackbox_dataset_tool PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_dataset_tool PRIVATE cxx_std_23)
    target_link_libraries(blackbox_dataset_tool PRIVATE BlackBox::Storage)
    blackbox_set_project_warnings(blackbox_dataset_tool)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_property(TARGET blackbox_dataset_tool PROPERTY INSTALL_RPATH
            "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/blackbox")
    endif()
    install(TARGETS blackbox_dataset_tool
        RUNTIME DESTINATION "${BLACKBOX_RUNTIME_INSTALL_DESTINATION}")

    # Development-only wall-clock campaign helper. It operates exclusively on
    # the runner's isolated archive and is intentionally absent from packages.
    add_executable(blackbox_soak_archive_fault app/soak_archive_fault.cpp)
    blackbox_add_windows_version_resource(blackbox_soak_archive_fault
        "BlackBox Soak Archive Fault Probe")
    target_compile_features(blackbox_soak_archive_fault PRIVATE cxx_std_23)
    target_link_libraries(blackbox_soak_archive_fault PRIVATE
        BlackBox::StorageDependencies)
    blackbox_set_project_warnings(blackbox_soak_archive_fault)

    if(BLACKBOX_ENABLE_ANALYSIS)
        add_executable(blackbox_dogfood_tool app/dogfood_tool.cpp)
        blackbox_add_windows_version_resource(blackbox_dogfood_tool
            "BlackBox Dogfood Evaluation Tool")
        target_include_directories(blackbox_dogfood_tool PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_features(blackbox_dogfood_tool PRIVATE cxx_std_23)
        target_link_libraries(blackbox_dogfood_tool PRIVATE
            BlackBox::Evaluation
            BlackBox::Storage
        )
        blackbox_set_project_warnings(blackbox_dogfood_tool)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_property(TARGET blackbox_dogfood_tool PROPERTY INSTALL_RPATH
                "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/blackbox")
        endif()
        install(TARGETS blackbox_dogfood_tool
            RUNTIME DESTINATION "${BLACKBOX_RUNTIME_INSTALL_DESTINATION}")

        add_executable(blackbox_offline_ml_tool app/offline_ml_tool.cpp)
        blackbox_add_windows_version_resource(blackbox_offline_ml_tool
            "BlackBox Offline Model Comparison Tool")
        target_include_directories(blackbox_offline_ml_tool PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_features(blackbox_offline_ml_tool PRIVATE cxx_std_23)
        target_link_libraries(blackbox_offline_ml_tool PRIVATE
            BlackBox::Evaluation
            BlackBox::Storage
        )
        blackbox_set_project_warnings(blackbox_offline_ml_tool)
    endif()

    if(WIN32 AND BLACKBOX_ENABLE_AUTOMATIC_DETECTION)
        add_executable(blackbox_dogfood_capture
            app/windows/dogfood_capture.cpp)
        blackbox_add_windows_version_resource(blackbox_dogfood_capture
            "BlackBox Dogfood Capture Tool")
        target_include_directories(blackbox_dogfood_capture PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_features(blackbox_dogfood_capture PRIVATE cxx_std_23)
        target_compile_definitions(blackbox_dogfood_capture PRIVATE
            _WIN32_WINNT=0x0A00)
        target_link_libraries(blackbox_dogfood_capture PRIVATE
            BlackBox::Storage
            BlackBox::TelemetryWindows
        )
        blackbox_set_project_warnings(blackbox_dogfood_capture)
    endif()
endif()

add_library(blackbox_app_support STATIC
    app/product_settings.cpp
    app/recorder_settings.cpp
    app/support_bundle.cpp
    app/wall_clock_report.cpp
)
add_library(BlackBox::AppSupport ALIAS blackbox_app_support)
target_include_directories(blackbox_app_support PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(blackbox_app_support PUBLIC cxx_std_23)
target_link_libraries(blackbox_app_support PUBLIC BlackBox::Telemetry)
blackbox_set_project_warnings(blackbox_app_support)

if(BLACKBOX_BUILD_APP)
    add_library(blackbox_ui STATIC
        ui/dashboard.cpp
        ui/dashboard_settings.cpp
        ui/incident_viewer.cpp
        ui/product_ui_model.cpp
    )
    add_library(BlackBox::UI ALIAS blackbox_ui)
    target_include_directories(blackbox_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox_ui PUBLIC cxx_std_23)
    target_link_libraries(blackbox_ui PUBLIC BlackBox::Core imgui::imgui implot::implot)
    blackbox_set_project_warnings(blackbox_ui)

    if(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES)
        add_library(blackbox_incident_viewer_service STATIC
            app/archive_maintenance_service.cpp
            app/incident_viewer_service.cpp
        )
        add_library(BlackBox::IncidentViewerService ALIAS
            blackbox_incident_viewer_service)
        target_include_directories(blackbox_incident_viewer_service
            PUBLIC ${PROJECT_SOURCE_DIR}/src)
        target_compile_features(blackbox_incident_viewer_service PUBLIC cxx_std_23)
        target_link_libraries(blackbox_incident_viewer_service PUBLIC
            BlackBox::Storage
            BlackBox::UI
        )
        if(BLACKBOX_ENABLE_ANALYSIS)
            target_link_libraries(blackbox_incident_viewer_service PUBLIC
                BlackBox::Analysis)
            target_compile_definitions(blackbox_incident_viewer_service
                PUBLIC BLACKBOX_ANALYSIS_ENABLED=1)
        else()
            target_compile_definitions(blackbox_incident_viewer_service
                PUBLIC BLACKBOX_ANALYSIS_ENABLED=0)
        endif()
        blackbox_set_project_warnings(blackbox_incident_viewer_service)
    endif()

    add_executable(blackbox
        app/application.cpp
        app/application_lifecycle.cpp
        app/main.cpp
    )
    blackbox_add_windows_version_resource(blackbox
        "BlackBox Computer Flight Recorder")
    target_include_directories(blackbox PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(blackbox PRIVATE cxx_std_23)
    target_link_libraries(blackbox PRIVATE
        BlackBox::Core
        BlackBox::AppSupport
        BlackBox::Telemetry
        BlackBox::Platform
        BlackBox::UI
        SDL3::SDL3
        imgui::imgui
        implot::implot
    )
    if(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES)
        target_link_libraries(blackbox PRIVATE
            BlackBox::Storage
            BlackBox::IncidentViewerService
        )
        target_compile_definitions(blackbox PRIVATE BLACKBOX_STORAGE_ENABLED=1)
    else()
        target_compile_definitions(blackbox PRIVATE BLACKBOX_STORAGE_ENABLED=0)
    endif()
    if(BLACKBOX_ENABLE_ANALYSIS)
        target_link_libraries(blackbox PRIVATE BlackBox::Analysis)
        target_compile_definitions(blackbox PRIVATE BLACKBOX_ANALYSIS_ENABLED=1)
    else()
        target_compile_definitions(blackbox PRIVATE BLACKBOX_ANALYSIS_ENABLED=0)
    endif()
    blackbox_set_project_warnings(blackbox)
    blackbox_copy_address_sanitizer_runtime(blackbox)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_property(TARGET blackbox PROPERTY INSTALL_RPATH
            "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/blackbox")
        install(TARGETS blackbox
            RUNTIME_DEPENDENCY_SET blackbox_linux_runtime
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        install(RUNTIME_DEPENDENCY_SET blackbox_linux_runtime
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/blackbox"
            PRE_EXCLUDE_REGEXES
                "^ld-linux.*"
                "^lib(c|dl|gcc_s|m|pthread|rt|stdc\\+\\+)\\.so.*"
            POST_EXCLUDE_REGEXES
                "^/lib/.*"
                "^/lib64/.*"
                "^/usr/lib/.*")
    elseif(APPLE)
        install(TARGETS blackbox BUNDLE DESTINATION . RUNTIME DESTINATION .)
    else()
        install(TARGETS blackbox RUNTIME DESTINATION .)
    endif()
    if(WIN32)
        install(FILES $<TARGET_RUNTIME_DLLS:blackbox> DESTINATION .)
        if(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES)
            find_file(BLACKBOX_SQLITE_RUNTIME
                NAMES sqlite3.dll
                PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
                NO_DEFAULT_PATH
            )
            if(BLACKBOX_SQLITE_RUNTIME)
                install(FILES "${BLACKBOX_SQLITE_RUNTIME}" DESTINATION .)
            endif()
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(FILES
            ${PROJECT_SOURCE_DIR}/README.md
            ${PROJECT_SOURCE_DIR}/ARCHITECTURE.md
            ${PROJECT_SOURCE_DIR}/ROADMAP.md
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/blackbox")
        install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/blackbox")
        install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/io.github.Haanuwaa.BlackBox.desktop
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
        install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/io.github.Haanuwaa.BlackBox.svg
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps")
    else()
        install(FILES
            ${PROJECT_SOURCE_DIR}/README.md
            ${PROJECT_SOURCE_DIR}/ARCHITECTURE.md
            ${PROJECT_SOURCE_DIR}/ROADMAP.md
            DESTINATION docs)
        install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/ DESTINATION docs)
    endif()

    if(WIN32)
        target_sources(blackbox PRIVATE app/windows/blackbox.manifest)
        target_link_libraries(blackbox PRIVATE
            BlackBox::TelemetryWindows
            BlackBox::PlatformWindows
        )
        set_property(TARGET blackbox PROPERTY WIN32_EXECUTABLE TRUE)
        target_compile_definitions(blackbox PRIVATE BLACKBOX_PLATFORM_NAME="Windows")
    elseif(APPLE)
        target_link_libraries(blackbox PRIVATE
            BlackBox::TelemetryMacos
            BlackBox::PlatformMacos)
        set_target_properties(blackbox PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME "BlackBox"
            MACOSX_BUNDLE_BUNDLE_NAME "BlackBox"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.Haanuwaa.BlackBox"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_INFO_PLIST
                "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in")
        target_compile_definitions(blackbox PRIVATE BLACKBOX_PLATFORM_NAME="macOS")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(blackbox PRIVATE
            BlackBox::TelemetryLinux
            BlackBox::PlatformLinux)
        target_compile_definitions(blackbox PRIVATE BLACKBOX_PLATFORM_NAME="Linux")
    elseif(UNIX)
        target_compile_definitions(blackbox PRIVATE BLACKBOX_PLATFORM_NAME="Unix")
    else()
        target_compile_definitions(blackbox PRIVATE BLACKBOX_PLATFORM_NAME="Unknown")
    endif()
endif()
