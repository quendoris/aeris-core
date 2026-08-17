# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED DESTINATION OR DESTINATION STREQUAL "")
    message(FATAL_ERROR "DESTINATION is required")
endif()

file(MAKE_DIRECTORY "${DESTINATION}")

set(_commit "f1890d9f152c896d250a77557a5751a93d494776")
set(_physical_base "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/${_commit}/110m_physical")
set(_cultural_base "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/${_commit}/110m_cultural")

function(aeris_download_verified base name sha256)
    set(_target "${DESTINATION}/${name}")
    message(STATUS "Fetching pinned Natural Earth resource: ${name}")
    file(DOWNLOAD
        "${base}/${name}"
        "${_target}"
        EXPECTED_HASH "SHA256=${sha256}"
        TLS_VERIFY ON
        STATUS _status
        SHOW_PROGRESS
    )
    list(GET _status 0 _code)
    list(GET _status 1 _message)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_target}")
        message(FATAL_ERROR "Download failed for ${name}: ${_message}")
    endif()
endfunction()

function(aeris_download_commit_pinned base name)
    set(_target "${DESTINATION}/${name}")
    message(STATUS "Fetching commit-pinned Natural Earth resource: ${name}")
    file(DOWNLOAD
        "${base}/${name}"
        "${_target}"
        TLS_VERIFY ON
        STATUS _status
        SHOW_PROGRESS
    )
    list(GET _status 0 _code)
    list(GET _status 1 _message)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_target}")
        message(FATAL_ERROR "Download failed for ${name}: ${_message}")
    endif()
endfunction()

aeris_download_verified(
    "${_physical_base}"
    "ne_110m_land.shp"
    "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de"
)
aeris_download_verified(
    "${_physical_base}"
    "ne_110m_land.prj"
    "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8"
)
aeris_download_verified(
    "${_physical_base}"
    "ne_110m_land.VERSION.txt"
    "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7"
)

foreach(_admin0_file IN ITEMS
    ne_110m_admin_0_countries.shp
    ne_110m_admin_0_countries.dbf
    ne_110m_admin_0_countries.cpg
    ne_110m_admin_0_countries.prj
    ne_110m_admin_0_countries.VERSION.txt
)
    aeris_download_commit_pinned("${_cultural_base}" "${_admin0_file}")
endforeach()

message(STATUS "Pinned AERIS physical + political demo world is ready at ${DESTINATION}")
message(STATUS "The viewer independently verifies the admin0 aggregate content SHA-256 before use.")
