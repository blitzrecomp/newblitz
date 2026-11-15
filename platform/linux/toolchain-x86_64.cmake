# Cross-compiling the Linux x86_64 build (Steam Deck, Ubuntu, Fedora) on a Mac: Homebrew LLVM's clang and lld against a
# Debian 12 (glibc 2.36) sysroot with libstdc++ and SDL2/Vulkan/FreeType headers and libs, unpacked by a script:
#   tools/linux/make_sysroot.sh
#   cmake -S . -B build/linux_x86_64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=platform/linux/toolchain-x86_64.cmake
# This checks that the code compiles and links for Linux; running it needs a Linux PC.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(BLITZ_LLVM_BIN "/opt/homebrew/opt/llvm/bin" CACHE PATH "LLVM with clang")
set(BLITZ_LINUX_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../build/linux/sysroot-x86_64" CACHE PATH "tools/linux/make_sysroot.sh output")
get_filename_component(BLITZ_LINUX_SYSROOT "${BLITZ_LINUX_SYSROOT}" ABSOLUTE)

set(CMAKE_SYSROOT "${BLITZ_LINUX_SYSROOT}")
set(CMAKE_C_COMPILER "${BLITZ_LLVM_BIN}/clang")
set(CMAKE_CXX_COMPILER "${BLITZ_LLVM_BIN}/clang++")
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_AR "${BLITZ_LLVM_BIN}/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${BLITZ_LLVM_BIN}/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${BLITZ_LLVM_BIN}/llvm-strip" CACHE FILEPATH "")

# libstdc++ 12 from the sysroot (not libc++), lld, and the sysroot's gcc 12 runtime files.
set(_gcc "${BLITZ_LINUX_SYSROOT}/usr/lib/gcc/x86_64-linux-gnu/12")
set(CMAKE_C_FLAGS_INIT "--gcc-install-dir=${_gcc}")
set(CMAKE_CXX_FLAGS_INIT "--gcc-install-dir=${_gcc} -stdlib=libstdc++")
foreach(_k EXE SHARED MODULE)
  set(CMAKE_${_k}_LINKER_FLAGS_INIT "-fuse-ld=lld --gcc-install-dir=${_gcc} -stdlib=libstdc++ -Wl,--allow-shlib-undefined")
endforeach()

# Programs (cmake helpers, python) come from the Mac; libraries, headers and packages only from the sysroot.
set(CMAKE_FIND_ROOT_PATH "${BLITZ_LINUX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config reads the sysroot's .pc files and prefixes their paths with it.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${BLITZ_LINUX_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${BLITZ_LINUX_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${BLITZ_LINUX_SYSROOT}/usr/share/pkgconfig")
