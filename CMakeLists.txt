cmake_minimum_required(VERSION 3.20)

# BlitzRecomp: NFL Blitz 2000 recompiled for macOS (Apple Silicon), with a Windows x64 build in progress
# (docs/WINDOWS_VULKAN.md: Apple-only parts are gated on APPLE, the build host's tools on CMAKE_HOST_*). Two apps: Classic Blitz (the
# original game) and Modern Blitz (the 32-team 2026 game). Program ids stay BlitzRecompiled and
# Blitz2026Recompiled: they name the app folders that hold settings and saves.
# Structure follows BanjoRecomp's CMakeLists.txt (GPL-3.0). Inputs generated from the ROM live in
# build/ (never committed): run `make recomp rsp` first, then configure this project.

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

# The app's version, in one place: the launcher, Copy Debug Info and the bundle's Info.plist all read it.
# The build number is the commit count, and the commit hash goes into Copy Debug Info (both at configure time).
# `make release VERSION=x.y.z` passes -DBLITZ_VERSION for the release's builds (and -U it otherwise, so a release's
# number never lingers in a build folder's cache).
if (NOT BLITZ_VERSION)
    set(BLITZ_VERSION "0.9.0")
endif()
execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE BLITZ_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE BLITZ_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT BLITZ_BUILD)
    set(BLITZ_BUILD "0")
endif()
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture")

# BRANDING: target and executable name (BlitzRecompiled)
project(BlitzRecompiled LANGUAGES C CXX)
if (APPLE)
    enable_language(OBJC OBJCXX)  # macos_support.mm
endif()

# BLITZ_RELEASE (make release): builds to publish carry no path of the machine that built them. __FILE__ (asserts,
# logs) and debug info name sources relative to the checkout ("./src/...") instead of the builder's home folder, and a
# Windows .exe names its .pdb without a folder. tools/release_scan.sh checks the results.
option(BLITZ_RELEASE "A build to publish: no build-machine paths inside" OFF)
if (BLITZ_RELEASE)
    set(BLITZ_PREFIX_MAPS "${CMAKE_SOURCE_DIR}=." "${CMAKE_BINARY_DIR}=./build")
    if (DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
        list(APPEND BLITZ_PREFIX_MAPS "$ENV{HOME}=~")
    endif()
    foreach(m ${BLITZ_PREFIX_MAPS})
        if (MSVC)
            add_compile_options("/clang:-ffile-prefix-map=${m}")
        else()
            add_compile_options("-ffile-prefix-map=${m}")
        endif()
    endforeach()
    if (MSVC)
        add_link_options("/pdbaltpath:%_PDB%")
    elseif (APPLE)
        # A Mac or iPhone executable names every object file it was linked from (the debug map): none in a release.
        add_link_options("LINKER:-S")
    endif()
endif()
# BLITZ_MACOS marks the Mac's own code paths (Finder, screencapture, spawning the other engine): __APPLE__ alone also
# covers an iOS build (BLITZ_IOS).
if (APPLE AND NOT IOS)
    add_compile_definitions(BLITZ_MACOS=1)
endif()
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Xcode 27's libc++ no longer pulls <cstdlib> in transitively; RT64's hlslpp uses labs() without it.
if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /FIcstdlib")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdlib")
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/build CACHE PATH "Recompiler output (RecompiledFuncs/, rsp/)")

# Where the app's resources go: the bundle's Resources on a Mac, next to the .exe on Windows, the bundle itself on iOS,
# the APK's assets on Android.
if (IOS)
    set(BLITZ_RESOURCE_DIR "$<TARGET_BUNDLE_DIR:BlitzRecompiled>")
elseif (APPLE)
    set(BLITZ_RESOURCE_DIR "$<TARGET_BUNDLE_DIR:BlitzRecompiled>/Contents/Resources")
elseif (ANDROID)
    set(BLITZ_RESOURCE_DIR "${CMAKE_BINARY_DIR}/apk/assets/res")  # APK assets, unpacked on first launch (BlitzActivity)
else()
    set(BLITZ_RESOURCE_DIR "$<TARGET_FILE_DIR:BlitzRecompiled>")
endif()

# Flavour: "classic" (the v1.1 game, default) or "teamplus" (the 2026 flavour: Zoinkity's Team
# Extension applied to v1.1, 32 teams; see docs/BLITZ2026.md). Each flavour is built in its own
# build directory from its own recompiled code; the app sources are shared.
# "modern": Modern Blitz's features on the original v1.1 game's code (no Team Extension; 32 teams by slot mapping,
# src/game/engine32.cpp). Its recompiled code is build/engine32 (tools/engine32_toml.py: blitz2000.toml plus its hooks).
set(BLITZ_FLAVOUR "classic" CACHE STRING "classic, teamplus or modern")
set(BLITZ_MODERN_APP OFF)
if (BLITZ_FLAVOUR STREQUAL "teamplus" OR BLITZ_FLAVOUR STREQUAL "modern")
    set(BLITZ_MODERN_APP ON)
