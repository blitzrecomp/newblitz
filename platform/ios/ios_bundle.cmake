# The iPhone app bundle (docs/IOS_FEASIBILITY.md): a flat .app with the executable, Info.plist (the scene manifest
# iOS 27 requires), icons and the game's resources. Signing and installing are tools/ios/install.sh's (your team's
# provisioning profile and entitlements), so this build stays a plain Ninja build like the other platforms.
if (BLITZ_MODERN_APP)
    # BRANDING: iOS bundle id
    set(BLITZ_IOS_BUNDLE_ID "com.blitzrecomp.newblitz")
else()
    # BRANDING: iOS bundle id
    set(BLITZ_IOS_BUNDLE_ID "com.blitzrecomp.newblitz.classic")
endif()
set(BLITZ_IOS_BUNDLE_ID ${BLITZ_IOS_BUNDLE_ID} CACHE INTERNAL "")
configure_file(${CMAKE_SOURCE_DIR}/platform/ios/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
# SDL starts UIKit (UIApplicationMain) in its own main() and runs the app's main() as SDL_main from there.
target_link_libraries(BlitzRecompiled PRIVATE SDL2::SDL2main "-framework UniformTypeIdentifiers")  # + the document picker
set_target_properties(BlitzRecompiled PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
    OUTPUT_NAME BlitzRecompiled)

add_custom_command(TARGET BlitzRecompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/Info.plist $<TARGET_BUNDLE_DIR:BlitzRecompiled>/Info.plist
    # A new build is unsigned: drop what an earlier tools/ios/install.sh left in the bundle (its provisioning profile,
    # which names the developer, and a signature that no longer matches), so an unsigned bundle carries neither.
    COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_BUNDLE_DIR:BlitzRecompiled>/embedded.mobileprovision
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_BUNDLE_DIR:BlitzRecompiled>/_CodeSignature
    # Home Screen icons (iPhone 60pt @2x/@3x, iPad 76pt @2x)
    COMMAND sips -z 120 120 ${BLITZ_APP_ICON} --out $<TARGET_BUNDLE_DIR:BlitzRecompiled>/AppIcon60x60@2x.png >/dev/null
    COMMAND sips -z 180 180 ${BLITZ_APP_ICON} --out $<TARGET_BUNDLE_DIR:BlitzRecompiled>/AppIcon60x60@3x.png >/dev/null
    COMMAND sips -z 152 152 ${BLITZ_APP_ICON} --out $<TARGET_BUNDLE_DIR:BlitzRecompiled>/AppIcon76x76@2x~ipad.png >/dev/null
    ${BLITZ_STAGE_RESOURCES}
    COMMENT "Staging the iOS app bundle"
    VERBATIM)
