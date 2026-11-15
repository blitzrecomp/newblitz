# Windows x64 staging (docs/WINDOWS_VULKAN.md): the app's resources and the DLLs it loads, next to the .exe, as
# platform/macos/apple_bundle.cmake puts them in the bundle. One engine per folder for now (no starter yet).
set(BLITZ_WIN_DEPS ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/mupen64plus-win32-deps)
# BRANDING: Windows exe name, from BLITZ_BUNDLE_NAME
set_target_properties(BlitzRecompiled PROPERTIES OUTPUT_NAME "${BLITZ_BUNDLE_NAME}")  # "Classic Blitz.exe" / "Modern Blitz.exe"
target_compile_definitions(BlitzRecompiled PRIVATE SDL_MAIN_HANDLED)
target_sources(BlitzRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/platform/windows/blitz.manifest)  # UTF-8 code page

# The app icon (Explorer, the taskbar): the Mac's icon PNG as a multi-size .ico, built into the .exe as a resource.
find_program(BLITZ_PYTHON python3 REQUIRED)  # with Pillow, as the other art tools (tools/make_icons.py)
set(BLITZ_ICO ${CMAKE_BINARY_DIR}/blitz.ico)
add_custom_command(OUTPUT ${BLITZ_ICO}
    COMMAND ${BLITZ_PYTHON} ${CMAKE_SOURCE_DIR}/tools/windows/make_ico.py ${BLITZ_APP_ICON} ${BLITZ_ICO}
    DEPENDS ${BLITZ_APP_ICON} ${CMAKE_SOURCE_DIR}/tools/windows/make_ico.py
    COMMENT "App icon for Windows"
    VERBATIM)
file(WRITE ${CMAKE_BINARY_DIR}/blitz_icon.rc "1 ICON \"blitz.ico\"\n")
set_source_files_properties(${CMAKE_BINARY_DIR}/blitz_icon.rc PROPERTIES OBJECT_DEPENDS ${BLITZ_ICO})
target_sources(BlitzRecompiled PRIVATE ${CMAKE_BINARY_DIR}/blitz_icon.rc)

set(BLITZ_VC_RUNTIME)
foreach(dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll)
    if (NOT EXISTS ${CMAKE_SOURCE_DIR}/build/xwin/redist/${dll})
        message(FATAL_ERROR "build/xwin/redist/${dll} is missing: run tools/windows/fetch_vc_runtime.sh")
    endif()
    list(APPEND BLITZ_VC_RUNTIME ${CMAKE_SOURCE_DIR}/build/xwin/redist/${dll})
endforeach()

add_custom_command(TARGET BlitzRecompiled POST_BUILD
    # The app's resources (BLITZ_STAGE_RESOURCES, the root CMakeLists.txt)
    ${BLITZ_STAGE_RESOURCES}

    # Runtime libraries: SDL2 (RT64's x64 build; FreeType is linked in), and DXC's, which RT64 loads before it picks a
    # graphics API (rt64_dynamic_libraries.cpp), even when that API is Vulkan. DXC needs the Visual C++ runtime, shipped
    # beside it (tools/windows/fetch_vc_runtime.sh) so a PC without the redistributable still starts.
    COMMAND ${CMAKE_COMMAND} -E copy ${BLITZ_WIN_DEPS}/SDL2-2.26.3/lib/x64/SDL2.dll ${BLITZ_RESOURCE_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll ${BLITZ_RESOURCE_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll ${BLITZ_RESOURCE_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy ${BLITZ_VC_RUNTIME} ${BLITZ_RESOURCE_DIR}/

    COMMENT "Staging the Windows app folder"
    VERBATIM
)

# A windowed app (no console window); SDL_MAIN_HANDLED keeps main() as the entry point.
set_target_properties(BlitzRecompiled PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(BlitzRecompiled PRIVATE /ENTRY:mainCRTStartup)
