if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}/src")
    message(FATAL_ERROR "SOURCE_ROOT must identify the BlackBox source tree")
endif()

function(require_literal path literal description)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Repository hygiene is missing ${path}")
    endif()
    file(READ "${path}" contents)
    string(FIND "${contents}" "${literal}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR
            "Repository hygiene contract failed: ${description} is absent")
    endif()
endfunction()

set(attributes "${SOURCE_ROOT}/.gitattributes")
set(ignore "${SOURCE_ROOT}/.gitignore")

foreach(rule IN ITEMS
        "* text=auto"
        ".gitattributes text eol=lf"
        "CMakeLists.txt text eol=lf"
        "*.cpp text eol=lf"
        "*.hpp text eol=lf"
        "*.cmake text eol=lf"
        "*.ps1 text eol=lf"
        "*.yml text eol=lf"
        "*.json text eol=lf"
        "*.md text eol=lf"
        "*.ini text eol=lf"
        "*.tsv text eol=lf"
        "*.sha256 text eol=lf"
        "*.zip -text"
        "*.exe -text"
        "*.dll -text"
        "*.dmp -text"
        "*.sqlite3 -text")
    require_literal("${attributes}" "${rule}" "required Git attribute '${rule}'")
endforeach()

foreach(rule IN ITEMS
        "/out/"
        "/_CPack_Packages/"
        "/vcpkg_installed/"
        "/.vs/"
        "/imgui.ini"
        "/BlackBox-*.zip"
        "/BlackBox-*.zip.sha256")
    require_literal("${ignore}" "${rule}" "required generated-artifact ignore '${rule}'")
endforeach()

message(STATUS
    "Repository hygiene verified: deterministic text and protected binary/artifact policy")
