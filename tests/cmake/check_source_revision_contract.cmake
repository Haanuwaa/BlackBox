if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

include("${SOURCE_ROOT}/cmake/BlackBoxSourceRevision.cmake")

function(require_source_revision value expected)
    blackbox_is_source_revision("${value}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "Unexpected source-revision validity for '${value}': ${actual}")
    endif()
endfunction()

require_source_revision("auto" TRUE)
require_source_revision("local-uncommitted" TRUE)
require_source_revision("0123456789abcdef0123456789abcdef01234567" TRUE)
require_source_revision("0123456789abcdef0123456789abcdef0123456" FALSE)
require_source_revision("0123456789abcdef0123456789abcdef012345678" FALSE)
require_source_revision("0123456789abcdef0123456789abcdef0123456g" FALSE)
require_source_revision("0123456789ABCDEF0123456789ABCDEF01234567" FALSE)
require_source_revision("" FALSE)

file(READ "${SOURCE_ROOT}/CMakePresets.json" presets)
if(NOT presets MATCHES "\"BLACKBOX_SOURCE_REVISION\"[ \t]*:[ \t]*\"auto\"")
    message(FATAL_ERROR
        "Configure presets must reset cached source identity to auto unless explicitly overridden")
endif()

blackbox_is_commit_revision(
    "0123456789abcdef0123456789abcdef01234567" valid_commit)
if(NOT valid_commit)
    message(FATAL_ERROR "A lowercase 40-character commit revision was rejected")
endif()

foreach(non_commit IN ITEMS auto local-uncommitted)
    blackbox_is_commit_revision("${non_commit}" valid_commit)
    if(valid_commit)
        message(FATAL_ERROR "A symbolic source state was accepted as a commit")
    endif()
endforeach()
