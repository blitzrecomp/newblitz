// The Blitz '27 UI kit (docs/design/redesign): framed grid panels, chrome corners, selector caps and the
// art from assets/ui/ (tools/ui_kit_assets.py), built from plain recompui elements and images, since the
// UI library has no 9-slice or tiling. Call from UI code with a context open.
#pragma once
#include <string>

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"

namespace blitz::ui {
    // The design's sizes are px at 960x720; recompui lays out in a 1080-tall space (1 dp = height / 1080),
    // so every design size is multiplied by 1.5.
    constexpr float k = 1.5f;
    constexpr float px(float design) { return design * k; }

    // Queues every kit image as "?/blitz/ui/<name>" (once).
    void load();
    // "?/blitz/ui/<name>"
    std::string src(const std::string& name);

    // A kit image absolutely placed in its parent (left/top in dp; width/height in dp).
    recompui::Image* image(recompui::ContextId ctx, recompui::Element* parent, const std::string& name, float left, float top, float w, float h);

    // A kit image filling its (positioned) parent: rules, backgrounds.
    recompui::Image* stretch(recompui::ContextId ctx, recompui::Element* parent, const std::string& name);

    struct PanelOptions {
        const char* frame = "panel_frame";  // panel_frame, panel_frame_hot, panel_frame_p1..4, header_bevel
        float inset = 16.0f;                // the frame's corner height (and width, unless inset_x is set)
        float inset_x = -1.0f;              // the corner width when it differs (header_bevel: 24 x 20)
        bool grid = true;                   // the teal grid inside
        bool corners = false;               // chrome corner brackets outside the corners
        bool corners_tl_br = false;         // only the top-left and bottom-right ones (toasts)
        float corner_size = 20.0f;
        bool absolute = false;              // the box is absolutely positioned (else it's made relative)
    };
    // Turns `box` (sized by the caller) into a framed panel: decorations behind, and returns the element
    // its content goes in (full width, position relative so it paints on top; its height is its content's,
    // so a box can size to its content, or a fixed-size box can set the content to 100%).
    recompui::Element* panel(recompui::ContextId ctx, recompui::Element* box, const PanelOptions& o = {});
    // Swaps a panel's frame (e.g. a player's colour when a controller connects).
    void set_panel_frame(recompui::ContextId ctx, recompui::Element* box, const char* frame);

    // Registers the kit as recompui's panel decorator (prompts and their title strips), and touch_only() as recompui's
    // touch-only check (hint rows). Before the UI exists.
    void register_decorators();

    // A phone (or tablet) played by touch alone: no controller connected, and the last key wasn't from a keyboard.
    // Button hints then name what's on screen to tap, not keys. BLITZ_TOUCH_ONLY=1 forces it on a computer (dev).
    bool touch_only();
    // Main thread (controllers_update), every ~100 ms: whether a controller's connected, for touch_only. touch_only runs in
    // the draw under the UI lock and must not ask SDL there.
    void touch_only_refresh();

    // Chrome "[ ]" caps outside a row's ends (the row must be positioned: relative or absolute) (the focused-item look); returns the two cap holders so
    // callers can show or hide them.
    std::pair<recompui::Element*, recompui::Element*> selector_caps(recompui::ContextId ctx, recompui::Element* row);
}
