if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}/src")
    message(FATAL_ERROR "SOURCE_ROOT must identify the BlackBox source tree")
endif()

function(require_literal relative_path literal description)
    set(path "${SOURCE_ROOT}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Release-claims contract is missing ${relative_path}")
    endif()
    file(READ "${path}" contents)
    string(FIND "${contents}" "${literal}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR
            "Release-claims contract failed: ${description} is absent from ${relative_path}")
    endif()
endfunction()

function(reject_literal relative_path literal description)
    set(path "${SOURCE_ROOT}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Release-claims contract is missing ${relative_path}")
    endif()
    file(READ "${path}" contents)
    string(FIND "${contents}" "${literal}" offset)
    if(NOT offset EQUAL -1)
        message(FATAL_ERROR
            "Release-claims contract failed: stale ${description} remains in ${relative_path}")
    endif()
endfunction()

require_literal("src/telemetry/windows/windows_telemetry_provider.cpp"
    "GPU Engine(*)"
    "implemented Windows GPU provider")
require_literal("docs/TELEMETRY.md"
    "Windows and capability-driven Linux implemented; macOS explicitly unsupported"
    "capability-gated GPU documentation")
require_literal("docs/TELEMETRY.md"
    "DPC/ISR responsiveness"
    "implemented responsiveness documentation")
reject_literal("docs/TELEMETRY.md"
    "No V0.1 choice; ETW/PDH/vendor paths require evaluation"
    "pre-V0.14 GPU research claim")
reject_literal("docs/TELEMETRY.md"
    "Post-V0.1 research"
    "pre-V0.14 GPU status")

require_literal("docs/V0151_QUALIFICATION_STATUS.md"
    "multi-machine evidence collection and one-shot held-out result are not"
    "incomplete real diagnostic evidence disclosure")
require_literal("docs/QUALITY_GATES.md"
    "release_claims_documentation_contract"
    "release-claims gate documentation")
require_literal("ROADMAP.md"
    "- [ ] Collect the qualifying consented, independently annotated, multi-hardware natural and quiet"
    "open consented corpus gate")
require_literal("ROADMAP.md"
    "- [ ] Meet the predeclared gate on the new one-shot held-out split"
    "open held-out quality gate")
require_literal("ROADMAP.md"
    "- [ ] Complete accessibility, DPI, multi-monitor, low-end hardware, battery, and power-mode validation"
    "open physical client matrix gate")
require_literal("docs/PRIVACY_THREAT_MODEL.md"
    "The current package is unsigned and not yet clean-client qualified"
    "unsigned package disclosure")
require_literal("docs/V1_RELEASE_EVIDENCE.md"
    "Neither chain can substitute for the other."
    "independent V1 evidence-chain composition")
require_literal("docs/V0151_COLLECTION_CAMPAIGN.md"
    "interruption, application crash, application hang"
    "complete nine-class diagnostic acquisition matrix")
require_literal("docs/CLIENT_QUALIFICATION.md"
    "BlackBox-1.0.0-windows-x64.zip"
    "exact final client-qualification package")
require_literal("docs/V017_RELEASE_EVIDENCE.md"
    "BlackBox-1.0.0-windows-x64.zip"
    "exact final V0.17-composition package")
require_literal("docs/V1_RELEASE_EVIDENCE.md"
    "BlackBox-1.0.0-windows-x64.zip"
    "exact final V1-composition package")
reject_literal("docs/V017_RELEASE_EVIDENCE.md"
    "BlackBox-0.15.0-windows-x64.zip"
    "prerelease package in final V0.17 evidence instructions")
reject_literal("docs/V1_RELEASE_EVIDENCE.md"
    "BlackBox-0.15.0-windows-x64.zip"
    "prerelease package in final V1 evidence instructions")
reject_literal("docs/V017_RELEASE_EVIDENCE.md"
    "BlackBox-0.17.0-windows-x64.zip"
    "current engineering package in final V0.17 evidence instructions")
reject_literal("docs/V1_RELEASE_EVIDENCE.md"
    "BlackBox-0.17.0-windows-x64.zip"
    "current engineering package in final V1 evidence instructions")
reject_literal("docs/V017_RELEASE_EVIDENCE.md"
    "BlackBox-0.18.0-windows-x64.zip"
    "current engineering package in final V0.17 evidence instructions")
reject_literal("docs/V1_RELEASE_EVIDENCE.md"
    "BlackBox-0.18.0-windows-x64.zip"
    "current engineering package in final V1 evidence instructions")
reject_literal("docs/V017_RELEASE_EVIDENCE.md"
    "BlackBox-0.22.0-windows-x64.zip"
    "current engineering package in final V0.17 evidence instructions")
reject_literal("docs/V1_RELEASE_EVIDENCE.md"
    "BlackBox-0.22.0-windows-x64.zip"
    "current engineering package in final V1 evidence instructions")
require_literal("docs/V1_RELEASE_EVIDENCE.md"
    "rejects reuse of one directory"
    "V1 evidence role-reuse rejection")
require_literal("ROADMAP.md"
    "- [ ] All V0.10-V0.17 acceptance criteria are proven by current evidence"
    "open V1 evidence acceptance gate")
require_literal("ROADMAP.md"
    "- [ ] Set every final product/package/evidence semantic-version surface to exactly `1.0.0`"
    "open exact final product-version gate")
require_literal("docs/RELEASE_READINESS.md"
    "The first public release version is exactly `1.0.0`."
    "exact final product-version policy")
require_literal("src/CMakeLists.txt"
    "function(blackbox_add_windows_version_resource target_name file_description)"
    "central Windows executable-version resource generation")
require_literal("src/app/windows/executable_version.rc.in"
    "VALUE \"ProductVersion\", \"@BLACKBOX_RESOURCE_VERSION_STRING@\\0\""
    "CMake-derived Windows product version")
require_literal("src/app/windows/executable_version.rc.in"
    "VALUE \"Comments\", \"source_revision=@BLACKBOX_RESOURCE_SOURCE_REVISION@\\0\""
    "signed executable source-revision identity")
require_literal("scripts/verify-release-source.ps1"
    "status', '--porcelain=v1', '--untracked-files=all', '--ignored=no'"
    "clean exact-HEAD release-source verification")
require_literal("scripts/verify-v1-release-evidence.ps1"
    "-ExpectedVersion $finalProductVersion"
    "packaged executable final-version verification")
require_literal("scripts/verify-v1-release-evidence.ps1"
    "-ExpectedSourceRevision $SourceRevision"
    "packaged executable source-revision verification")
require_literal("scripts/sign-release.ps1"
    "[string]$ExpectedVersion = '1.0.0'"
    "fail-closed final signing version")
require_literal("scripts/sign-release.ps1"
    "'verify-release-build-binaries.ps1'"
    "pre-signing executable identity verification")
require_literal("scripts/sign-release.ps1"
    "'verify-release-source.ps1'"
    "pre-signing clean source verification")

message(STATUS
    "Release-claims contract verified: implemented evidence documented, external gates disclosed")