endif()
if (BLITZ_MODERN_APP)
    if (BLITZ_FLAVOUR STREQUAL "teamplus")
        set(FUNCS_DIR ${GENERATED_DIR}/tp/RecompiledFuncs)
        # BRANDING: desktop bundle id
        set(BLITZ_BUNDLE_ID "local.blitzrecompiled.blitz2026")
        add_compile_definitions(BLITZ_TEAMPLUS=1)
    else()
        set(FUNCS_DIR ${GENERATED_DIR}/engine32/RecompiledFuncs)
        # BRANDING: desktop bundle id
        set(BLITZ_BUNDLE_ID "local.blitzrecompiled.modern")
        add_compile_definitions(BLITZ_TEAMPLUS=0 BLITZ_ENGINE_ORIGINAL=1)
        set(BLITZ_ENGINE32_INCLUDE ${GENERATED_DIR}/engine32)  # engine32_sites.inc (tools/engine32_toml.py): Export ROM's cart
    endif()
    # The one app (team, 2026-10-25): "New Blitz" on the home screen; ids and app folders stay as they were.
    if (BLITZ_FLAVOUR STREQUAL "teamplus")
        # BRANDING: the app's shown name on every platform (macOS, iOS, Android label, Windows exe, PS5 title)
        set(BLITZ_BUNDLE_NAME "Modern Blitz")
    else()
        # BRANDING: the app's shown name on every platform (macOS, iOS, Android label, Windows exe, PS5 title)
        set(BLITZ_BUNDLE_NAME "New Blitz")
    endif()
    # BRANDING: app icon art
    set(BLITZ_APP_ICON ${CMAKE_SOURCE_DIR}/icons/app_2026.png)  # tools/make_icons.py
    add_compile_definitions(BLITZ_MODERN=1)
    # packs/blitz2026 ships inside the app as the default (our own roster data; docs/BLITZ2026.md).
    # Also the app's own art (art/blitz2026/files: the NFL Blitz 2027 logo as game images; art/blitz2026/logo27: the
    # overlays laid over the player's own title, New Features and badge images at startup; tools/make_logo2027.py).
    # packs/pac12 is the "Blitz: Pac-12" league pak (tools/make_league_pak.py), picked in Settings → Season → Rosters.
    # A public checkout may leave the real-name roster pack out (tools/make_public_tree.sh): then the app plays
    # the ROM's own teams by default, and the pack can be dropped into <app folder>/packs/blitz2026.
    # Into the app's resources (BLITZ_RESOURCE_DIR).
    # BLITZ_CONTENT=public (the Makefile's switch; docs/design/SEASON_PAK.md): the public app carries no licensed
    # content, so neither roster pack goes in; the 2026 Season pak brings packs/blitz2026 (src/game/content_dirs.cpp).
    # The '27 logo and its overlays (art/blitz2026) are ours and go in either way.
    set(BLITZ_CONTENT "private" CACHE STRING "private: the 2026 content inside the app; public: none (the season pak brings it)")
    set(BLITZ_BUNDLED_PACK_COMMAND)
    if (BLITZ_CONTENT STREQUAL "public")
        message(STATUS "BLITZ_CONTENT=public: no roster packs in the app")
        # The code's own switch for the public app: no developer-only features (Android's Send Logs).
        add_compile_definitions(BLITZ_PUBLIC_BUILD=1)
    elseif (EXISTS ${CMAKE_SOURCE_DIR}/packs/blitz2026/pack.json)
        set(BLITZ_BUNDLED_PACK_COMMAND COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/packs/blitz2026 ${BLITZ_RESOURCE_DIR}/packs/blitz2026)
    endif()
    # Each of these is bundled only when it's in the checkout: the public tree can leave any of them out
    # (tools/public_exclude.txt), and the app then does without (no Pac-12 choice; the cartridge's own title and badges).
    if (NOT BLITZ_CONTENT STREQUAL "public" AND EXISTS ${CMAKE_SOURCE_DIR}/packs/pac12/pack.json)
        list(APPEND BLITZ_BUNDLED_PACK_COMMAND
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/packs/pac12 ${BLITZ_RESOURCE_DIR}/packs/pac12)
    endif()
    if (EXISTS ${CMAKE_SOURCE_DIR}/art/blitz2026/files)
        list(APPEND BLITZ_BUNDLED_PACK_COMMAND
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/art/blitz2026/files ${BLITZ_RESOURCE_DIR}/art/files)
    endif()
    if (EXISTS ${CMAKE_SOURCE_DIR}/art/blitz2026/logo27)
        list(APPEND BLITZ_BUNDLED_PACK_COMMAND
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/art/blitz2026/logo27 ${BLITZ_RESOURCE_DIR}/art/logo27)
    endif()
