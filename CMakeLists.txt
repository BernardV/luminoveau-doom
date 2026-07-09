cmake_minimum_required(VERSION 3.20)

# Doom needs the SDL3 callback entry point that lumi_main.cpp provides.
# Must be set before the engine subdirectory is added.
set(LUMINOVEAU_USE_CALLBACKS ON CACHE BOOL "" FORCE)

project(lumi_doom LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

set(DOOM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/doom/linuxdoom-1.10")
set(LUMI_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lumi")

# ── Engine ──────────────────────────────────────────────────────────────────
add_subdirectory("${LUMI_DIR}" "${CMAKE_BINARY_DIR}/luminoveau")

# ── Doom core (original C sources, minus the platform files we replace) ───────
file(GLOB DOOM_SOURCES "${DOOM_SRC_DIR}/*.c")
list(FILTER DOOM_SOURCES EXCLUDE REGEX
    "/(i_main|i_video|i_system|i_sound|i_net)\\.c$")

add_library(doomcore STATIC ${DOOM_SOURCES})
target_include_directories(doomcore PUBLIC "${DOOM_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(doomcore PUBLIC NORMALUNIX)

# linuxdoom-1.10 is K&R-ish C89 with tentative-definition globals. Modern clang
# defaults (fno-common, implicit-decl-as-error) reject it, so relax.
#   Native: -fcommon merges the tentative defs.
#   Wasm:   emcc has no common symbols, so use -fno-common and let wasm-ld merge
#           duplicates (--allow-multiple-definition on the final link, below).
if(NOT MSVC)
    if(EMSCRIPTEN)
        set(DOOM_C_FLAGS -std=gnu89 -fno-common -w
            -Wno-implicit-function-declaration -Wno-implicit-int
            -Wno-int-conversion -Wno-return-type)
    else()
        set(DOOM_C_FLAGS -std=gnu89 -fcommon -w
            -Wno-implicit-function-declaration -Wno-implicit-int
            -Wno-int-conversion -Wno-return-type)
    endif()
    target_compile_options(doomcore PRIVATE ${DOOM_C_FLAGS})
endif()

# ── Game executable: our platform layer + the Lumi host ───────────────────────
add_executable(doom
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/i_lumi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dg_music.c")

target_include_directories(doom PRIVATE
    "${DOOM_SRC_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(doom PRIVATE NORMALUNIX)
target_link_libraries(doom PRIVATE doomcore luminoveau)

# On macOS a bare CLI executable never becomes a proper foreground GUI app, so
# the window shows and takes the mouse but the keyboard stays with Terminal.
# Packaging as a .app bundle (launched via `open`) fixes keyboard focus.
if(APPLE AND NOT EMSCRIPTEN)
    set_target_properties(doom PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "DOOM"
        MACOSX_BUNDLE_GUI_IDENTIFIER "net.luminoveau.doom"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.10"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.10")
endif()

if(NOT MSVC)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/i_lumi.c"
        PROPERTIES COMPILE_OPTIONS "${DOOM_C_FLAGS}")
endif()

# Wasm-specific link options.
if(EMSCRIPTEN)
    target_link_options(doom PRIVATE
        # Merge linuxdoom's duplicate tentative definitions (the -fcommon
        # substitute — see the doomcore flags above).
        "-Wl,--allow-multiple-definition"
        # Patch TextDecoder.decode + GPUQueue writes + auto-resume audio before
        # the module runs. See web_prejs.js.
        "--pre-js" "${CMAKE_CURRENT_SOURCE_DIR}/src/web_prejs.js"
        # miniaudio's Web Audio glue reads Module.HEAPF32, but emscripten 6.0.2
        # doesn't attach heap views to Module by default -> "reading 'buffer' of
        # undefined" in onaudioprocess. Export them (kept updated across growth).
        "-sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPF64,HEAP8,HEAPU8,HEAP16,HEAPU16,HEAP32,HEAPU32")
endif()

# ── Assets / packaging ────────────────────────────────────────────────────────
if(EMSCRIPTEN)
    # Preloads assets/ (containing doom1.wad) into the virtual FS at /assets and
    # emits doom.html + wasm using the engine's web helper.
    # Auto-start shell (no click-to-start gate) — same one the engine's web demos
    # use. Audio is stubbed, so no user gesture is needed to unlock it.
    luminoveau_configure_web_target(doom
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin"
        SHELL_FILE "${LUMINOVEAU_ROOT_DIR}/assets/shell_sample.html")
    set_target_properties(doom PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
elseif(APPLE)
    set_target_properties(doom PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    # Bundle: put assets in Contents/Resources — where SDL_GetBasePath() points.
    add_custom_command(TARGET doom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_BUNDLE_CONTENT_DIR:doom>/Resources/assets")
else()
    set_target_properties(doom PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    # Copy the WAD next to the executable so Doom's fopen finds assets/doom1.wad.
    add_custom_command(TARGET doom POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:doom>/assets")
endif()
