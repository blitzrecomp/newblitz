# The Android toolchain on this Mac (sourced by tools/android/*.sh and the Makefile's android targets).
# Override any of these in the environment. NDK r28+ aligns native libraries for 16 KB pages by default.
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ANDROID_NDK_VERSION="${ANDROID_NDK_VERSION:-29.0.14206865}"
ANDROID_NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/$ANDROID_NDK_VERSION}"
ANDROID_API="${ANDROID_API:-29}"              # minSdk: Android 10 (AAudio, Vulkan 1.1 on the devices that matter)
ANDROID_TARGET_API="${ANDROID_TARGET_API:-36}"
ANDROID_BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$(ls -d "$ANDROID_HOME"/build-tools/* 2>/dev/null | sort -V | tail -1)}"
JAVA_HOME="${JAVA_HOME:-/Applications/Android Studio.app/Contents/jbr/Contents/Home}"
export ANDROID_HOME ANDROID_NDK JAVA_HOME
ANDROID_CMAKE_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
                    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-$ANDROID_API" -DANDROID_STL=c++_shared)
[ -d "$ANDROID_NDK" ] || { echo "No NDK at $ANDROID_NDK (sdkmanager --install \"ndk;$ANDROID_NDK_VERSION\")" >&2; return 2 2>/dev/null || exit 2; }
