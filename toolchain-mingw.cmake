# Cross-compile toolchain: macOS host -> Windows x86_64 via mingw-w64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /opt/homebrew/Cellar/mingw-w64/14.0.0_1/toolchain-x86_64/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# The engine's Release flags include -march=native/-mtune=native, which is wrong
# when cross-compiling; pin to a generic x86-64 target instead.
set(CMAKE_C_FLAGS_INIT   "-march=x86-64 -mtune=generic")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64 -mtune=generic")
