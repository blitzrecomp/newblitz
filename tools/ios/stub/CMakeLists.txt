# A stub app with the real app's bundle ID and entitlements, built by Xcode only so automatic signing creates (or
# refreshes) its development provisioning profile; tools/ios/install.sh takes that profile for the real app.
cmake_minimum_required(VERSION 3.24)
project(BlitzStub LANGUAGES C OBJC)
set(BLITZ_IOS_TEAM "" CACHE STRING "Apple developer team ID")
set(BLITZ_IOS_BUNDLE_ID "" CACHE STRING "Bundle identifier")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
add_executable(BlitzStub MACOSX_BUNDLE stub.m)
target_link_libraries(BlitzStub PRIVATE "-framework UIKit")
# Another entitlements file (BLITZ_IOS_ENTITLEMENTS): e.g. without extended-virtual-addressing, to try a free account's limits.
if(NOT BLITZ_IOS_ENTITLEMENTS)
    set(BLITZ_IOS_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../platform/ios/BlitzRecomp.entitlements")
endif()
set_target_properties(BlitzStub PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${BLITZ_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${BLITZ_IOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${BLITZ_IOS_ENTITLEMENTS}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "17.0")
