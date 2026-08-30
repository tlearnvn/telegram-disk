# Tăng phiên bản trong tệp VERSION.
#   -DTTD_SOURCE_DIR=...  -DTTD_PART=major|minor|patch

if(NOT TTD_SOURCE_DIR)
    message(FATAL_ERROR "Thiếu TTD_SOURCE_DIR")
endif()
if(NOT TTD_PART)
    set(TTD_PART patch)
endif()

file(READ "${TTD_SOURCE_DIR}/VERSION" _ver)
string(STRIP "${_ver}" _ver)

if(NOT _ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Tệp VERSION không đúng định dạng X.Y.Z: '${_ver}'")
endif()
set(_maj ${CMAKE_MATCH_1})
set(_min ${CMAKE_MATCH_2})
set(_pat ${CMAKE_MATCH_3})

if(TTD_PART STREQUAL "major")
    math(EXPR _maj "${_maj} + 1")
    set(_min 0)
    set(_pat 0)
elseif(TTD_PART STREQUAL "minor")
    math(EXPR _min "${_min} + 1")
    set(_pat 0)
else()
    math(EXPR _pat "${_pat} + 1")
endif()

file(WRITE "${TTD_SOURCE_DIR}/VERSION" "${_maj}.${_min}.${_pat}\n")
message(STATUS "Phiên bản: ${_ver} -> ${_maj}.${_min}.${_pat}")
