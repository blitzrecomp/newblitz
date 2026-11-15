# Cross-compiling the Windows x64 build on a Mac (docs/WINDOWS_VULKAN.md): Homebrew LLVM's clang-cl and lld-link with
# Microsoft's CRT and Windows SDK unpacked by xwin (Microsoft's license accepted by the person running it):
#   xwin --accept-license --arch x86_64 --cache-dir build/xwin-cache splat --output build/xwin
#   cmake -S . -B build/win_classic -G Ninja -DCMAKE_TOOLCHAIN_FILE=platform/windows/toolchain-xwin.cmake
# This checks that the code compiles and links for Windows; running it needs a Windows PC.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(BLITZ_LLVM_BIN "/opt/homebrew/opt/llvm/bin" CACHE PATH "LLVM with clang-cl")
set(BLITZ_XWIN "${CMAKE_CURRENT_LIST_DIR}/../../build/xwin" CACHE PATH "xwin splat output")
get_filename_component(BLITZ_XWIN "${BLITZ_XWIN}" ABSOLUTE)

set(CMAKE_C_COMPILER "${BLITZ_LLVM_BIN}/clang-cl")
set(CMAKE_CXX_COMPILER "${BLITZ_LLVM_BIN}/clang-cl")
set(CMAKE_LINKER "/opt/homebrew/bin/lld-link")
set(CMAKE_AR "${BLITZ_LLVM_BIN}/llvm-lib")
set(CMAKE_RC_COMPILER "${BLITZ_LLVM_BIN}/llvm-rc")
set(CMAKE_MT "${BLITZ_LLVM_BIN}/llvm-mt")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_inc "/imsvc${BLITZ_XWIN}/crt/include /imsvc${BLITZ_XWIN}/sdk/include/ucrt /imsvc${BLITZ_XWIN}/sdk/include/um /imsvc${BLITZ_XWIN}/sdk/include/shared /imsvc${BLITZ_XWIN}/sdk/include/winrt")
set(CMAKE_C_FLAGS_INIT "${_inc}")
set(CMAKE_CXX_FLAGS_INIT "${_inc} /EHsc")
set(_lib "/libpath:${BLITZ_XWIN}/crt/lib/x86_64 /libpath:${BLITZ_XWIN}/sdk/lib/um/x86_64 /libpath:${BLITZ_XWIN}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")

# Programs built for Windows can't run here: the build host's tools come from the Mac (CMAKE_HOST_APPLE branches).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# xwin unpacks the release CRT only, so every configuration (and CMake's own test programs) uses it; linked statically
# (libcmt/libcpmt), so the app runs on a PC without the Visual C++ Redistributable.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
# RT64's bundled SDL2 package file asks for CMake 2.x-era behaviour, which CMake 4 refuses without this.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
