if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}/src")
    message(FATAL_ERROR "SOURCE_ROOT must identify the BlackBox source tree")
endif()

set(tool_path "${SOURCE_ROOT}/src/app/dogfood_tool.cpp")
file(READ "${tool_path}" tool_source)

string(FIND "${tool_source}" "int main(" main_offset)
if(main_offset EQUAL -1)
    message(FATAL_ERROR "Prediction-free CLI contract failed: main dispatch is missing")
endif()
string(SUBSTRING "${tool_source}" ${main_offset} -1 main_source)
string(FIND "${main_source}" "IntelligentIncidentAnalyzer" main_analyzer_offset)
if(NOT main_analyzer_offset EQUAL -1)
    message(FATAL_ERROR
        "Prediction-free CLI contract failed: main eagerly constructs analyzer state")
endif()

string(REGEX MATCHALL "current_pipeline_identity\\(\\)" identity_occurrences "${tool_source}")
list(LENGTH identity_occurrences identity_count)
if(NOT identity_count EQUAL 4)
    message(FATAL_ERROR
        "Prediction-free CLI contract failed: pipeline identity must have one definition and exactly three explicit calls")
endif()

string(REGEX MATCHALL
    "analysis::IntelligentIncidentAnalyzer[ ]+analyzer"
    analyzer_constructions "${tool_source}")
list(LENGTH analyzer_constructions analyzer_construction_count)
if(NOT analyzer_construction_count EQUAL 3)
    message(FATAL_ERROR
        "Prediction-free CLI contract failed: analyzer construction must remain restricted to identity, development inspection, and evaluation")
endif()

function(require_identity_call command)
    set(marker "std::string_view{argv[1]} == \"${command}\"")
    string(FIND "${main_source}" "${marker}" command_offset)
    if(command_offset EQUAL -1)
        message(FATAL_ERROR
            "Prediction-free CLI contract failed: '${command}' dispatch is missing")
    endif()
    string(SUBSTRING "${main_source}" ${command_offset} 400 command_window)
    string(FIND "${command_window}" "current_pipeline_identity()" identity_offset)
    if(identity_offset EQUAL -1)
        message(FATAL_ERROR
            "Prediction-free CLI contract failed: '${command}' must explicitly acquire pipeline identity")
    endif()
endfunction()

require_identity_call("fingerprint")
require_identity_call("init")
require_identity_call("init-session")

message(STATUS
    "Prediction-free dogfood CLI verified: eager_analyzers=0 identity_calls=3 explicit_analyzer_paths=3")
