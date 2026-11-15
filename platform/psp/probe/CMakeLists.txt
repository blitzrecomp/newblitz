# The PSP probe (docs/PSP_PORT.md §7) and, on the Mac, psp-nettest: the same log/HTTP/remote code against tools/psp/mock_worker.py.
#   make psp-probe      -> build/psp/probe/EBOOT.PBP (+ nbprobe.prx for PSPLink, nbprobe for psp-addr2line)
#   make psp-nettest    -> build/psp/nettest/psp-nettest
cmake_minimum_required(VERSION 3.16)
project(blitz_psp_probe C CXX)

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

set(PSP_COMMON
  ${PSP_SRC}/psp_sys.cpp
  ${PSP_SRC}/psp_log.cpp
  ${PSP_SRC}/psp_http.cpp
  ${PSP_SRC}/psp_net.cpp
  ${PSP_SRC}/../common/sha256.cpp
  ${PSP_SRC}/psp_remote.cpp)

if(PSP)
  option(PROBE_CFW_IMPORTS "Import kuKernelGetModel / sctrlHENGetVersion (the EBOOT won't load without a CFW that exports them)" OFF)

  # Section 4's build-time flag: does RSPRecomp's n_aspMain.cpp compile with psp-g++? (a configure-time compile, -O1, with
  # RECOMP_GPR32=1 as every PSP TU will have it)
  set(PROBE_N_ASPMAIN 0)
  set(PROBE_N_ASPMAIN_ERR "build/rsp/n_aspMain.cpp missing (make rsp)")
  if(EXISTS "${BLITZ_ROOT}/build/rsp/n_aspMain.cpp")
    set(_rt "${BLITZ_ROOT}/lib/N64ModernRuntime")
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -O1 -G0 -D__PSP__ -DPSP -DRECOMP_GPR32=1 -I${PSPDEV}/psp/include -I${PSPDEV}/psp/sdk/include
              -I${_rt}/librecomp/include -I${_rt}/ultramodern/include -I${_rt}/N64Recomp/include -I${_rt}/thirdparty
              -c "${BLITZ_ROOT}/build/rsp/n_aspMain.cpp" -o "${CMAKE_CURRENT_BINARY_DIR}/n_aspMain_try.o"
      RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
    if(_rc EQUAL 0)
      set(PROBE_N_ASPMAIN 1)
      set(PROBE_N_ASPMAIN_ERR "compiled (-O1)")
    else()
      string(REGEX MATCH "error: [^\n]*" _first "${_err}")
      string(REPLACE "\"" "'" _first "${_first}")
      set(PROBE_N_ASPMAIN_ERR "${_first}")
    endif()
    message(STATUS "probe: n_aspMain.cpp with psp-g++: ${PROBE_N_ASPMAIN} (${PROBE_N_ASPMAIN_ERR})")
  endif()

  add_executable(nbprobe ${PSP_SRC}/probe/probe_main.cpp ${PSP_COMMON} ${PSP_SRC}/nbremote/nbr_launch_psp.c)
  set_source_files_properties(${PSP_SRC}/nbremote/nbr_launch_psp.c PROPERTIES COMPILE_DEFINITIONS NBR_CFW_IMPORTS=1)
  target_include_directories(nbprobe PRIVATE ${PSP_SRC}
    ${BLITZ_ROOT}/lib/N64ModernRuntime/thirdparty/concurrentqueue
    ${BLITZ_ROOT}/lib/N64ModernRuntime/thirdparty/json)
  target_compile_definitions(nbprobe PRIVATE BLITZ_BUILD="${BLITZ_BUILD}" BLITZ_COMMIT="${BLITZ_COMMIT}"
    PROBE_N_ASPMAIN=${PROBE_N_ASPMAIN} PROBE_N_ASPMAIN_ERR="${PROBE_N_ASPMAIN_ERR}"
    PROBE_CFW_IMPORTS=$<BOOL:${PROBE_CFW_IMPORTS}>)
  target_compile_options(nbprobe PRIVATE -O2 -G0 -Wall -Wno-unused-function -fno-strict-aliasing)
  set(_cfw pspsystemctrl_user)   # restart / home=remote (nbr_launch_psp.c); PPSSPP loads it (NB Remote's runs, 2026-11-05)
  if(PROBE_CFW_IMPORTS)
    set(_cfw pspkubridge pspsystemctrl_user)
  endif()
  # Every PSP library once. psp-gcc's specs already add "-lpsputility -lpsprtc -lpspnet_inet -lpspnet_resolver -lpspsdk
  # -lpspmodinfo -lpspuser" after libcglue; naming one of those here as well links its stub module twice, and the loader then
  # binds the second block's functions to the wrong stubs (found in PPSSPP: send/recv returned garbage, and the loader logged
  # "Unknown syscall from known HLE module 'sceNetInet'"). So those stay out of this list.
  target_link_libraries(nbprobe PRIVATE
    pspgu pspdebug pspdisplay pspge pspctrl pspaudio psppower pspmath pspvfpu
    pspnet pspnet_apctl pspwlan ${_cfw}
    pspfpu stdc++ atomic pthread)
  # BRANDING: PSP XMB title
  create_pbp_file(TARGET nbprobe TITLE "New Blitz Probe" BUILD_PRX MEMSIZE 1)
else()
  add_executable(psp-nettest ${BLITZ_ROOT}/src/psp/host/nettest_main.cpp ${PSP_COMMON})
  target_include_directories(psp-nettest PRIVATE ${PSP_SRC})
  target_compile_definitions(psp-nettest PRIVATE BLITZ_BUILD="${BLITZ_BUILD}" BLITZ_COMMIT="${BLITZ_COMMIT}")
  target_compile_options(psp-nettest PRIVATE -Wall -O1 -g)
  find_package(Threads REQUIRED)
  target_link_libraries(psp-nettest PRIVATE Threads::Threads)
endif()
