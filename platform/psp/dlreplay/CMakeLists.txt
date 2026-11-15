# M6 (docs/PSP_PORT.md 4.5): the sceGu renderer v1 and its display-list replay.
#   make psp-dlreplay    -> build/psp/dlreplay/EBOOT.PBP: renders <app>/dumps/rdram_*.bin through src/psp/gfx and writes
#                           <app>/shots/<dump>.shot (BLITZSHOT1, tools/psp/shot2png.py) and timing lines
#   make psp-gfxtest     -> build/psp/gfxtest/gfx-dltest: the same walker on the Mac with the null GE backend
#                           (tools/psp/gbi_check.py compares its decode with tools/gbi_dump.py's)
cmake_minimum_required(VERSION 3.16)
project(blitz_psp_dlreplay C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(BLITZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PSP_SRC "${BLITZ_ROOT}/src/psp")

execute_process(COMMAND git -C "${BLITZ_ROOT}" rev-list --count HEAD OUTPUT_VARIABLE BLITZ_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${BLITZ_ROOT}" rev-parse --short=7 HEAD OUTPUT_VARIABLE BLITZ_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BLITZ_BUILD)
  set(BLITZ_BUILD 0)
endif()
if(NOT BLITZ_COMMIT)
  set(BLITZ_COMMIT unknown)
endif()

# The renderer core: the same files on both targets; only the GE backend differs.
set(GFX_CORE
  ${PSP_SRC}/gfx/gbi.cpp
  ${PSP_SRC}/gfx/tex.cpp
  ${PSP_SRC}/gfx/combiner.cpp)

if(PSP)
  add_executable(nbdlreplay ${PSP_SRC}/dlreplay/dlreplay_main.cpp ${GFX_CORE} ${PSP_SRC}/gfx/ge_psp.cpp
    ${PSP_SRC}/psp_sys.cpp ${PSP_SRC}/psp_log.cpp)
  target_include_directories(nbdlreplay PRIVATE ${PSP_SRC})
  target_compile_definitions(nbdlreplay PRIVATE BLITZ_BUILD="${BLITZ_BUILD}" BLITZ_COMMIT="${BLITZ_COMMIT}")
  # -Wno-format: psp-gcc's uint32_t is unsigned long, which %u/%X print correctly on a 32-bit target
  target_compile_options(nbdlreplay PRIVATE -O2 -G0 -Wall -Wno-format -Wno-unused-function -fno-strict-aliasing)
  # Every PSP library once (docs/PSP_PORT.md 14.4: the specs already add psputility, psprtc, pspnet_inet, ...).
  target_link_libraries(nbdlreplay PRIVATE pspgu pspge pspdisplay pspctrl psppower pspfpu stdc++)
  create_pbp_file(TARGET nbdlreplay TITLE "New Blitz DL replay" BUILD_PRX MEMSIZE 1)
else()
  add_executable(gfx-dltest ${PSP_SRC}/dlreplay/dltest_host.cpp ${GFX_CORE} ${PSP_SRC}/gfx/ge_null.cpp)
  target_include_directories(gfx-dltest PRIVATE ${PSP_SRC})
  target_compile_options(gfx-dltest PRIVATE -Wall -Wextra -Wno-unused-parameter -O1 -g)
endif()
