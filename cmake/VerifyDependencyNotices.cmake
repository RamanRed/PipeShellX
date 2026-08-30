cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS PIPESHELLX_SOURCE_DIR PIPESHELLX_GOOGLETEST_FALLBACK_VERSION
                           PIPESHELLX_GOOGLETEST_FALLBACK_COMMIT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "VerifyDependencyNotices.cmake requires -D${_required}=...")
    endif()
endforeach()

string(LENGTH "${PIPESHELLX_GOOGLETEST_FALLBACK_COMMIT}" _commit_length)
if(NOT _commit_length EQUAL 40 OR
   NOT PIPESHELLX_GOOGLETEST_FALLBACK_COMMIT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "GoogleTest revision is not a full immutable Git commit")
endif()

set(_notices "${PIPESHELLX_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
if(NOT EXISTS "${_notices}")
    message(FATAL_ERROR "third-party notices are missing: ${_notices}")
endif()
file(READ "${_notices}" _notice_contents)
foreach(_expected IN ITEMS
        "Fallback version: v${PIPESHELLX_GOOGLETEST_FALLBACK_VERSION}"
        "${PIPESHELLX_GOOGLETEST_FALLBACK_COMMIT}"
        "License: BSD 3-Clause")
    string(FIND "${_notice_contents}" "${_expected}" _expected_at)
    if(_expected_at EQUAL -1)
        message(FATAL_ERROR "THIRD_PARTY_NOTICES.md is missing '${_expected}'")
    endif()
endforeach()
