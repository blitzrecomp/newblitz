# NB Remote (docs/PSP_PORT.md "NB Remote"): src/psp/nbremote.
#   make psp-remote        -> build/psp/nbremote/EBOOT.PBP ("New Blitz Remote", PSP/GAME/NBREMOTE) and, through
#                             platform/psp/nbremote/plugin/Makefile, build/psp/nbremote_plugin/nbremote.prx (+ nbremote_net.prx)
#   make psp-remote-test   -> build/psp/nbremote_host/nbremote-host: the same core on the Mac, run against the mock Worker
cmake_minimum_required(VERSION 3.16)
project(blitz_psp_nbremote C)

set(CMAKE_C_STANDARD 99)
get_filename_component(BLITZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(NBR_SRC "${BLITZ_ROOT}/src/psp/nbremote")

execute_process(COMMAND git -C "${BLITZ_ROOT}" rev-list --count HEAD OUTPUT_VARIABLE BLITZ_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${BLITZ_ROOT}" rev-parse --short=7 HEAD OUTPUT_VARIABLE BLITZ_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BLITZ_BUILD)
  set(BLITZ_BUILD 0)
endif()
if(NOT BLITZ_COMMIT)
  set(BLITZ_COMMIT unknown)
endif()

set(NBR_CORE ${NBR_SRC}/nbr_core.c ${NBR_SRC}/nbr_text.c ${NBR_SRC}/nbr_sha256.c ${NBR_SRC}/nbr_http.c)

if(PSP)
  add_executable(nbremote ${NBR_SRC}/nbr_eboot.c ${NBR_CORE} ${NBR_SRC}/nbr_plat_psp.c ${NBR_SRC}/nbr_net_psp.c ${NBR_SRC}/nbr_launch_psp.c)
  target_compile_definitions(nbremote PRIVATE NBR_BUILD="${BLITZ_BUILD} ${BLITZ_COMMIT}" NBR_CFW_IMPORTS=1)
  target_compile_options(nbremote PRIVATE -O2 -G0 -Wall -Wextra -Wno-unused-parameter)
  # psp-gcc's specs add psputility, psprtc, pspnet_inet, pspnet_resolver, pspuser and friends already (see the probe's note:
  # naming one twice links its stub module twice)
  target_link_libraries(nbremote PRIVATE pspdebug pspdisplay pspge pspctrl psppower pspnet pspnet_apctl pspwlan pspsystemctrl_user)
  # BRANDING: PSP XMB title
  create_pbp_file(TARGET nbremote TITLE "New Blitz Remote" BUILD_PRX)
else()
  add_executable(nbremote-host ${NBR_SRC}/nbr_host_main.c ${NBR_CORE} ${NBR_SRC}/nbr_plat_host.c)
  target_compile_definitions(nbremote-host PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_compile_options(nbremote-host PRIVATE -Wall -Wextra -Wno-unused-parameter -O1 -g)
  # the VSH plugin's decision logic (when it starts NB Remote, when it stays in the XMB), run on the Mac
  add_executable(nbremote-plugin-host ${NBR_SRC}/nbr_plugin_host_main.c ${NBR_SRC}/nbr_plugin.c ${NBR_CORE} ${NBR_SRC}/nbr_plat_host.c)
  target_compile_definitions(nbremote-plugin-host PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE NBR_HOST=1 NBR_PLUGIN_NET=0)
  target_compile_options(nbremote-plugin-host PRIVATE -Wall -Wextra -Wno-unused-parameter -O1 -g)
endif()
