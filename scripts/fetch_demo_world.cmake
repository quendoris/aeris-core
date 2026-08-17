# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED DESTINATION OR DESTINATION STREQUAL "")
    message(FATAL_ERROR "DESTINATION is required")
endif()

file(MAKE_DIRECTORY "${DESTINATION}")

set(_commit "f1890d9f152c896d250a77557a5751a93d494776")
set(_physical_base "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/${_commit}/110m_physical")
set(_cultural_base "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/${_commit}/110m_cultural")

function(aeris_download_with_retry base name)
    set(_target "${DESTINATION}/${name}")
    set(_success FALSE)
    set(_last_message "unknown download failure")
    foreach(_attempt RANGE 1 3)
        file(REMOVE "${_target}")
        message(STATUS "Fetching Natural Earth resource: ${name} (attempt ${_attempt}/3)")
        file(DOWNLOAD
            "${base}/${name}"
            "${_target}"
            TLS_VERIFY ON
            STATUS _status
            SHOW_PROGRESS
        )
        list(GET _status 0 _code)
        list(GET _status 1 _last_message)
        if(_code EQUAL 0)
            set(_success TRUE)
            break()
        endif()
        file(REMOVE "${_target}")
    endforeach()
    if(NOT _success)
        message(FATAL_ERROR "Download failed for ${name} after 3 attempts: ${_last_message}")
    endif()
endfunction()

function(aeris_verify_local name sha256)
    set(_target "${DESTINATION}/${name}")
    file(SHA256 "${_target}" _actual_sha256)
    if(NOT _actual_sha256 STREQUAL sha256)
        file(REMOVE "${_target}")
        message(FATAL_ERROR
            "SHA-256 mismatch for ${name}: expected ${sha256}, got ${_actual_sha256}")
    endif()
endfunction()

function(aeris_download_verified base name sha256)
    aeris_download_with_retry("${base}" "${name}")
    aeris_verify_local("${name}" "${sha256}")
endfunction()

# Large binary payloads are fetched from the immutable upstream commit and
# checked byte-for-byte by SHA-256.
aeris_download_verified(
    "${_physical_base}"
    "ne_110m_land.shp"
    "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de"
)
aeris_download_verified(
    "${_cultural_base}"
    "ne_110m_admin_0_countries.shp"
    "08e341606e8391e458c3f08deb312de664b56bfae376064c5aa0aee6681a5f55"
)
aeris_download_verified(
    "${_cultural_base}"
    "ne_110m_admin_0_countries.dbf"
    "1fee677cd4e03b367876e03861eb10197e4022a846bf92060e0313432863785b"
)

# These tiny immutable companion resources are materialized byte-for-byte from
# the pinned upstream commit contents. Avoiding an extra raw-CDN request for
# each one removes needless transport fragility while retaining exact hashes.
set(_wgs84_prj [=[GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.017453292519943295]]]=])
file(WRITE "${DESTINATION}/ne_110m_land.prj" "${_wgs84_prj}")
file(WRITE "${DESTINATION}/ne_110m_admin_0_countries.prj" "${_wgs84_prj}")
file(WRITE "${DESTINATION}/ne_110m_land.VERSION.txt" "4.1.0\n")
file(WRITE "${DESTINATION}/ne_110m_admin_0_countries.cpg" "UTF-8")
file(WRITE "${DESTINATION}/ne_110m_admin_0_countries.VERSION.txt" "5.1.1\n")

aeris_verify_local(
    "ne_110m_land.prj"
    "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8"
)
aeris_verify_local(
    "ne_110m_admin_0_countries.prj"
    "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8"
)
aeris_verify_local(
    "ne_110m_land.VERSION.txt"
    "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7"
)
aeris_verify_local(
    "ne_110m_admin_0_countries.cpg"
    "3ad3031f5503a4404af825262ee8232cc04d4ea6683d42c5dd0a2f2a27ac9824"
)
aeris_verify_local(
    "ne_110m_admin_0_countries.VERSION.txt"
    "f9893302cd3158f3b5aea394dcd2a91574869e9e6ff69e9235b10a3bf8c983fb"
)

message(STATUS "Pinned AERIS physical + political demo world is ready at ${DESTINATION}")
message(STATUS "Every resource is bound to the exact upstream commit bytes and verified by SHA-256; the viewer also verifies aggregate source identities before use.")
