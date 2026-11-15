# The Linux app folder (docs/PLATFORMS.md): the executable ("Classic Blitz" / "Modern Blitz") with its resources
# beside it, as on Windows. It uses the system's SDL2, FreeType and Vulkan loader (every desktop and the Steam Deck have
# them). platform_linux.cpp's early_init points the frontend at this folder.
set_target_properties(BlitzRecompiled PROPERTIES OUTPUT_NAME "${BLITZ_BUNDLE_NAME}")
add_custom_command(TARGET BlitzRecompiled POST_BUILD
    ${BLITZ_STAGE_RESOURCES}
    COMMAND ${CMAKE_COMMAND} -E copy ${BLITZ_APP_ICON} ${BLITZ_RESOURCE_DIR}/icon.png
    COMMENT "Staging the Linux app folder"
    VERBATIM)
