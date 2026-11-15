# New Blitz for the PSP (docs/PSP_PORT.md §2.2): one standalone project, two targets.
#   PSP_TARGET=host  the Mac headless core, psp-host (make psp-host): the PSP's runtime and game code with null graphics,
#                    silent audio, scripted input and stdout logs, built with the Mac's clang
#   PSP_TARGET=psp   the EBOOT (make psp-classic, configured with psp-cmake): build/psp/app_classic/EBOOT.PBP, newblitz.prx
#                    (PSPLink) and newblitz.elf (psp-addr2line). The sceGu renderer (src/psp/gfx, M7; the switch file nogfx:
#                    M5's null renderer with pspDebugScreen text), silent audio, sceCtrl plus script input, all four log sinks
#                    and the remote verbs
#   FLAVOUR=classic|modern   (modern: M8)
#   GPR32=ON|OFF     ON: tools/psp/gpr32.py's 32-bit-register code (build/psp/<flavour>/RecompiledFuncs) with RECOMP_GPR32;
#                    OFF: the stock 64-bit code (build/RecompiledFuncs), the other half of the lockstep comparison (§3.4)
#   MEMCHECK=ON      -DBLITZ_PSP_MEMCHECK: every RDRAM address checked before it is used (§4.2)
# The root CMakeLists.txt and every other platform are untouched by this file.
cmake_minimum_required(VERSION 3.20)
project(blitz_psp C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(PSP)
  # newlib hides its POSIX parts (strnlen, fileno, strcasecmp, dprintf, _exit) from strict c++20. Not gnu++20: that also
  # defines "mips", which ultramodern/extensions.h takes for an N64 patch build.
  add_compile_definitions(_GNU_SOURCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PSP_TARGET "host" CACHE STRING "host (Mac headless core) or psp (EBOOT)")
set(FLAVOUR "classic" CACHE STRING "classic or modern")
option(GPR32 "32-bit registers (tools/psp/gpr32.py output)" ON)
option(MEMCHECK "Check every RDRAM address (BLITZ_PSP_MEMCHECK)" OFF)

get_filename_component(BLITZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RT "${BLITZ_ROOT}/lib/N64ModernRuntime")
set(PSP_SRC "${BLITZ_ROOT}/src/psp")
set(GAME "${BLITZ_ROOT}/src/game")
set(MAIN "${BLITZ_ROOT}/src/main")

execute_process(COMMAND git -C "${BLITZ_ROOT}" rev-list --count HEAD OUTPUT_VARIABLE BLITZ_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${BLITZ_ROOT}" rev-parse --short=7 HEAD OUTPUT_VARIABLE BLITZ_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BLITZ_BUILD)
  set(BLITZ_BUILD 0)
endif()
if(NOT BLITZ_COMMIT)
  set(BLITZ_COMMIT unknown)
endif()

if(PSP_TARGET STREQUAL "psp")
  if(NOT PSP)
    message(FATAL_ERROR "PSP_TARGET=psp needs the PSP toolchain: configure with psp-cmake (make psp-classic)")
  endif()
  set(IS_PSP ON)
elseif(PSP_TARGET STREQUAL "host")
  set(IS_PSP OFF)
else()
  message(FATAL_ERROR "PSP_TARGET=${PSP_TARGET}: host or psp")
endif()
if(FLAVOUR STREQUAL "classic")
  set(IS_MODERN OFF)
elseif(FLAVOUR STREQUAL "modern")
  # Modern Blitz on the original v1.1 game (the desktop's BLITZ_FLAVOUR=modern: build/engine32's code, engine32.cpp's
  # 32 teams, the Blitz 2026 pack, Modern's art merged into the ROM on the Mac: docs/PSP_PORT.md §4.3, M8)
  set(IS_MODERN ON)
else()
  message(FATAL_ERROR "FLAVOUR=${FLAVOUR}: classic or modern")
endif()

if(GPR32)
  set(FUNCS_DIR "${BLITZ_ROOT}/build/psp/${FLAVOUR}/RecompiledFuncs")
else()
  if(IS_MODERN)
    set(FUNCS_DIR "${BLITZ_ROOT}/build/engine32/RecompiledFuncs")
  else()
    set(FUNCS_DIR "${BLITZ_ROOT}/build/RecompiledFuncs")
  endif()
endif()
if(NOT EXISTS "${FUNCS_DIR}/recomp_overlays.inl")
  message(FATAL_ERROR "${FUNCS_DIR} is missing: run make psp-recomp (gpr32) or make recomp (stock)")
endif()

# ---- Global definitions: every TU, because RECOMP_GPR32 and the host_thread backend change struct layouts (§3.1) ----
# ULTRAMODERN_HOST_SEM_PLATFORM: ultramodern::host_sem is src/psp/ultramodern_host_sem_platform.hpp (kernel semaphores on
# the PSP, moodycamel on the Mac), and recomp.cpp's game start waits on one instead of std::atomic::wait (patch 0021).
add_compile_definitions(
  BLITZ_PSP=1
  RECOMP_SMALL_RDRAM=1 RECOMP_NO_MODS=1
  ULTRAMODERN_HOST_THREAD_PLATFORM=1 ULTRAMODERN_HOST_SEM_PLATFORM=1
  NDEBUG
  BLITZ_VERSION="0.9.0" BLITZ_BUILD="${BLITZ_BUILD}" BLITZ_COMMIT="${BLITZ_COMMIT}")
if(IS_PSP)
  if(NOT GPR32)
    message(FATAL_ERROR "the EBOOT needs GPR32 (64-bit registers are only for the Mac's lockstep comparison)")
  endif()
  # RECOMP_RDRAM_MASK: every guest address is masked into the 8 MB block (no MMU: nothing may land outside it, §4.2).
  add_compile_definitions(BLITZ_PSP_DEVICE=1 RECOMP_RDRAM_MASK=1)   # no BLITZ_SAVESTATES: the sources test it with #ifdef
else()
  add_compile_definitions(BLITZ_PSP_HOST=1
    ULTRAMODERN_LOCKSTEP=1)         # compiled in on psp-host; off unless --lockstep
endif()
if(GPR32)
  add_compile_definitions(RECOMP_GPR32=1)
endif()
if(IS_MODERN)
  # The desktop's BLITZ_FLAVOUR=modern definitions (root CMakeLists.txt)
  add_compile_definitions(BLITZ_MODERN=1 BLITZ_TEAMPLUS=0 BLITZ_ENGINE_ORIGINAL=1)
endif()
if(MEMCHECK)
  if(NOT GPR32)
    message(FATAL_ERROR "MEMCHECK needs GPR32 (it replaces recomp.h's RDRAM_OFF)")
  endif()
  add_compile_definitions(BLITZ_PSP_MEMCHECK=1)
  add_compile_options("SHELL:-include ${PSP_SRC}/psp_memcheck.h")
endif()
if(IS_PSP)
  # docs/PSP_PORT.md §3.3. -g1: line tables for psp-addr2line on newblitz.elf (the EBOOT's PRX carries none of it).
  add_compile_options(-O2 -G0 -g1 -fno-strict-aliasing -fwrapv -fno-math-errno -ffunction-sections -fdata-sections
                      -mno-check-zero-division)
else()
  add_compile_options(-O2 -g -fno-strict-aliasing -fwrapv)
endif()

# miniz's export header is normally generated by its own CMake project (mods.hpp includes miniz).
file(WRITE "${CMAKE_BINARY_DIR}/gen/miniz_export.h" "#pragma once\n#define MINIZ_EXPORT\n")
set(RECOMP_INCLUDES
  "${CMAKE_BINARY_DIR}/gen"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"     # recompui-lite's shadow headers first
  "${BLITZ_ROOT}/include"
  "${RT}/ultramodern/include"
  "${RT}/librecomp/include"
  "${RT}/N64Recomp/include"
  "${RT}/thirdparty"
  "${RT}/thirdparty/concurrentqueue"
  "${RT}/thirdparty/miniz"
  "${RT}/thirdparty/json"
  "${RT}/thirdparty/sse2neon"
  "${PSP_SRC}")

# ---- The game's code ----
file(GLOB FUNC_C_SOURCES "${FUNCS_DIR}/*.c")
add_library(RecompiledFuncs STATIC ${FUNC_C_SOURCES} "${FUNCS_DIR}/lookup.cpp")
target_include_directories(RecompiledFuncs PRIVATE ${RECOMP_INCLUDES})
target_compile_options(RecompiledFuncs PRIVATE -Wno-unused-variable -Wno-implicit-function-declaration)
if(IS_PSP)
  # -fexceptions: unwind tables for the game's C code. A game thread ends by throwing ultramodern::thread_terminated from
  # osDestroyThread through its recompiled frames; psp-gcc emits no .eh_frame for C without this (clang on the Mac does by
  # default), and the throw would end in std::terminate.
  target_compile_options(RecompiledFuncs PRIVATE -fexceptions)
endif()

# ---- ultramodern: all of it (none of it is tied to SDL or RT64) ----
set(UM "${RT}/ultramodern/src")
add_library(ultramodern STATIC
  ${UM}/audio.cpp ${UM}/error_handling.cpp ${UM}/events.cpp ${UM}/extensions.cpp ${UM}/input.cpp ${UM}/mesgqueue.cpp
  ${UM}/misc_ultra.cpp ${UM}/renderer_context.cpp ${UM}/rsp.cpp ${UM}/scheduling.cpp ${UM}/threadqueue.cpp
  ${UM}/threads.cpp ${UM}/timer.cpp ${UM}/ultrainit.cpp)
target_include_directories(ultramodern PRIVATE ${RECOMP_INCLUDES})
target_compile_options(ultramodern PRIVATE -Wno-unused-parameter)

# ---- librecomp without mods, the live recompiler, sljit, the mod heap or math_routines (§2.2, §3.5 risk 5) ----
set(LR "${RT}/librecomp/src")
add_library(librecomp STATIC
  ${LR}/ai.cpp ${LR}/cont.cpp ${LR}/config_option.cpp ${LR}/config.cpp ${LR}/dp.cpp ${LR}/eep.cpp ${LR}/euc-jp.cpp
  ${LR}/extensions.cpp ${LR}/files.cpp ${LR}/flash.cpp ${LR}/overlays.cpp ${LR}/pak.cpp ${LR}/pi.cpp ${LR}/print.cpp
  ${LR}/recomp.cpp ${LR}/rsp.cpp ${LR}/sp.cpp ${LR}/ultra_stubs.cpp ${LR}/ultra_translation.cpp ${LR}/vi.cpp)
target_include_directories(librecomp PRIVATE ${RECOMP_INCLUDES} "${RT}/librecomp/src" "${RT}/librecomp/include/librecomp")
target_compile_options(librecomp PRIVATE -Wno-unused-parameter)

# ---- The app: Tier 0 game code (both flavours), Classic's own, Tier 2 that compiles against recompui-lite, the PSP layer ----
set(GAME_TIER0
  ${GAME}/threads.cpp ${GAME}/overlays.cpp ${GAME}/game_init.cpp ${GAME}/ai.cpp
  ${GAME}/controller_pak.cpp ${GAME}/debug_print.cpp ${GAME}/notices.cpp
  ${MAIN}/register_overlays.cpp ${MAIN}/dev_monitor.cpp
  ${GAME}/play_log.cpp)
# ultra_extras.cpp's two clocks are psp_main.cpp's (they have a lockstep path).
set(GAME_CLASSIC ${GAME}/content_pack.cpp ${GAME}/team_store.cpp ${GAME}/team_store_original.cpp)
# Modern's Tier 1 (§2.2). rosters_live.cpp (SDL, its own thread and JSON player file) is replaced on the PSP by
# psp_rosters.cpp over psp/pack.txt (§6); art_dropin.cpp (it rewrites the whole ROM in memory) runs only in psp-host's
# --make-kit, which merges the art into the kit's ROM; the EBOOT reads that ROM's file table instead (psp_artfs.cpp).
set(GAME_MODERN
  ${GAME}/content_pack_tp.cpp ${GAME}/season32.cpp ${GAME}/adv_mode.cpp ${GAME}/adv_season.cpp ${GAME}/adv_save.cpp
  ${GAME}/adv_engine.cpp ${GAME}/known_roms.cpp ${GAME}/team_select.cpp)
if(IS_MODERN)
  list(APPEND GAME_CLASSIC ${GAME_MODERN})
endif()
# Tier 2: compiled against recompui-lite. Not compiled (their hooks are in src/psp/stubs_hooks.cpp): gfx_ext (RT64's
# extended GBI), receiver_targets (SDL, RmlUi), savestate, crew, ds_*, art_dropin (rewrites the whole ROM in memory),
# config (SDL, the settings UI), controllers, dev_input (SDL).
set(GAME_TIER2
  ${GAME}/skin_tones.cpp ${GAME}/house_rules.cpp ${GAME}/cpu_smart.cpp ${GAME}/opponent_select.cpp ${GAME}/mixed_uniforms.cpp
  ${GAME}/checkpoint.cpp ${GAME}/tournament.cpp ${GAME}/uniform_picker.cpp ${GAME}/button_glyphs.cpp ${GAME}/board_digits.cpp
  ${GAME}/engine32.cpp ${GAME}/content_dirs.cpp ${GAME}/season_tiebreak.cpp ${GAME}/season_settings.cpp ${GAME}/adv_league.cpp)
set(PSP_LAYER
  ${PSP_SRC}/psp_main.cpp ${PSP_SRC}/psp_sys.cpp ${PSP_SRC}/psp_log.cpp
  ${PSP_SRC}/stubs_hooks.cpp ${PSP_SRC}/psp_settings.cpp
  ${PSP_SRC}/recompui_lite/lite.cpp
  ${PSP_SRC}/backends_common.cpp)     # null renderer counters, silent audio, script input: the same on both
if(IS_PSP)
  list(APPEND PSP_LAYER
    ${PSP_SRC}/psp_threads.cpp ${PSP_SRC}/psp_mem.cpp ${PSP_SRC}/psp_rom.cpp ${PSP_SRC}/psp_input.cpp
    ${PSP_SRC}/psp_overlay.cpp ${PSP_SRC}/psp_stdio.cpp ${PSP_SRC}/psp_tls.cpp
    ${PSP_SRC}/psp_gfx.cpp                                       # the GE renderer in the game (M7)
    ${PSP_SRC}/gfx/gbi.cpp ${PSP_SRC}/gfx/tex.cpp ${PSP_SRC}/gfx/combiner.cpp ${PSP_SRC}/gfx/ge_psp.cpp
    ${PSP_SRC}/psp_http.cpp ${PSP_SRC}/psp_net.cpp ${PSP_SRC}/../common/sha256.cpp ${PSP_SRC}/psp_remote.cpp
    ${PSP_SRC}/nbremote/nbr_launch_psp.c                         # restart and home=remote (NB Remote): ARK's launcher call
    ${PSP_SRC}/psp_audio.cpp)                                    # audio v1 (M8, §4.6): n_aspMain + sceAudioSRC
  set_source_files_properties(${PSP_SRC}/nbremote/nbr_launch_psp.c PROPERTIES COMPILE_DEFINITIONS NBR_CFW_IMPORTS=1)
  # The audio microcode, RSPRecomp's output (make rsp): the SISD vector unit (rsp_vu.hpp without SSE4.1). BLITZ_PSP_AUDIO
  # turns the real output on (backends_common.cpp's silent sink stays, for the noaudio switch).
  set(RSP_ASP "${BLITZ_ROOT}/build/rsp/n_aspMain.cpp")
  if(NOT EXISTS "${RSP_ASP}")
    message(FATAL_ERROR "${RSP_ASP} is missing: run make rsp")
  endif()
  list(APPEND PSP_LAYER ${RSP_ASP})
  add_compile_definitions(BLITZ_PSP_AUDIO=1)
else()
  list(APPEND PSP_LAYER ${PSP_SRC}/host/host_backends.cpp)
endif()
if(MEMCHECK)
  list(APPEND PSP_LAYER ${PSP_SRC}/psp_memcheck.cpp)
endif()
if(IS_MODERN)
  # §6: the weekly rosters over psp/pack.txt (psp-host fetches with the same HTTP client, from BLITZ_ROSTER_SOURCE)
  list(APPEND PSP_LAYER ${PSP_SRC}/psp_rosters.cpp)
  if(NOT IS_PSP)
    list(APPEND PSP_LAYER ${PSP_SRC}/psp_http.cpp ${PSP_SRC}/../common/sha256.cpp)
  endif()
endif()
list(REMOVE_ITEM PSP_LAYER ${PSP_SRC}/psp_main.cpp)

if(IS_PSP)
  set(APP newblitz)
else()
  set(APP newblitz-host)
endif()
# Everything but the boot file and the added-files layer, once, for both executables of a Modern psp-host build.
add_library(app_objs OBJECT ${GAME_TIER0} ${GAME_CLASSIC} ${GAME_TIER2} ${PSP_LAYER})
target_include_directories(app_objs PRIVATE ${RECOMP_INCLUDES} "${FUNCS_DIR}" "${GAME}")
target_compile_options(app_objs PRIVATE -fms-extensions)
if(NOT IS_PSP)
  target_compile_definitions(app_objs PRIVATE BLITZ_SOURCE_ROOT="${BLITZ_ROOT}")
endif()
set(APP_ONLY ${PSP_SRC}/psp_main.cpp)
if(IS_MODERN)
  list(APPEND APP_ONLY ${PSP_SRC}/psp_artfs.cpp)   # the kit ROM's file table (§4.3)
endif()
add_executable(${APP} $<TARGET_OBJECTS:app_objs> ${APP_ONLY})
target_include_directories(${APP} PRIVATE ${RECOMP_INCLUDES} "${FUNCS_DIR}" "${GAME}")
target_compile_options(${APP} PRIVATE -fms-extensions)
if(IS_MODERN AND NOT IS_PSP)
  # newblitz-kit (psp-host --make-kit, §4.3): the same core with the desktop's art_dropin.cpp, which merges Modern's art
  # into the ROM in memory at on_init; psp_main.cpp then writes that ROM and what the merge left outside it into the kit.
  add_executable(newblitz-kit $<TARGET_OBJECTS:app_objs> ${PSP_SRC}/psp_main.cpp ${GAME}/art_dropin.cpp)
  target_include_directories(newblitz-kit PRIVATE ${RECOMP_INCLUDES} "${FUNCS_DIR}" "${GAME}")
  target_compile_options(newblitz-kit PRIVATE -fms-extensions)
  target_compile_definitions(newblitz-kit PRIVATE BLITZ_SOURCE_ROOT="${BLITZ_ROOT}" BLITZ_PSP_KIT=1)
  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  find_package(Threads REQUIRED)
  target_link_libraries(newblitz-kit PRIVATE RecompiledFuncs librecomp ultramodern Threads::Threads)
endif()

if(IS_PSP)
  # The archives are cyclic (the game's code calls the runtime and the hooks, the runtime calls the game's entry): GNU ld
  # needs them in one group. Every PSP library once (§14.4: psp-gcc's specs already add psputility, psprtc, pspnet_inet,
  # pspnet_resolver, pspsdk, pspmodinfo and pspuser after libcglue; naming one again binds the wrong stubs).
  target_link_libraries(${APP} PRIVATE
    "-Wl,--start-group" RecompiledFuncs librecomp ultramodern "-Wl,--end-group"
    pspgu pspdebug pspdisplay pspge pspctrl pspaudio psppower pspvfpu
    pspnet pspnet_apctl pspwlan pspsystemctrl_user
    pspfpu stdc++ atomic pthread)
  target_link_options(${APP} PRIVATE -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/newblitz.map)
  if(IS_MODERN)
    # BRANDING: PSP XMB title
    create_pbp_file(TARGET ${APP} TITLE "New Blitz" BUILD_PRX MEMSIZE 1)
  else()
    # BRANDING: PSP XMB title
    create_pbp_file(TARGET ${APP} TITLE "New Blitz Classic" BUILD_PRX MEMSIZE 1)
  endif()
  # newblitz.elf: the linked ELF with its symbols and line tables, for psp-addr2line (the PRX is made from the same file).
  add_custom_command(TARGET ${APP} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${APP}>" "${CMAKE_BINARY_DIR}/newblitz.elf"
    COMMENT "newblitz.elf for psp-addr2line")
else()
  target_compile_definitions(${APP} PRIVATE BLITZ_SOURCE_ROOT="${BLITZ_ROOT}")
  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  find_package(Threads REQUIRED)
  # Apple's ld resolves the cycles between the archives on its own.
  target_link_libraries(${APP} PRIVATE RecompiledFuncs librecomp ultramodern Threads::Threads)
  if(NOT GPR32)
    set_target_properties(${APP} PROPERTIES OUTPUT_NAME newblitz-host-stock)
  endif()
endif()
