# Define the path to the entitlements file
set(ENTITLEMENTS_FILE ${CMAKE_SOURCE_DIR}/platform/macos/entitlements.plist)

# Set bundle properties
set_target_properties(BlitzRecompiled PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "${BLITZ_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${BLITZ_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${BLITZ_BUILD}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${BLITZ_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${ENTITLEMENTS_FILE}
)

# Create icon files for macOS bundle
set(ICON_SOURCE ${BLITZ_APP_ICON})  # per flavour (CMakeLists.txt)
set(ICONSET_DIR ${CMAKE_BINARY_DIR}/AppIcon.iconset)
set(ICNS_FILE ${CMAKE_BINARY_DIR}/resources/AppIcon.icns)

# Create iconset directory and add PNG file
add_custom_command(
        OUTPUT ${ICONSET_DIR}
        DEPENDS ${ICON_SOURCE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ICONSET_DIR}
        COMMAND sips -z 512 512 ${ICON_SOURCE} --out ${ICONSET_DIR}/icon_512x512.png >/dev/null
        COMMAND ${CMAKE_COMMAND} -E copy ${ICON_SOURCE} ${ICONSET_DIR}/icon_512x512@2x.png
        COMMAND touch ${ICONSET_DIR}
        COMMENT "Creating iconset directory and copying PNG file"
)

# Convert iconset to icns
add_custom_command(
        OUTPUT ${ICNS_FILE}
        DEPENDS ${ICONSET_DIR}
        COMMAND iconutil -c icns ${ICONSET_DIR} -o ${ICNS_FILE}
        COMMENT "Converting iconset to icns"
)

# Custom target to ensure icns creation
add_custom_target(create_icns ALL DEPENDS ${ICNS_FILE})

# Set source file properties for the resulting icns file
set_source_files_properties(${ICNS_FILE} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
)

# Add the icns file to the executable target
target_sources(BlitzRecompiled PRIVATE ${ICNS_FILE})

# Ensure BlitzRecompiled depends on create_icns
add_dependencies(BlitzRecompiled create_icns)

# Configure Info.plist
configure_file(${CMAKE_SOURCE_DIR}/platform/macos/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)

# Install the app bundle
install(TARGETS BlitzRecompiled BUNDLE DESTINATION .)

# Ensure the entitlements file exists
if(NOT EXISTS ${ENTITLEMENTS_FILE})
    message(FATAL_ERROR "Entitlements file not found at ${ENTITLEMENTS_FILE}")
endif()

# Post-build steps for macOS bundle
add_custom_command(TARGET BlitzRecompiled POST_BUILD
    # Copy and fix frameworks first, into an empty Frameworks folder (a library the app stopped using, e.g.
    # Homebrew's libpng before tools/build_deps.sh, mustn't linger in the bundle)
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_BUNDLE_CONTENT_DIR:BlitzRecompiled>/Frameworks
    COMMAND ${CMAKE_COMMAND} -D CMAKE_BUILD_TYPE=$<CONFIG> -D CMAKE_GENERATOR=${CMAKE_GENERATOR} -D REAL_SDL2_LIBDIR=${REAL_SDL2_LIBDIR} -P ${CMAKE_SOURCE_DIR}/platform/macos/fixup_bundle.cmake

    # The app's resources (BLITZ_STAGE_RESOURCES, the root CMakeLists.txt): assets, controller database, packs
    ${BLITZ_STAGE_RESOURCES}

    # Set RPATH
    COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks/" $<TARGET_BUNDLE_DIR:BlitzRecompiled>/Contents/MacOS/BlitzRecompiled

    # Sign the bundle
    COMMAND codesign --verbose=4 --options=runtime --no-strict --sign - --entitlements ${ENTITLEMENTS_FILE} --deep --force $<TARGET_BUNDLE_DIR:BlitzRecompiled>

    COMMENT "Performing post-build steps for macOS bundle"
    VERBATIM
)