elseif (BLITZ_FLAVOUR STREQUAL "classic")
    set(FUNCS_DIR ${GENERATED_DIR}/RecompiledFuncs)
    # BRANDING: the app's shown name on every platform (macOS, iOS, Android label, Windows exe, PS5 title)
    set(BLITZ_BUNDLE_NAME "Classic Blitz")
    # BRANDING: desktop bundle id
    set(BLITZ_BUNDLE_ID "local.blitzrecompiled")
    # BRANDING: app icon art
    set(BLITZ_APP_ICON ${CMAKE_SOURCE_DIR}/icons/app_classic.png)
else()
    message(FATAL_ERROR "BLITZ_FLAVOUR must be classic, teamplus or modern")
endif()
# The app's resources, the same on every platform: each platform's staging file (platform/<os>/*.cmake) runs these
# in its POST_BUILD step and adds its own extras (icons, runtime libraries, signing).
set(BLITZ_STAGE_RESOURCES
    # The UI's assets (without the SCSS sources) and the controller database
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${BLITZ_RESOURCE_DIR}/assets
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${BLITZ_RESOURCE_DIR}/assets/scss
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt ${BLITZ_RESOURCE_DIR}/
    # Built-in content packs and the app's own art (Modern)
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BLITZ_RESOURCE_DIR}/packs
    ${BLITZ_BUNDLED_PACK_COMMAND})

# BLITZ_SAVESTATES (save states; `make app` / `make app2026` pass ON, SAVESTATES=0 turns it off): build from
# tools/savestate/resumable.py's resumable copy
# of the recompiled code instead (build/savestate/...; build/analysis/savestates/PLAN.md).
option(BLITZ_SAVESTATES "Build from the resumable code (save states, experimental)" OFF)
if (BLITZ_SAVESTATES)
    if (BLITZ_FLAVOUR STREQUAL "teamplus")
        set(FUNCS_DIR ${GENERATED_DIR}/savestate/tp/RecompiledFuncs)
    elseif (BLITZ_FLAVOUR STREQUAL "modern")
        set(FUNCS_DIR ${GENERATED_DIR}/engine32/savestate/RecompiledFuncs)
    else()
        set(FUNCS_DIR ${GENERATED_DIR}/savestate/RecompiledFuncs)
    endif()
    add_compile_definitions(BLITZ_SAVESTATES=1)
endif()
if (NOT EXISTS ${FUNCS_DIR}/recomp_overlays.inl OR NOT EXISTS ${GENERATED_DIR}/rsp/n_aspMain.cpp)
    message(FATAL_ERROR "Recompiler output missing in ${FUNCS_DIR}. Run `make recomp rsp` (or `make recomp2026 rsp`) first.")
endif()

# tools/build_deps.sh builds SDL2 and FreeType for macOS 13 into build/deps/prefix; with it the app runs on
# macOS 13+. Without it the build falls back to Homebrew's, which target the build Mac's macOS (26 here).
# Windows uses RT64's bundled x64 SDL2 2.26.3 and FreeType (src/contrib/mupen64plus-win32-deps), one SDL for the
# whole build: the frontend's Windows include path names it as sdl2_SOURCE_DIR.
set(BLITZ_DEPS_PREFIX ${CMAKE_SOURCE_DIR}/build/deps/prefix)
if (IOS)
    # iPhone (docs/IOS_FEASIBILITY.md; sideloaded): SDL2 (with UIKit scene support) and FreeType, static, from
    # tools/ios/build_deps.sh. No desktop file dialog (the system document picker instead), no Export ROM (BLITZ_MOBILE).
    set(BLITZ_IOS_PREFIX ${CMAKE_SOURCE_DIR}/build/ios/prefix)
    if (NOT EXISTS ${BLITZ_IOS_PREFIX}/lib/libSDL2.a)
        message(FATAL_ERROR "build/ios/prefix is missing: run tools/ios/build_deps.sh")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH ${BLITZ_IOS_PREFIX})
    set(SDL2_DIR ${BLITZ_IOS_PREFIX}/lib/cmake/SDL2)
    set(RT64_NO_FILE_DIALOG TRUE)
    add_compile_definitions(BLITZ_IOS=1 BLITZ_MOBILE=1)
    include_directories(${BLITZ_IOS_PREFIX}/include/SDL2 ${BLITZ_IOS_PREFIX}/include)
    add_library(Freetype::Freetype STATIC IMPORTED GLOBAL)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION ${BLITZ_IOS_PREFIX}/lib/libfreetype.a
        INTERFACE_INCLUDE_DIRECTORIES ${BLITZ_IOS_PREFIX}/include/freetype2
        INTERFACE_LINK_LIBRARIES z)
    # RT64's Metal shader converter runs on the Mac: the Mac build's (make app builds it).
    set(RT64_HOST_SPIRV_CROSS_MSL ${CMAKE_SOURCE_DIR}/build/bin/spirv_cross_msl)
    if (NOT EXISTS ${RT64_HOST_SPIRV_CROSS_MSL})
        message(FATAL_ERROR "build/bin/spirv_cross_msl is missing: build the Mac app once (make app)")
    endif()
