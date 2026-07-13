# Cross-compile toolchain: macOS host -> Windows x86_64 via mingw-w64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Locate the mingw-w64 sysroot. Overridable with -DMINGW_SYSROOT=...; otherwise try
# Homebrew (macOS, any version) then the distro package layout (Linux / CI).
if(NOT CMAKE_FIND_ROOT_PATH)
    if(DEFINED MINGW_SYSROOT)
        set(CMAKE_FIND_ROOT_PATH "${MINGW_SYSROOT}")
    else()
        file(GLOB _mingw_candidates
            /opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/x86_64-w64-mingw32
            /usr/local/Cellar/mingw-w64/*/toolchain-x86_64/x86_64-w64-mingw32)
        list(APPEND _mingw_candidates /usr/x86_64-w64-mingw32)   # Debian/Ubuntu apt
        foreach(_c ${_mingw_candidates})
            if(EXISTS "${_c}")
                set(CMAKE_FIND_ROOT_PATH "${_c}")
                break()
            endif()
        endforeach()
    endif()
endif()
if(NOT CMAKE_FIND_ROOT_PATH)
    message(FATAL_ERROR "mingw-w64 sysroot not found; pass -DMINGW_SYSROOT=<path>")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# The engine's Release flags include -march=native/-mtune=native, which is wrong
# when cross-compiling; pin to a generic x86-64 target instead.
set(CMAKE_C_FLAGS_INIT   "-march=x86-64 -mtune=generic")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64 -mtune=generic")
