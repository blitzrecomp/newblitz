#!/usr/bin/env bash
# Links BlitzRecompiled as a PS5 title's eboot.bin (docs/PS5_HOMEBREW.md). CMake runs it as the target's link launcher
# (RULE_LAUNCH_LINK): it gets the payload SDK's ordinary link command, keeps its objects and libraries, and links them
# the way UnleashedRecomp-Prospero links a title with RADV (its tools/radv-link.sh and tools/build.sh, GPL-3.0, used
# from the checkout tools/ps5/setup_sdk.sh makes in build/ps5/deps): a PIE with the PS5 start-up code and Mesa's RADV
# linked whole, converted for the console's loader and signed as a fake SELF by ps5-native-tool.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ps5=$root/build/ps5
export LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm/bin/llvm-config}  # the SDK's compiler wrappers find LLVM through it
sdk=$ps5/sdk-m
unleashed=$ps5/deps/unleashed-prospero
radv=$ps5/radv/lib/libvulkan_radeon.ps5.a
tool=$ps5/host/ps5-native-tool
for f in "$sdk/bin/prospero-lld" "$unleashed/tools/radv-link.sh" "$radv" "$tool"; do
    [[ -e $f ]] || { echo "link_title.sh: missing $f (tools/ps5/setup_sdk.sh, tools/ps5/build_radv.sh)" >&2; exit 2; }
done

# The command CMake would have run: <compiler> <flags> -o <out> <objects> <libraries>.
shift  # the compiler
out=""
objects=()
archives=()
libdirs=("$ps5/prefix/lib")
libs=()
while (($#)); do
    case $1 in
        -o) out=$2; shift ;;
        *.o|*.obj) objects+=("$1") ;;
        *.a) archives+=("$1") ;;
        -L*) libdirs+=("${1#-L}") ;;
        -l*) libs+=("${1#-l}") ;;
    esac
    shift
done
[[ -n $out ]] || { echo "link_title.sh: no -o in the link command" >&2; exit 2; }
# -lfoo: a static library from our prefix joins the archives; the system's (libSceX.so) come with the SDK's stubs below.
for lib in "${libs[@]}"; do
    for dir in "${libdirs[@]}"; do
        if [[ -f $dir/lib$lib.a ]]; then archives+=("$dir/lib$lib.a"); break; fi
    done
done

work=$PWD/ps5-link  # the build folder, outside the title folder
mkdir -p "$work"
cxx=("$sdk/bin/prospero-clang++")
cc=("$sdk/bin/prospero-clang")
native=$unleashed/tooling/native

# The title's entry point (_start: IEEE float state, the SDK's environment, constructors, main).
"${cxx[@]}" -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/app_crt.o"
# zstd's optional trace hooks, defined here so the converter doesn't look for them in a system module.
"${cc[@]}" -std=c11 -O2 -fPIC -c "$native/zstd_trace_stubs.c" -o "$work/zstd_trace_stubs.o"
# RADV's PS5 window system imports the AGC graphics modules: link-time stand-ins that name them.
for pair in libSceAgc:agc_canary_link_stub libSceAgcDriver:agc_driver_canary_link_stub; do
    library=${pair%%:*}
    "${cc[@]}" -std=c11 -O2 -fPIC -c "$unleashed/vendor/ps5/sdk/stubs/${pair#*:}.c" -o "$work/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/$library.so" "$work/$library.o"
done

# shellcheck source=/dev/null
source "$unleashed/tools/radv-link.sh"
PS5_CLANG="$sdk/bin/clang" radv_link_recipe "$unleashed" "$sdk" "$radv"

# A title can't use the kernel's exit: those go to the system shell (platform_ps5.cpp, catchReturnFromMain).
radv_link_flags+=(--wrap=exit --wrap=_Exit --wrap=quick_exit)
# No debug info in the title: a third the size to copy, and the console has no debugger here.
radv_link_flags+=(--strip-debug)

"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/app_crt.o" "$work/zstd_trace_stubs.o" "${objects[@]}" \
    --start-group "${archives[@]}" --end-group \
    "$work/libSceAgc.so" "$work/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so

# The recipe lets undefined symbols through (the console's modules resolve them when the title loads), so an app
# function that was never built would only fail on the console: catch our own here.
nm_tool="$(command -v /opt/homebrew/opt/llvm/bin/llvm-nm || command -v llvm-nm || true)"
if [ -n "$nm_tool" ]; then
    missing="$("$nm_tool" -u -C "$work/llvm-pie.elf" | grep -E 'blitz::|blitz_' || true)"
    if [ -n "$missing" ]; then
        echo "link_title.sh: undefined app symbols:" >&2
        echo "$missing" >&2
        exit 1
    fi
fi

# The console's loader: SCE dynamic metadata (module SDK 2.00, companion libc 8.05), then a fake-signed SELF.
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$work/libSceAgc.so" --stub "$work/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$out" --magic 0x1D3D154F
