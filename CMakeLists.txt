cmake_minimum_required(VERSION 3.25)

project(
    BlackBox
    VERSION 0.20.0
    DESCRIPTION "A lightweight native computer flight recorder"
    LANGUAGES CXX
)

if(APPLE)
    enable_language(OBJCXX)
endif()

include(CTest)
include(GNUInstallDirs)
include(cmake/BlackBoxSourceRevision.cmake)

option(BLACKBOX_BUILD_APP "Build the BlackBox desktop application" ON)
option(BLACKBOX_BUILD_TESTS "Build BlackBox unit tests" ${BUILD_TESTING})
option(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES "Build SQLite incident storage" ON)
option(BLACKBOX_ENABLE_ANALYSIS "Build optional incident analysis" ON)
option(BLACKBOX_ENABLE_AUTOMATIC_DETECTION "Build optional automatic incident detection" ON)
option(BLACKBOX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(BLACKBOX_ENABLE_ADDRESS_SANITIZER "Instrument BlackBox targets with AddressSanitizer" OFF)
option(BLACKBOX_ENABLE_UNDEFINED_SANITIZER "Instrument BlackBox targets with UndefinedBehaviorSanitizer" OFF)
option(BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS "Run MSVC native code analysis while compiling" OFF)
option(BLACKBOX_ENABLE_COVERAGE "Instrument BlackBox targets for compiler-native coverage" OFF)
option(BLACKBOX_BUILD_FUZZERS "Build bounded libFuzzer entry points" OFF)
set(BLACKBOX_LINUX_PACKAGE_GENERATORS "TGZ" CACHE STRING
    "Semicolon-separated Linux engineering package generators (TGZ, DEB, RPM)")
set(BLACKBOX_SOURCE_REVISION "auto" CACHE STRING
    "Source identity embedded in Windows executables: auto, local-uncommitted, or 40 lowercase hex")

blackbox_is_source_revision("${BLACKBOX_SOURCE_REVISION}" blackbox_source_revision_valid)
if(NOT blackbox_source_revision_valid)
    message(FATAL_ERROR
        "BLACKBOX_SOURCE_REVISION must be auto, local-uncommitted, or 40 lowercase hex")
endif()
set(BLACKBOX_EFFECTIVE_SOURCE_REVISION "${BLACKBOX_SOURCE_REVISION}")
if(BLACKBOX_SOURCE_REVISION STREQUAL "auto")
    set(BLACKBOX_EFFECTIVE_SOURCE_REVISION "local-uncommitted")
    find_package(Git QUIET)
    if(NOT GIT_FOUND AND WIN32)
        find_program(GIT_EXECUTABLE NAMES git.exe
            HINTS "C:/Program Files/Git/cmd" "C:/Program Files/Git/bin")
    endif()
    if(GIT_EXECUTABLE AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}"
                rev-parse --verify HEAD
            RESULT_VARIABLE blackbox_git_head_result
            OUTPUT_VARIABLE blackbox_git_head
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}"
                status --porcelain=v1 --untracked-files=all --ignored=no
            RESULT_VARIABLE blackbox_git_status_result
            OUTPUT_VARIABLE blackbox_git_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        blackbox_is_commit_revision("${blackbox_git_head}" blackbox_git_head_valid)
        if(blackbox_git_head_result EQUAL 0 AND
           blackbox_git_status_result EQUAL 0 AND
           blackbox_git_head_valid AND
           blackbox_git_status STREQUAL "")
            set(BLACKBOX_EFFECTIVE_SOURCE_REVISION "${blackbox_git_head}")
        endif()
    endif()
endif()
message(STATUS "BlackBox source revision: ${BLACKBOX_EFFECTIVE_SOURCE_REVISION}")

if(BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS AND
   (BLACKBOX_ENABLE_ADDRESS_SANITIZER OR BLACKBOX_ENABLE_UNDEFINED_SANITIZER OR
    BLACKBOX_ENABLE_COVERAGE OR BLACKBOX_BUILD_FUZZERS))
    message(FATAL_ERROR "MSVC code analysis must run in its own build graph")
