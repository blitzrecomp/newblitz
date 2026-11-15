include(BundleUtilities)

# Check for pkgx installation
find_program(PKGX_EXECUTABLE pkgx)

# Xcode generator puts the build type in the build directory
set(BUILD_PREFIX "")
if (CMAKE_GENERATOR STREQUAL "Xcode")
    set(BUILD_PREFIX "${CMAKE_BUILD_TYPE}/")
endif()

# Use generator expressions to get the absolute path to the bundle
set(APPS "${BUILD_PREFIX}BlitzRecompiled.app/Contents/MacOS/BlitzRecompiled")

# Set up framework search paths
set(DIRS "${BUILD_PREFIX}BlitzRecompiled.app/Contents/Frameworks")

# Detect if we're using pkgx
if(PKGX_EXECUTABLE)
    message(STATUS "pkgx detected, adding pkgx directories to framework search path")
    list(APPEND DIRS "$ENV{HOME}/.pkgx/")
endif()

# Convert all paths to absolute paths
file(REAL_PATH ${APPS} APPS)

set(RESOLVED_DIRS "")
foreach(DIR IN LISTS DIRS)
    # Handle home directory expansion
    string(REPLACE "~" "$ENV{HOME}" DIR "${DIR}")
    # Convert to absolute path, but don't fail if directory doesn't exist
    if(EXISTS "${DIR}")
        file(REAL_PATH "${DIR}" RESOLVED_DIR)
        list(APPEND RESOLVED_DIRS "${RESOLVED_DIR}")
    endif()
endforeach()

# Debug output
message(STATUS "Bundle fixup paths:")
message(STATUS "  App: ${APPS}")
message(STATUS "  Search dirs: ${RESOLVED_DIRS}")

# Homebrew's sdl2-compat can take over /opt/homebrew/opt/sdl2 (it provides "sdl2"), and the real SDL2
# keg's dylib still records that path as its install name. Resolve it to the real keg instead, or
# the bundle ships the compat shim, which needs SDL3 at runtime ("Failed loading SDL3 library").
if(REAL_SDL2_LIBDIR)
    function(gp_resolve_item_override context item exepath dirs resolved_item_var resolved_var)
        if(item MATCHES "^/opt/homebrew/opt/sdl2/lib/(libSDL2.*\\.dylib)$")
            set(${resolved_item_var} "${REAL_SDL2_LIBDIR}/${CMAKE_MATCH_1}" PARENT_SCOPE)
            set(${resolved_var} 1 PARENT_SCOPE)
        endif()
    endfunction()
    message(STATUS "  SDL2 from: ${REAL_SDL2_LIBDIR}")
endif()

# Fix up the bundle
fixup_bundle("${APPS}" "" "${RESOLVED_DIRS}")