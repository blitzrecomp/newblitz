# PS5 homebrew, milestone 1 (docs/PS5_HOMEBREW.md): the game side builds for the console. N64ModernRuntime
# (ultramodern, librecomp), Classic's recompiled code and the RSP audio microcode as PS5 static libraries, with the
# open PS5 payload SDK (build/ps5/sdk; its toolchain/prospero.cmake). No SDL, RT64 or app yet: those come with their
# console ports (SDL2's PS5 port; RT64 on plume-ps5 and a static RADV).
#   export PS5_PAYLOAD_SDK=$PWD/build/ps5/sdk/ps5-payload-sdk LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
#   cmake -S platform/ps5 -B build/ps5/probe -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/toolchain/prospero.cmake
#   cmake --build build/ps5/probe
cmake_minimum_required(VERSION 3.20)
project(BlitzPS5 C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BLITZ_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
get_filename_component(BLITZ_ROOT ${BLITZ_ROOT} ABSOLUTE)
set(GENERATED_DIR ${BLITZ_ROOT}/build CACHE PATH "Recompiler output")
set(FUNCS_DIR ${GENERATED_DIR}/RecompiledFuncs CACHE PATH "Classic's recompiled code")

# The console has no native thread-local storage for loaded code: emulated TLS (as UnleashedRecomp-Prospero).
# Its Zen 2 CPU has SSE4.1 (the RSP's vector code).
add_compile_options(-femulated-tls -msse4.1 -fno-strict-aliasing)

set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${BLITZ_ROOT}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/runtime)

set(RECOMP_INCLUDES
    ${BLITZ_ROOT}/include
    ${BLITZ_ROOT}/lib/N64ModernRuntime/ultramodern/include
    ${BLITZ_ROOT}/lib/N64ModernRuntime/librecomp/include
    ${BLITZ_ROOT}/lib/N64ModernRuntime/N64Recomp/include)

add_library(RecompiledFuncs STATIC)
file(GLOB FUNC_C_SOURCES ${FUNCS_DIR}/*.c)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES})
target_compile_options(RecompiledFuncs PRIVATE -Wno-unused-variable -Wno-implicit-function-declaration -falign-functions=16)
target_include_directories(RecompiledFuncs PRIVATE ${RECOMP_INCLUDES})

add_library(rsp_audio STATIC ${GENERATED_DIR}/rsp/n_aspMain.cpp)
target_include_directories(rsp_audio PRIVATE ${RECOMP_INCLUDES} ${BLITZ_ROOT}/lib/N64ModernRuntime/thirdparty)

# Milestone 2: RT64 for the console (Plume's PS5 display surface: platform/plume-patches/0002). SDL2 from
# tools/ps5/setup_sdk.sh; RT64's shaders and file_to_c run on the Mac (platform/rt64-patches/0004).
option(BLITZ_PS5_RT64 "Build RT64 for the PS5" ON)
if (BLITZ_PS5_RT64)
    list(APPEND CMAKE_PREFIX_PATH ${BLITZ_ROOT}/build/ps5/prefix)
    set(SDL2_DIR ${BLITZ_ROOT}/build/ps5/prefix/lib/cmake/SDL2)
    set(RT64_STATIC TRUE)
    set(RT64_NO_FILE_DIALOG TRUE)  # no native file dialog on the console (rt64 patch 0004)
    add_compile_definitions(HLSL_CPU __PRFCHWINTRIN_H)
    set(RT64_HOST_DXC "DYLD_LIBRARY_PATH=${BLITZ_ROOT}/lib/rt64/src/contrib/dxc/lib/arm64" "${BLITZ_ROOT}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos")
    set(RT64_HOST_FILE_TO_C ${CMAKE_BINARY_DIR}/host/file_to_c)
    if (NOT EXISTS ${RT64_HOST_FILE_TO_C})
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/host)
        execute_process(COMMAND xcrun c++ -std=c++17 -O2 ${BLITZ_ROOT}/lib/rt64/src/tools/file_to_c/file_to_c.cpp -o ${RT64_HOST_FILE_TO_C})
    endif()
    add_subdirectory(${BLITZ_ROOT}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)
endif()
