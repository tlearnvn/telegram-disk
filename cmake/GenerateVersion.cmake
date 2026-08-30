# Sinh tệp version_info.cpp và (tuỳ chọn) tăng số build.
#   -DTTD_SOURCE_DIR=...  -DTTD_GENERATED_DIR=...  -DTTD_DO_BUMP=ON|OFF

if(NOT TTD_SOURCE_DIR OR NOT TTD_GENERATED_DIR)
    message(FATAL_ERROR "Thiếu TTD_SOURCE_DIR hoặc TTD_GENERATED_DIR")
endif()

file(READ "${TTD_SOURCE_DIR}/VERSION" _ver)
string(STRIP "${_ver}" _ver)

set(_build_file "${TTD_SOURCE_DIR}/BUILD_NUMBER")
if(EXISTS "${_build_file}")
    file(READ "${_build_file}" _build)
    string(STRIP "${_build}" _build)
else()
    set(_build 0)
endif()
if(NOT _build MATCHES "^[0-9]+$")
    set(_build 0)
endif()

if(TTD_DO_BUMP)
    math(EXPR _build "${_build} + 1")
    file(WRITE "${_build_file}" "${_build}\n")
endif()

# Thông tin git (không bắt buộc)
set(_git_commit "unknown")
set(_git_branch "unknown")
find_program(_git_exe git)
if(_git_exe)
    execute_process(COMMAND ${_git_exe} rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${TTD_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_commit OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR _git_commit STREQUAL "")
        set(_git_commit "unknown")
    endif()
    execute_process(COMMAND ${_git_exe} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY "${TTD_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_branch OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc2)
    if(NOT _rc2 EQUAL 0 OR _git_branch STREQUAL "")
        set(_git_branch "unknown")
    endif()
endif()

string(TIMESTAMP _build_utc "%Y-%m-%dT%H:%M:%SZ" UTC)
string(TIMESTAMP _build_epoch "%s" UTC)

set(_out "${TTD_GENERATED_DIR}/version_info.cpp")
set(_content "// Tệp này được sinh tự động — đừng sửa tay.\n")
string(APPEND _content "namespace ttd {\nnamespace version {\n")
string(APPEND _content "const char* kAppName = \"Tuấn's Telegram Disk\";\n")
string(APPEND _content "const char* kAppFooter = \"Thiết kế bởi Tuandethuong.\";\n")
string(APPEND _content "const char* kVersion = \"${_ver}\";\n")
string(APPEND _content "const char* kGitCommit = \"${_git_commit}\";\n")
string(APPEND _content "const char* kGitBranch = \"${_git_branch}\";\n")
string(APPEND _content "const char* kBuildTimeUtc = \"${_build_utc}\";\n")
string(APPEND _content "long long kBuildEpoch = ${_build_epoch}LL;\n")
string(APPEND _content "int kBuildNumber = ${_build};\n")
string(APPEND _content "} // namespace version\n} // namespace ttd\n")

set(_old "")
if(EXISTS "${_out}")
    file(READ "${_out}" _old)
endif()
if(NOT _old STREQUAL _content)
    file(WRITE "${_out}" "${_content}")
endif()

message(STATUS "Phiên bản ${_ver} build ${_build} (${_git_commit})")
