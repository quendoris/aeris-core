# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED DESTINATION OR DESTINATION STREQUAL "")
    message(FATAL_ERROR "DESTINATION is required")
endif()

file(MAKE_DIRECTORY "${DESTINATION}")

set(_base "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/f1890d9f152c896d250a77557a5751a93d494776/110m_physical")

function(aeris_download_pinned name sha256)
    set(_target "${DESTINATION}/${name}")
    message(STATUS "Fetching pinned Natural Earth resource: ${name}")
    file(DOWNLOAD
        "${_base}/${name}"
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

aeris_download_pinned(
    "ne_110m_land.shp"
    "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de"
)
aeris_download_pinned(
    "ne_110m_land.prj"
    "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8"
)
aeris_download_pinned(
    "ne_110m_land.VERSION.txt"
    "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7"
)

message(STATUS "Pinned AERIS demo world is ready at ${DESTINATION}")
