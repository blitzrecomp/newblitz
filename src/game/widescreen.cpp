// Widescreen support for the game side. RT64 widens Blitz's 3D by itself in Aspect Ratio "Expand"
// (platform/rt64-patches/0001). The game never clears its colour buffer in most modes, because its
// own backdrop or 3D scene covers the 4:3 screen every frame; the widened sides of 2D-only screens
// would keep whatever an earlier frame left there. While Expand is on, gfx_build_frame uses the
// game's own clear mode (a full-screen FILLRECT, which RT64 stretches over the widened buffer).
#include "ultramodern/config.hpp"

extern "C" int blitz_force_color_clear(void) {
    return ultramodern::renderer::get_graphics_config().ar_option == ultramodern::renderer::AspectRatio::Expand;
}
