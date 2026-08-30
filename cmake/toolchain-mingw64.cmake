# Toolchain biên dịch chéo cho Windows x64 bằng mingw-w64.
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake ...

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TTD_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "Tiền tố của mingw-w64")

find_program(CMAKE_C_COMPILER   NAMES ${TTD_MINGW_PREFIX}-gcc-posix ${TTD_MINGW_PREFIX}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TTD_MINGW_PREFIX}-g++-posix ${TTD_MINGW_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER  NAMES ${TTD_MINGW_PREFIX}-windres)
find_program(CMAKE_AR           NAMES ${TTD_MINGW_PREFIX}-ar REQUIRED)
find_program(CMAKE_RANLIB       NAMES ${TTD_MINGW_PREFIX}-ranlib REQUIRED)

set(CMAKE_FIND_ROOT_PATH /usr/${TTD_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# mingw-w64 mặc định dùng winpthread; ép dùng bản POSIX threads để std::thread hoạt động.
set(THREADS_PREFER_PTHREAD_FLAG OFF)
set(CMAKE_USE_WIN32_THREADS_INIT 1)
