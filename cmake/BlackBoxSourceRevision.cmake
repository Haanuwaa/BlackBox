function(blackbox_is_commit_revision value output_variable)
    string(LENGTH "${value}" blackbox_revision_length)
    if(blackbox_revision_length EQUAL 40 AND value MATCHES "^[0-9a-f]+$")
        set(${output_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(blackbox_is_source_revision value output_variable)
    blackbox_is_commit_revision("${value}" blackbox_is_commit)
    if(value STREQUAL "auto" OR
       value STREQUAL "local-uncommitted" OR
       blackbox_is_commit)
        set(${output_variable} TRUE PARENT_SCOPE)
    else()
        set(${output_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()