elseif (PS5)
    # PS5 homebrew (docs/PS5_HOMEBREW.md; the PS5 payload SDK's toolchain/prospero.cmake): SDL2's PS5 port and FreeType
    # from tools/ps5/setup_sdk.sh, no native file dialog, emulated thread-local storage, the Zen 2's SSE4.1.
    list(PREPEND CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/build/ps5/prefix)
    set(SDL2_DIR ${CMAKE_SOURCE_DIR}/build/ps5/prefix/lib/cmake/SDL2)
    set(RT64_NO_FILE_DIALOG TRUE)
    add_compile_options(-femulated-tls -msse4.1)
    # SDL's headers for every subproject (the frontend adds them only for macOS and Linux; the PS5 is FreeBSD)
    include_directories(${CMAKE_SOURCE_DIR}/build/ps5/prefix/include/SDL2 ${CMAKE_SOURCE_DIR}/build/ps5/prefix/include)
    link_directories(${CMAKE_SOURCE_DIR}/build/ps5/prefix/lib)  # SDL2's own dependencies (libsamplerate)
    # The toolchain keeps package searches inside the SDK's sysroot: FreeType (RmlUi's fonts) by its target name.
    add_library(Freetype::Freetype STATIC IMPORTED GLOBAL)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/build/ps5/prefix/lib/libfreetype.a
        INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/build/ps5/prefix/include/freetype2)
elseif (ANDROID)
    # Android (docs/ANDROID.md; sideloaded APK): SDL2 (shared, loaded by SDLActivity) and FreeType from
    # tools/android/build_deps.sh, the NDK's toolchain (tools/android/env.sh). No desktop file dialog, no Export ROM.
    set(BLITZ_ANDROID_PREFIX ${CMAKE_SOURCE_DIR}/build/android/prefix)
    if (NOT EXISTS ${BLITZ_ANDROID_PREFIX}/lib/libSDL2.so)
        message(FATAL_ERROR "build/android/prefix is missing: run tools/android/build_deps.sh")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH ${BLITZ_ANDROID_PREFIX})
    set(CMAKE_FIND_ROOT_PATH ${BLITZ_ANDROID_PREFIX} ${CMAKE_FIND_ROOT_PATH})
    set(SDL2_DIR ${BLITZ_ANDROID_PREFIX}/lib/cmake/SDL2)
    set(RT64_NO_FILE_DIALOG TRUE)
    add_compile_definitions(BLITZ_ANDROID=1 BLITZ_MOBILE=1)
    include_directories(${BLITZ_ANDROID_PREFIX}/include/SDL2 ${BLITZ_ANDROID_PREFIX}/include)
    add_library(Freetype::Freetype STATIC IMPORTED GLOBAL)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION ${BLITZ_ANDROID_PREFIX}/lib/libfreetype.a
        INTERFACE_INCLUDE_DIRECTORIES ${BLITZ_ANDROID_PREFIX}/include/freetype2
        INTERFACE_LINK_LIBRARIES z)
elseif (WIN32)
    set(BLITZ_WIN_DEPS ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/mupen64plus-win32-deps)
    set(sdl2_SOURCE_DIR ${BLITZ_WIN_DEPS}/SDL2-2.26.3)
    set(SDL2_DIR ${BLITZ_WIN_DEPS}/SDL2-2.26.3/cmake CACHE PATH "SDL2 package (RT64's Windows x64 build)" FORCE)
    set(FREETYPE_DIR ${BLITZ_WIN_DEPS}/freetype-2.13.0)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux x86_64 (docs/PLATFORMS.md; ROADMAP.md): cross-built on the Mac against a Debian 12 sysroot
    # (tools/linux/make_sysroot.sh, platform/linux/toolchain-x86_64.cmake). The system's SDL2 and FreeType; RT64 draws
    # through SDL's Vulkan surface (X11 or Wayland), so the whole build agrees on SDL_Window as the window; the file
    # dialog through xdg-desktop-portal (D-Bus), not GTK; SSE4.1 for the RSP's vector code.
    set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
    set(NFD_PORTAL ON CACHE BOOL "" FORCE)
    add_compile_definitions(RT64_SDL_WINDOW_VULKAN PLUME_SDL_VULKAN_ENABLED ZSTD_DISABLE_ASM)  # zstd: its C decoder (the
    add_compile_options(-msse4.1)                                                             # .S isn't assembled here)
