# The Android app's contents (docs/ANDROID.md), staged in build/.../apk: lib/arm64-v8a holds the game (stripped; the
# unstripped .so stays beside it for crash symbols), SDL2 and the NDK's shared C++ runtime; assets/res the game's
# resources, which BlitzActivity unpacks to the app's files folder on first launch (and after an update); res/ the
# launcher icon. tools/android/build_apk.sh compiles the Java shell and packs, aligns and signs the APK.
if (BLITZ_MODERN_APP)
    # BRANDING: Android app id
    set(BLITZ_ANDROID_PACKAGE "com.blitzrecomp.newblitz")
else()
    # BRANDING: Android app id
    set(BLITZ_ANDROID_PACKAGE "com.blitzrecomp.newblitz.classic")
endif()
set(BLITZ_ANDROID_PACKAGE ${BLITZ_ANDROID_PACKAGE} CACHE INTERNAL "")
set(BLITZ_APK_DIR ${CMAKE_BINARY_DIR}/apk)
set(BLITZ_APK_LIBS ${BLITZ_APK_DIR}/lib/arm64-v8a)
configure_file(${CMAKE_SOURCE_DIR}/platform/android/AndroidManifest.xml.in ${BLITZ_APK_DIR}/AndroidManifest.xml @ONLY)
file(WRITE ${BLITZ_APK_DIR}/app.properties "package=${BLITZ_ANDROID_PACKAGE}\nlabel=${BLITZ_BUNDLE_NAME}\nversion=${BLITZ_VERSION}\nbuild=${BLITZ_BUILD}\ncommit=${BLITZ_COMMIT}\n")

target_link_libraries(BlitzRecompiled PRIVATE android log)
# The NDK's libc++_shared.so (ANDROID_STL=c++_shared: SDL and the game share one C++ runtime).
set(BLITZ_ANDROID_CXX_SHARED ${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so)

add_custom_command(TARGET BlitzRecompiled POST_BUILD
    ${BLITZ_STAGE_RESOURCES}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BLITZ_APK_LIBS}
    COMMAND ${CMAKE_STRIP} --strip-debug -o ${BLITZ_APK_LIBS}/libBlitzRecompiled.so $<TARGET_FILE:BlitzRecompiled>
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/build/android/prefix/lib/libSDL2.so ${BLITZ_APK_LIBS}/
    COMMAND ${CMAKE_COMMAND} -E copy ${BLITZ_ANDROID_CXX_SHARED} ${BLITZ_APK_LIBS}/
    # The launcher icon (the Mac's sips; the build runs on a Mac)
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BLITZ_APK_DIR}/res/mipmap-xxxhdpi
    COMMAND sips -z 192 192 ${BLITZ_APP_ICON} --out ${BLITZ_APK_DIR}/res/mipmap-xxxhdpi/ic_launcher.png >/dev/null
    COMMENT "Staging the Android app (${BLITZ_ANDROID_PACKAGE})"
    VERBATIM)
