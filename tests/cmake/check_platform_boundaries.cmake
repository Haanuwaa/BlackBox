if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(GLOB_RECURSE CORE_FILES
    "${SOURCE_ROOT}/src/core/*.hpp"
    "${SOURCE_ROOT}/src/core/*.cpp"
)

file(GLOB_RECURSE TELEMETRY_FILES
    "${SOURCE_ROOT}/src/telemetry/*.hpp"
    "${SOURCE_ROOT}/src/telemetry/*.cpp"
)

file(GLOB_RECURSE PLATFORM_FILES
    "${SOURCE_ROOT}/src/platform/*.hpp"
    "${SOURCE_ROOT}/src/platform/*.cpp"
)

file(GLOB_RECURSE STORAGE_FILES
    "${SOURCE_ROOT}/src/storage/*.hpp"
    "${SOURCE_ROOT}/src/storage/*.cpp"
)

file(GLOB_RECURSE ANALYSIS_FILES
    "${SOURCE_ROOT}/src/analysis/*.hpp"
    "${SOURCE_ROOT}/src/analysis/*.cpp"
)

file(GLOB_RECURSE UI_FILES
    "${SOURCE_ROOT}/src/ui/*.hpp"
    "${SOURCE_ROOT}/src/ui/*.cpp"
)

set(FORBIDDEN_DEPENDENCY_INCLUDE
    "#[ \t]*include[ \t]*[<\"](windows\\.h|SDL3/|imgui|implot|sqlite)"
)

foreach(FILE_PATH IN LISTS CORE_FILES)
    file(READ "${FILE_PATH}" CONTENTS)
    if(CONTENTS MATCHES "${FORBIDDEN_DEPENDENCY_INCLUDE}" OR
       CONTENTS MATCHES "#[ \t]*include[ \t]*[<\"](analysis/|app/|platform/|storage/|telemetry/|ui/)")
        message(FATAL_ERROR "Core boundary violation in ${FILE_PATH}")
    endif()
endforeach()

foreach(FILE_PATH IN LISTS TELEMETRY_FILES)
    if(FILE_PATH MATCHES "/(windows|linux|macos)/")
        continue()
    endif()
    file(READ "${FILE_PATH}" CONTENTS)
    if(CONTENTS MATCHES "${FORBIDDEN_DEPENDENCY_INCLUDE}" OR
       CONTENTS MATCHES "#[ \t]*include[ \t]*[<\"](analysis/|app/|storage/|ui/)")
        message(FATAL_ERROR "Platform-independent telemetry boundary violation in ${FILE_PATH}")
    endif()
endforeach()

foreach(FILE_PATH IN LISTS ANALYSIS_FILES)
    file(READ "${FILE_PATH}" CONTENTS)
    if(CONTENTS MATCHES "${FORBIDDEN_DEPENDENCY_INCLUDE}" OR
       CONTENTS MATCHES "#[ \t]*include[ \t]*[<\"](telemetry/|storage/|ui/|platform/|app/)")
        message(FATAL_ERROR "Analysis boundary violation in ${FILE_PATH}")
    endif()
endforeach()

foreach(FILE_PATH IN LISTS PLATFORM_FILES)
    if(FILE_PATH MATCHES "/(windows|linux|macos|posix)/")
        continue()
    endif()
    file(READ "${FILE_PATH}" CONTENTS)
    if(CONTENTS MATCHES "${FORBIDDEN_DEPENDENCY_INCLUDE}")
        message(FATAL_ERROR "Platform-independent platform boundary violation in ${FILE_PATH}")
    endif()
endforeach()

foreach(FILE_PATH IN LISTS STORAGE_FILES)
    file(READ "${FILE_PATH}" CONTENTS)
    if(CONTENTS MATCHES "#[ \t]*include[ \t]*[<\"](analysis/|app/|telemetry/|ui/|platform/|windows\\.h)")
        message(FATAL_ERROR "Storage boundary violation in ${FILE_PATH}")
    endif()
endforeach()

foreach(FILE_PATH IN LISTS UI_FILES)
    file(READ "${FILE_PATH}" CONTENTS)
    if(CONTENTS MATCHES "#[ \t]*include[ \t]*[<\"](analysis/|app/|platform/|storage/|telemetry/|windows\\.h)")
        message(FATAL_ERROR "UI boundary violation in ${FILE_PATH}")
    endif()
endforeach()

message(STATUS "Core, telemetry, platform, storage, and analysis boundaries are clean")
