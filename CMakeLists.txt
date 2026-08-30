cmake_minimum_required(VERSION 3.16)

#
#  Tuấn's Telegram Disk — ổ đĩa lưu trữ dùng Telegram làm nơi chứa dữ liệu
#  Thiết kế bởi Tuandethuong.
#

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TTD_VERSION_RAW)
string(STRIP "${TTD_VERSION_RAW}" TTD_VERSION)

project(tuan_telegram_disk VERSION ${TTD_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Kiểu build" FORCE)
endif()

option(TTD_STATIC "Liên kết tĩnh để tạo bản chạy độc lập (standalone)" ON)
option(TTD_BUILD_TESTS "Biên dịch bộ tự kiểm tra" ON)
option(TTD_AUTO_BUMP "Tự động tăng số build mỗi lần biên dịch" ON)

set(TTD_OUTPUT_NAME "tuan-telegram-disk")

# ---------------------------------------------------------------------------
#  Cờ biên dịch
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W3 /utf-8 /bigobj /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O2)
    endif()
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601 UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
#  Tự động tăng số build + sinh tệp version
# ---------------------------------------------------------------------------
set(TTD_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${TTD_GENERATED_DIR}")

# Sinh lần đầu ngay lúc cấu hình, để tệp luôn tồn tại.
execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DTTD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTTD_GENERATED_DIR=${TTD_GENERATED_DIR}
            -DTTD_DO_BUMP=OFF
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake
    RESULT_VARIABLE _ttd_ver_rc
    OUTPUT_QUIET)

add_custom_target(ttd_version ALL
    COMMAND ${CMAKE_COMMAND}
            -DTTD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTTD_GENERATED_DIR=${TTD_GENERATED_DIR}
            -DTTD_DO_BUMP=${TTD_AUTO_BUMP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake
    BYPRODUCTS "${TTD_GENERATED_DIR}/version_info.cpp"
    COMMENT "Cập nhật thông tin phiên bản (auto-bump)"
    VERBATIM)

set_source_files_properties("${TTD_GENERATED_DIR}/version_info.cpp" PROPERTIES GENERATED TRUE)

# ---------------------------------------------------------------------------
#  Nhúng giao diện web + schema TL vào tệp thực thi
# ---------------------------------------------------------------------------
file(GLOB_RECURSE TTD_WEB_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/*")
file(GLOB_RECURSE TTD_SCHEMA_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schema/*.tl")

add_custom_command(
    OUTPUT "${TTD_GENERATED_DIR}/embedded_assets.cpp"
    COMMAND ${CMAKE_COMMAND}
            -DTTD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTTD_GENERATED_DIR=${TTD_GENERATED_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake
    DEPENDS ${TTD_WEB_FILES} ${TTD_SCHEMA_FILES}
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake"
    COMMENT "Nhúng tài nguyên web và schema TL"
    VERBATIM)

# ---------------------------------------------------------------------------
#  SQLite (bản amalgamation đi kèm — không cần thư viện hệ thống)
# ---------------------------------------------------------------------------
add_library(ttd_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(ttd_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(ttd_sqlite PUBLIC
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
    SQLITE_ENABLE_COLUMN_METADATA)
if(NOT MSVC)
    target_compile_options(ttd_sqlite PRIVATE -w)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(ttd_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
#  Thư viện lõi của ứng dụng
# ---------------------------------------------------------------------------
set(TTD_CORE_SOURCES
    src/common/strutil.cpp
    src/common/json.cpp
    src/common/timeutil.cpp
    src/common/logging.cpp
    src/common/config.cpp
    src/common/net.cpp
    src/common/dns.cpp
    src/common/fsutil.cpp

    src/crypto/hash.cpp
    src/crypto/aes.cpp
    src/crypto/bigint.cpp
    src/crypto/rsa.cpp
    src/crypto/random.cpp
    src/crypto/srp.cpp

    src/compress/inflate.cpp

    src/db/database.cpp
    src/db/sqlite_database.cpp
    src/db/mysql_protocol.cpp
    src/db/mysql_database.cpp
    src/db/schema_sql.cpp

    src/tg/tl_value.cpp
    src/tg/tl_schema.cpp
    src/tg/tl_codec.cpp
    src/tg/mtproto_crypto.cpp
    src/tg/mtproto_transport.cpp
    src/tg/mtproto_session.cpp
    src/tg/dc_config.cpp
    src/tg/tg_account.cpp
    src/tg/tg_login.cpp
    src/tg/account_pool.cpp
    src/tg/backend_local.cpp

    src/storage/chunk_buffer.cpp
    src/storage/storage_engine.cpp
    src/storage/upload_manager.cpp
    src/storage/download_stream.cpp
    src/storage/block_cache.cpp

    src/http/http_types.cpp
    src/http/http_server.cpp
    src/http/mime.cpp
    src/http/api_routes.cpp
    src/http/webdav.cpp
    src/http/assets.cpp

    src/app/app.cpp
    src/app/users.cpp
    src/app/vfs.cpp

    "${TTD_GENERATED_DIR}/version_info.cpp"
    "${TTD_GENERATED_DIR}/embedded_assets.cpp")

add_library(ttd_core STATIC ${TTD_CORE_SOURCES})
add_dependencies(ttd_core ttd_version)
target_include_directories(ttd_core PUBLIC src "${TTD_GENERATED_DIR}")
target_link_libraries(ttd_core PUBLIC ttd_sqlite)

if(WIN32)
    target_link_libraries(ttd_core PUBLIC ws2_32 iphlpapi bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ttd_core PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
#  Tệp thực thi
# ---------------------------------------------------------------------------
add_executable(tuan_telegram_disk src/main.cpp)
set_target_properties(tuan_telegram_disk PROPERTIES OUTPUT_NAME "${TTD_OUTPUT_NAME}")
target_link_libraries(tuan_telegram_disk PRIVATE ttd_core)

if(TTD_STATIC AND NOT MSVC)
    if(WIN32)
        target_link_options(tuan_telegram_disk PRIVATE
            -static -static-libgcc -static-libstdc++ -Wl,--gc-sections)
    else()
        target_link_options(tuan_telegram_disk PRIVATE
            -static-libgcc -static-libstdc++ -Wl,--gc-sections)
        # Bản hoàn toàn tĩnh: dùng trình phân giải DNS nội bộ nên không phụ thuộc NSS.
        if(TTD_FULLY_STATIC)
            target_link_options(tuan_telegram_disk PRIVATE -static)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
#  Bộ tự kiểm tra
# ---------------------------------------------------------------------------
if(TTD_BUILD_TESTS)
    enable_testing()
    add_executable(ttd_selftest tests/selftest.cpp)
    target_link_libraries(ttd_selftest PRIVATE ttd_core)
    if(TTD_STATIC AND NOT MSVC AND WIN32)
        target_link_options(ttd_selftest PRIVATE -static -static-libgcc -static-libstdc++)
    elseif(TTD_STATIC AND NOT MSVC)
        target_link_options(ttd_selftest PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME selftest COMMAND ttd_selftest)
endif()

# ---------------------------------------------------------------------------
#  Mục tiêu phụ trợ
# ---------------------------------------------------------------------------
add_custom_target(bump_minor
    COMMAND ${CMAKE_COMMAND} -DTTD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DTTD_PART=minor
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BumpVersion.cmake
    COMMENT "Tăng phiên bản MINOR")

add_custom_target(bump_major
    COMMAND ${CMAKE_COMMAND} -DTTD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DTTD_PART=major
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BumpVersion.cmake
    COMMENT "Tăng phiên bản MAJOR")

add_custom_target(bump_patch
    COMMAND ${CMAKE_COMMAND} -DTTD_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DTTD_PART=patch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BumpVersion.cmake
    COMMENT "Tăng phiên bản PATCH")

install(TARGETS tuan_telegram_disk RUNTIME DESTINATION bin)

message(STATUS "Tuấn's Telegram Disk ${TTD_VERSION} — static=${TTD_STATIC} tests=${TTD_BUILD_TESTS}")