elseif (EXISTS ${BLITZ_DEPS_PREFIX}/lib/cmake/SDL2/SDL2Config.cmake)
    set(SDL2_DIR ${BLITZ_DEPS_PREFIX}/lib/cmake/SDL2 CACHE PATH "SDL2 package (build/deps, macOS 13+)" FORCE)
    set(FREETYPE_INCLUDE_DIR_ft2build ${BLITZ_DEPS_PREFIX}/include/freetype2 CACHE PATH "" FORCE)
    set(FREETYPE_INCLUDE_DIR_freetype2 ${BLITZ_DEPS_PREFIX}/include/freetype2 CACHE PATH "" FORCE)
    set(FREETYPE_LIBRARY ${BLITZ_DEPS_PREFIX}/lib/libfreetype.dylib CACHE FILEPATH "" FORCE)
    set(FREETYPE_LIBRARY_RELEASE ${BLITZ_DEPS_PREFIX}/lib/libfreetype.dylib CACHE FILEPATH "" FORCE)
    list(PREPEND CMAKE_PREFIX_PATH ${BLITZ_DEPS_PREFIX})
    set(BLITZ_MIN_MACOS 13.0)
    message(STATUS "Bundled libraries: build/deps (macOS ${BLITZ_MIN_MACOS}+)")
else()
    # Switching back from build/deps (removed or not built): forget its cached paths.
    foreach(v SDL2_DIR FREETYPE_INCLUDE_DIR_ft2build FREETYPE_INCLUDE_DIR_freetype2 FREETYPE_LIBRARY FREETYPE_LIBRARY_RELEASE)
        if (DEFINED CACHE{${v}} AND "$CACHE{${v}}" MATCHES "build/deps/prefix")
            unset(${v} CACHE)
        endif()
    endforeach()
    set(BLITZ_MIN_MACOS 26.0)  # Homebrew's SDL2/freetype/libpng are built for the build Mac (otool -l: minos 26.0)
    message(STATUS "Bundled libraries: Homebrew (macOS ${BLITZ_MIN_MACOS}+; run tools/build_deps.sh for macOS 13+)")
endif()