endif()
if(BLACKBOX_ENABLE_COVERAGE AND
   (BLACKBOX_ENABLE_ADDRESS_SANITIZER OR BLACKBOX_ENABLE_UNDEFINED_SANITIZER OR
    BLACKBOX_BUILD_FUZZERS))
    message(FATAL_ERROR "Coverage must run in its own build graph")
endif()

if(BLACKBOX_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/INCREMENTAL:NO)
        get_filename_component(BLACKBOX_MSVC_COMPILER_DIRECTORY
            "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_file(BLACKBOX_MSVC_ASAN_RUNTIME
            NAMES clang_rt.asan_dynamic-x86_64.dll
            PATHS "${BLACKBOX_MSVC_COMPILER_DIRECTORY}"
            NO_DEFAULT_PATH
            REQUIRED)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    else()
        message(FATAL_ERROR "AddressSanitizer is unsupported by this compiler")
    endif()
endif()

function(blackbox_copy_address_sanitizer_runtime target)
    if(BLACKBOX_ENABLE_ADDRESS_SANITIZER AND MSVC)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BLACKBOX_MSVC_ASAN_RUNTIME}"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying the MSVC AddressSanitizer runtime for ${target}")
    endif()
endfunction()

if(BLACKBOX_ENABLE_UNDEFINED_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
    else()
        message(FATAL_ERROR "UndefinedBehaviorSanitizer requires Clang or GCC mode")
    endif()
endif()

if(BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS)
    if(MSVC)
        add_compile_options(/analyze /analyze:external-)
    else()
        message(FATAL_ERROR "BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS requires MSVC")
    endif()
endif()

if(BLACKBOX_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        add_compile_options(--coverage -O0 -g)
        add_link_options(--coverage)
    else()
        message(FATAL_ERROR "Coverage instrumentation requires Clang or GCC mode")
    endif()
endif()

if(BLACKBOX_BUILD_FUZZERS AND NOT
   (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC))
    message(FATAL_ERROR "Native fuzz targets require Clang libFuzzer mode")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(BLACKBOX_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    find_package(implot CONFIG REQUIRED)
endif()

if(BLACKBOX_ENABLE_STORAGE_DEPENDENCIES)
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        set(BLACKBOX_SQLITE_TARGET SQLite3::SQLite3)
    elseif(TARGET SQLite::SQLite3)
        set(BLACKBOX_SQLITE_TARGET SQLite::SQLite3)
    else()
        message(FATAL_ERROR
            "FindSQLite3 succeeded without a supported imported target")
    endif()
    message(STATUS "BlackBox SQLite target: ${BLACKBOX_SQLITE_TARGET}")
endif()

add_subdirectory(src)

if(BLACKBOX_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

set(CPACK_PACKAGE_NAME "BlackBox")
set(CPACK_PACKAGE_VENDOR "BlackBox")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
if(WIN32)
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Windows computer flight recorder")
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "BlackBox-${PROJECT_VERSION}-windows-x64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Unsupported Linux engineering preview of the BlackBox computer flight recorder")
    set(CPACK_GENERATOR "${BLACKBOX_LINUX_PACKAGE_GENERATORS}")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" BLACKBOX_PACKAGE_PROCESSOR)
    set(CPACK_PACKAGE_FILE_NAME
        "BlackBox-${PROJECT_VERSION}-linux-${BLACKBOX_PACKAGE_PROCESSOR}-engineering-preview")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Hanuwa <hanuwa@aol.com>")
    set(CPACK_DEBIAN_PACKAGE_NAME "blackbox")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_RPM_PACKAGE_LICENSE "LicenseRef-Proprietary")
    set(CPACK_RPM_PACKAGE_NAME "blackbox")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
else()
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Unsupported engineering build of the BlackBox computer flight recorder")
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME
        "BlackBox-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-engineering-preview")
endif()
include(CPack)
