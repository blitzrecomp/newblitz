# iOS feasibility probe (docs/IOS_FEASIBILITY.md, section 9): a tiny signed app that checks, on a real iPhone, what
# Blitz's runtime needs: the 4 GiB guest-address reservation with 512 MiB writable, the memory iOS grants, the Metal
# GPU's features, and that writable+executable memory is refused (the game must not need it). Results on screen and
# in Documents/probe.txt.
#   cmake -S tools/ios/probe -B build/ios/probe -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DBLITZ_IOS_TEAM=<your team ID>
#   xcodebuild -project build/ios/probe/BlitzProbe.xcodeproj -scheme BlitzProbe -sdk iphoneos -allowProvisioningUpdates
cmake_minimum_required(VERSION 3.24)
project(BlitzProbe LANGUAGES C CXX OBJCXX)
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
set(BLITZ_IOS_TEAM "" CACHE STRING "Apple developer team ID")

add_executable(BlitzProbe MACOSX_BUNDLE probe.mm)
target_compile_options(BlitzProbe PRIVATE -fobjc-arc)
target_link_libraries(BlitzProbe PRIVATE "-framework UIKit" "-framework Metal" "-framework Foundation")
set_target_properties(BlitzProbe PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.blitzrecomp.probe"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${BLITZ_IOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/BlitzProbe.entitlements"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0")