# Homebrew's sdl2-compat (the SDL2 API on top of SDL3) takes over the shared libSDL2 links when another
# package depends on it, and an app bundled with it fails at launch ("Failed loading SDL3 library").
# Even /opt/homebrew/opt/sdl2 then points at sdl2-compat, so use the real SDL2 keg's Cellar path.
file(GLOB REAL_SDL2_DIRS /opt/homebrew/Cellar/sdl2/*/lib/cmake/SDL2)
if (APPLE AND REAL_SDL2_DIRS AND BLITZ_MIN_MACOS STREQUAL "26.0")
    list(SORT REAL_SDL2_DIRS)
    list(GET REAL_SDL2_DIRS -1 REAL_SDL2_DIR)
    set(SDL2_DIR ${REAL_SDL2_DIR} CACHE PATH "SDL2 package (real SDL2, not sdl2-compat)" FORCE)
    # The keg's libSDL2 still names /opt/homebrew/opt/sdl2 as its install path, so the bundle step
    # is told where the real file is too (platform/macos/fixup_bundle.cmake).
    get_filename_component(REAL_SDL2_LIBDIR ${REAL_SDL2_DIR}/../.. ABSOLUTE)
endif()

set(RT64_STATIC TRUE)
# Cross-compiling for Windows on a Mac: RT64's shaders are compiled by the Mac's DXC (platform/rt64-patches/0004).
if (CMAKE_CROSSCOMPILING AND CMAKE_HOST_APPLE)
    # Its DXIL comes out unsigned, which D3D12 refuses: the frontend makes Auto mean Vulkan (frontend patch 0013).
    add_compile_definitions(BLITZ_CROSS_BUILT_SHADERS=1)
    set(RT64_HOST_DXC "DYLD_LIBRARY_PATH=${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos")
    # and file_to_c (shaders and images into C), built for the Mac here
    set(RT64_HOST_FILE_TO_C ${CMAKE_BINARY_DIR}/host/file_to_c)
    if (NOT EXISTS ${RT64_HOST_FILE_TO_C})
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/host)
        execute_process(COMMAND xcrun c++ -std=c++17 -O2 ${CMAKE_SOURCE_DIR}/lib/rt64/src/tools/file_to_c/file_to_c.cpp
                                -o ${RT64_HOST_FILE_TO_C} RESULT_VARIABLE host_tool_result)
        if (NOT host_tool_result EQUAL 0)
            message(FATAL_ERROR "Couldn't build the Mac's file_to_c")
        endif()
    endif()
endif()
add_compile_definitions(HLSL_CPU __PRFCHWINTRIN_H)
add_compile_definitions(BLITZ_VERSION="${BLITZ_VERSION}" BLITZ_BUILD="${BLITZ_BUILD}" BLITZ_COMMIT="${BLITZ_COMMIT}")

if (ANDROID)
    # zstd's dictionary builder calls qsort_r, which Android's C library lacks; nothing here builds dictionaries.
    set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

set(RECOMP_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

# RecompiledFuncs: the game's code, translated to C by N64Recomp.
add_library(RecompiledFuncs STATIC)
if (MSVC)  # clang-cl: the same Clang options, passed through
    target_compile_options(RecompiledFuncs PRIVATE /clang:-fno-strict-aliasing -Wno-unused-variable
                           -Wno-implicit-function-declaration /clang:-falign-functions=16)
else()
    target_compile_options(RecompiledFuncs PRIVATE
        -fno-strict-aliasing
        -Wno-unused-variable
        -Wno-implicit-function-declaration
        -falign-functions=16
    )
endif()
target_include_directories(RecompiledFuncs PRIVATE ${RECOMP_INCLUDES})
file(GLOB FUNC_C_SOURCES ${FUNCS_DIR}/*.c)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES})
if (BLITZ_SAVESTATES)
    target_sources(RecompiledFuncs PRIVATE ${CMAKE_SOURCE_DIR}/src/game/ss_runtime.c)
    target_include_directories(RecompiledFuncs PRIVATE ${FUNCS_DIR})
endif()

# The app: an executable, or on Android the shared library SDLActivity loads (platform/android).
if (ANDROID)
    add_library(BlitzRecompiled SHARED)
else()
    add_executable(BlitzRecompiled)
endif()

target_sources(BlitzRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/dev_input.cpp
    ${CMAKE_SOURCE_DIR}/src/main/controllers.cpp
    ${CMAKE_SOURCE_DIR}/src/main/launcher.cpp
    ${CMAKE_SOURCE_DIR}/src/main/os_shell.cpp
    ${CMAKE_SOURCE_DIR}/src/main/app_log.cpp
    ${CMAKE_SOURCE_DIR}/src/main/phone_pads.cpp
    ${CMAKE_SOURCE_DIR}/src/main/game_moment.cpp
    ${CMAKE_SOURCE_DIR}/src/main/firstrun.cpp
    ${CMAKE_SOURCE_DIR}/src/main/firstrun_screen.cpp
    ${CMAKE_SOURCE_DIR}/src/main/crash_report.cpp
    ${CMAKE_SOURCE_DIR}/src/main/reset_data.cpp
    ${CMAKE_SOURCE_DIR}/src/main/device_send.cpp
    ${CMAKE_SOURCE_DIR}/src/main/capture.cpp
    ${CMAKE_SOURCE_DIR}/src/main/toast.cpp
    ${CMAKE_SOURCE_DIR}/src/main/replay_screen.cpp
    ${CMAKE_SOURCE_DIR}/src/main/replay_gif.cpp
    ${CMAKE_SOURCE_DIR}/src/main/touch_controls.cpp
    ${CMAKE_SOURCE_DIR}/src/main/fps_counter.cpp
    ${CMAKE_SOURCE_DIR}/src/main/hints.cpp
    ${CMAKE_SOURCE_DIR}/src/main/install_expiry.cpp
    ${CMAKE_SOURCE_DIR}/src/main/experience.cpp
    ${CMAKE_SOURCE_DIR}/src/main/ui_kit.cpp
    ${CMAKE_SOURCE_DIR}/third_party/qrcodegen/qrcodegen.cpp
    ${CMAKE_SOURCE_DIR}/src/main/dev_monitor.cpp
    ${CMAKE_SOURCE_DIR}/src/main/audio.cpp
    ${CMAKE_SOURCE_DIR}/src/game/config.cpp
    ${CMAKE_SOURCE_DIR}/src/game/known_roms.cpp
    ${CMAKE_SOURCE_DIR}/src/game/uniform_picker.cpp
    ${CMAKE_SOURCE_DIR}/src/game/receiver_targets.cpp
    ${CMAKE_SOURCE_DIR}/src/game/portraits.cpp
    ${CMAKE_SOURCE_DIR}/src/game/addons.cpp
    ${CMAKE_SOURCE_DIR}/src/game/export_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/game/export_screen.cpp
    ${CMAKE_SOURCE_DIR}/src/game/overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ultra_extras.cpp
    ${CMAKE_SOURCE_DIR}/src/game/game_init.cpp
    ${CMAKE_SOURCE_DIR}/src/game/threads.cpp
    ${CMAKE_SOURCE_DIR}/src/game/savestate.cpp
    ${CMAKE_SOURCE_DIR}/src/game/debug_print.cpp
    ${CMAKE_SOURCE_DIR}/src/game/controller_pak.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ai.cpp
    ${CMAKE_SOURCE_DIR}/src/game/widescreen.cpp
    ${CMAKE_SOURCE_DIR}/src/game/tall_view.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ds_view.cpp
    ${CMAKE_SOURCE_DIR}/src/game/replay.cpp
    ${CMAKE_SOURCE_DIR}/src/game/gfx_ext.cpp
    ${CMAKE_SOURCE_DIR}/src/game/skin_tones.cpp
    ${CMAKE_SOURCE_DIR}/src/game/notices.cpp
    ${CMAKE_SOURCE_DIR}/src/game/content_pack.cpp
    ${CMAKE_SOURCE_DIR}/src/game/content_pack_tp.cpp
    ${CMAKE_SOURCE_DIR}/src/game/team_store.cpp
    ${CMAKE_SOURCE_DIR}/src/game/team_store_original.cpp
    ${CMAKE_SOURCE_DIR}/src/game/engine32.cpp
    ${CMAKE_SOURCE_DIR}/src/game/art_dropin.cpp
    ${CMAKE_SOURCE_DIR}/src/game/button_glyphs.cpp
    ${CMAKE_SOURCE_DIR}/src/game/team_select.cpp
    ${CMAKE_SOURCE_DIR}/src/game/play_log.cpp
    ${CMAKE_SOURCE_DIR}/src/game/board_digits.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ds_stylus.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ds_touch.cpp
    ${CMAKE_SOURCE_DIR}/src/game/checkpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/game/opponent_select.cpp
    ${CMAKE_SOURCE_DIR}/src/game/crew.cpp
    ${CMAKE_SOURCE_DIR}/src/game/roster_room.cpp
    ${CMAKE_SOURCE_DIR}/src/game/mixed_uniforms.cpp
    ${CMAKE_SOURCE_DIR}/src/game/tournament.cpp
    ${CMAKE_SOURCE_DIR}/src/game/adv_mode.cpp
    ${CMAKE_SOURCE_DIR}/src/game/adv_season.cpp
    ${CMAKE_SOURCE_DIR}/src/game/adv_save.cpp
    ${CMAKE_SOURCE_DIR}/src/game/adv_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/game/adv_league.cpp
    ${CMAKE_SOURCE_DIR}/src/game/season32.cpp
    ${CMAKE_SOURCE_DIR}/src/game/season_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/game/rosters_live.cpp
    ${CMAKE_SOURCE_DIR}/src/game/controllers_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ui_theme.cpp
    ${CMAKE_SOURCE_DIR}/src/game/house_rules.cpp
    ${CMAKE_SOURCE_DIR}/src/game/cpu_smart.cpp
    ${CMAKE_SOURCE_DIR}/src/game/season_tiebreak.cpp
    ${CMAKE_SOURCE_DIR}/src/game/content_dirs.cpp
    ${CMAKE_SOURCE_DIR}/src/game/season_pak.cpp
    ${CMAKE_SOURCE_DIR}/src/common/sha256.cpp
    ${GENERATED_DIR}/rsp/n_aspMain.cpp
)
# The host services each platform provides (window menu, HTTP, ...): the same functions, one file per platform.
if (IOS)
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/src/main/platform_ios.mm ${CMAKE_SOURCE_DIR}/src/main/provision_profile.mm)
elseif (APPLE)
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/src/main/macos_support.mm)
elseif (WIN32)
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/src/main/platform_windows.cpp)
elseif (PS5)
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/src/main/platform_ps5.cpp)
elseif (ANDROID)
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/src/main/platform_android.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/src/main/platform_linux.cpp)
endif()

target_include_directories(BlitzRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/qrcodegen
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/src  # widgets for our own settings tabs (controllers_tab.cpp)
    ${FUNCS_DIR}
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/sse2neon
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty
    ${CMAKE_SOURCE_DIR}/lib/rt64/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompinput/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# icon_bytes.c: the app icon embedded for the launcher thumbnail.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.h
    COMMAND file_to_c ${BLITZ_APP_ICON} icon_bytes ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.h
    DEPENDS ${BLITZ_APP_ICON}
)
target_sources(BlitzRecompiled PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c)

# logo_bytes.c: the launcher logo (our own art, tools/make_logo27.py): '27 for the 2026 app, 2000 for Classic.
if (BLITZ_MODERN_APP)
    # BRANDING: launcher logo art
    set(BLITZ_LOGO ${CMAKE_SOURCE_DIR}/icons/logo_2026.png)
else()
    # BRANDING: launcher logo art
    set(BLITZ_LOGO ${CMAKE_SOURCE_DIR}/icons/logo_2026.png)  # one logo: Original 1999 shows the cartridge's own in the game
endif()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/logo_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/logo_bytes.h
    COMMAND file_to_c ${BLITZ_LOGO} logo_bytes ${CMAKE_CURRENT_BINARY_DIR}/logo_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/logo_bytes.h
    DEPENDS ${BLITZ_LOGO}
)
target_sources(BlitzRecompiled PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/logo_bytes.c)

# wide_logo_bytes.c: the short, wide Blitz '27 logo the phone page shows in Modern Blitz (tools/make_logo_wide.py).
if (BLITZ_MODERN_APP)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wide_logo_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/wide_logo_bytes.h
        # BRANDING: phone page logo art
        COMMAND file_to_c ${CMAKE_SOURCE_DIR}/icons/logo_wide_2026.png wide_logo_bytes ${CMAKE_CURRENT_BINARY_DIR}/wide_logo_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/wide_logo_bytes.h
        DEPENDS ${CMAKE_SOURCE_DIR}/icons/logo_wide_2026.png
    )
    target_sources(BlitzRecompiled PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/wide_logo_bytes.c)
endif()

if (MSVC)
    # x64 Windows: SSE4.1 for the RSP's vector code (n_aspMain.cpp; Macs use sse2neon), and no min/max macros
    target_compile_options(BlitzRecompiled PRIVATE /clang:-fno-strict-aliasing /clang:-msse4.1)
    target_compile_definitions(BlitzRecompiled PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(BlitzRecompiled PRIVATE -fno-strict-aliasing -fms-extensions)
endif()

if (APPLE AND NOT IOS)
    # ld64 wrapper that keeps __TEXT writable (librecomp patches functions at runtime for mods).
    target_link_options(BlitzRecompiled PRIVATE "-fuse-ld=${CMAKE_SOURCE_DIR}/platform/macos/ld64")
endif()

find_package(SDL2 REQUIRED)
target_include_directories(BlitzRecompiled PRIVATE ${SDL2_INCLUDE_DIRS})
if (WIN32)
    # RT64's Windows SDL keeps its headers in include/ itself; our sources include <SDL2/SDL.h>.
    file(COPY ${sdl2_SOURCE_DIR}/include/ DESTINATION ${CMAKE_BINARY_DIR}/sdl2_include/SDL2)
    target_include_directories(BlitzRecompiled PRIVATE ${CMAKE_BINARY_DIR}/sdl2_include)
    target_link_directories(BlitzRecompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)  # its package names SDL2.lib bare
endif()
if (BLITZ_ENGINE32_INCLUDE)
    target_include_directories(BlitzRecompiled PRIVATE ${BLITZ_ENGINE32_INCLUDE})
endif()
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
target_link_libraries(BlitzRecompiled PRIVATE ${CMAKE_DL_LIBS} Threads::Threads SDL2::SDL2)
if (ANDROID)
    # Nearly every recompiled call otherwise goes through the PLT, which a weak CPU feels.
    target_link_options(BlitzRecompiled PRIVATE -Wl,-Bsymbolic)
endif()

if (IOS)
    include(${CMAKE_SOURCE_DIR}/platform/ios/ios_bundle.cmake)
elseif (APPLE)
    include(${CMAKE_SOURCE_DIR}/platform/macos/apple_bundle.cmake)
elseif (WIN32)
    include(${CMAKE_SOURCE_DIR}/platform/windows/windows_stage.cmake)
elseif (PS5)
    include(${CMAKE_SOURCE_DIR}/platform/ps5/ps5_stage.cmake)
elseif (ANDROID)
    include(${CMAKE_SOURCE_DIR}/platform/android/android_stage.cmake)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(${CMAKE_SOURCE_DIR}/platform/linux/linux_stage.cmake)
endif()

# Shader compiler binaries RT64 uses at build time (same settings as BanjoRecomp): the build host's.
set(DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")
if (CMAKE_HOST_APPLE)
    set(DXC "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos")
    set(SPIRVCROSS "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64//src/contrib/spirv-cross/bin/x64/spirv-cross")
elseif (CMAKE_HOST_WIN32)
    set(DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
endif()

set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
set(RECOMP_FRONTEND_RT64_PATH ${CMAKE_SOURCE_DIR}/lib/rt64)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RecompFrontend)

target_link_libraries(BlitzRecompiled PRIVATE
    RecompiledFuncs
    recompui
    recompinput
    librecomp
    ultramodern
    rt64
)
if (NOT RT64_NO_FILE_DIALOG)
    target_link_libraries(BlitzRecompiled PRIVATE nfd)
endif()

# BlitzRecomp's starter (src/starter/starter.c, docs/design/ONE_APP.md): picks Classic or Modern and execs that engine.
# tools/make_bundle.sh puts it and both engines into one BlitzRecomp.app. (Windows: one engine per folder for now.)
if (APPLE AND NOT IOS)
    add_executable(BlitzRecompStarter src/starter/starter.c)
    # BRANDING: the one-app starter's name
    set_target_properties(BlitzRecompStarter PROPERTIES OUTPUT_NAME BlitzRecomp)
endif()
