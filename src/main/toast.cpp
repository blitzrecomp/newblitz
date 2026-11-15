// Toasts: a short status ("Slot 2: state saved") in a small Blitz '27 card at the bottom right, in a window
// or full screen, gone after a moment (docs/design/redesign/IMPLEMENTATION.md §4). A grid panel with chrome
// corners top-left and bottom-right and a flame rule along the bottom; "Eyebrow: Title" splits into a cyan
// eyebrow over a yellow title. Its own UI context that takes no input, so the game keeps every button
// press. On a phone it sits at the top centre instead (the bottom right is the A and B buttons), and a tap on it puts
// it away (toast_tap, from touch_controls.cpp). Main thread only (update_gfx in main.cpp).
#include <chrono>
#include <string>

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "ui_kit.h"
#include "blitz_platform.h"

namespace {
    using Clock = std::chrono::steady_clock;
    constexpr auto fade_in = std::chrono::milliseconds(150);
    constexpr auto show_for = std::chrono::milliseconds(2500);
    constexpr auto fade_out = std::chrono::milliseconds(300);

    recompui::ContextId context = recompui::ContextId::null();
    recompui::Element* card = nullptr;
    recompui::Element* content_box = nullptr;  // the panel itself, for taps
    recompui::Label* eyebrow = nullptr;
    recompui::Label* title = nullptr;
    bool shown = false;
    Clock::time_point started{};
    float opacity = -1.0f, shift = -1.0f;

    void build() {
        using namespace recompui;
        context = create_context();
        context.open();
        context.set_captures_input(false);
        context.set_captures_mouse(false);
        Element* root = context.create_element<Element>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(blitz::ui::px(0));
        root->set_left(blitz::ui::px(0));
        root->set_right(blitz::ui::px(0));
        root->set_bottom(blitz::ui::px(0));
        root->set_background_color(theme::color::Transparent);
        root->set_pointer_events(PointerEvents::None);
        if (blitz::platform::touch_controls) {
            // A phone: a row across the top, the card centred in it, under the SAVE and LOAD buttons.
            Element* row = context.create_element<Element>(root);
            row->set_position(Position::Absolute);
            row->set_top(blitz::ui::px(70));
            row->set_left(blitz::ui::px(0));
            row->set_right(blitz::ui::px(0));
            row->set_display(Display::Flex);
            row->set_justify_content(JustifyContent::Center);
            row->set_pointer_events(PointerEvents::None);
            card = context.create_element<Element>(row);
            card->set_position(Position::Relative);
        }
        else {
            card = context.create_element<Element>(root);
            card->set_position(Position::Absolute);
            card->set_right(blitz::ui::px(24));
            card->set_bottom(blitz::ui::px(24));
        }
        card->set_max_width(blitz::ui::px(420));
        card->set_pointer_events(PointerEvents::None);
        blitz::ui::PanelOptions o;
        o.absolute = true;
        o.corners_tl_br = true;
        o.corner_size = 14;
        Element* content = blitz::ui::panel(context, card, o);
        content_box = content;
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_gap(blitz::ui::px(4));
        content->set_padding_top(blitz::ui::px(14));
        content->set_padding_left(blitz::ui::px(20));
        content->set_padding_right(blitz::ui::px(20));
        content->set_padding_bottom(blitz::ui::px(12));
        eyebrow = context.create_element<Label>(content, "", theme::Typography::LabelXS);
        eyebrow->set_color(Color{143, 239, 245, 255});
        title = context.create_element<Label>(content, "", theme::Typography::Header3);
        title->set_color(theme::color::SecondaryL);
        Element* rule = context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_margin_top(blitz::ui::px(4));
        rule->set_position(Position::Relative);
        blitz::ui::stretch(context, rule, "rule_flame");
        context.close();
    }

    void set_look(float o, float dx) {
        if (o == opacity && dx == shift) return;
        opacity = o;
        shift = dx;
        context.open();
        card->set_opacity(o);
        card->set_translate_2D(dx, 0.0f);
        context.close();
    }

    std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
}

namespace blitz {
    void toast_show(const std::string& message) {
        if (context == recompui::ContextId::null()) build();
        size_t colon = message.find(": ");
        std::string top = colon == std::string::npos ? "" : message.substr(0, colon);
        std::string main = colon == std::string::npos ? message : message.substr(colon + 2);
        context.open();
        eyebrow->set_text(upper(top));
        eyebrow->set_display(top.empty() ? recompui::Display::None : recompui::Display::Block);
        title->set_text(upper(main));
        context.close();
        if (!shown) {
            set_look(0.0f, 12.0f);
            recompui::show_context(context, "");
            shown = true;
            started = Clock::now();
        }
        else {
            started = Clock::now() - fade_in;  // already up: just restart its time on screen
        }
    }

    // Held (a load, notices.cpp post_status_held): it stays up until released, a tap, or the next message.
    bool held = false;
    void toast_hold(bool on) { held = on; }

    // A tap at (x, y), in the UI's pixels (touch_controls.cpp): on the card, it fades out now. True if it was on it.
    bool toast_tap(float x, float y) {
        if (!shown || content_box == nullptr) return false;
        auto t = Clock::now() - started;
        if (t >= fade_in + show_for) return false;  // already going
        float margin = 16.0f * content_box->get_dp_to_pixel_ratio();
        float l = content_box->get_absolute_left() - margin, top = content_box->get_absolute_top() - margin;
        float r = content_box->get_absolute_left() + content_box->get_client_width() + margin;
        float b = content_box->get_absolute_top() + content_box->get_client_height() + margin;
        if (x < l || x > r || y < top || y > b) return false;
        started = Clock::now() - fade_in - show_for;  // straight to the fade
        held = false;
        return true;
    }

    // Every UI tick: slide in, hold, fade out, then take the context off screen.
    void toast_update() {
        if (!shown) return;
        if (held && Clock::now() - started > fade_in) started = Clock::now() - fade_in;
        auto t = Clock::now() - started;
        auto ms = [](auto d) { return std::chrono::duration<float, std::milli>(d).count(); };
        if (t < fade_in) {
            float k = ms(t) / ms(fade_in);
            set_look(k, 12.0f * (1.0f - k));
        }
        else if (t < fade_in + show_for) {
            set_look(1.0f, 0.0f);
        }
        else if (t < fade_in + show_for + fade_out) {
            set_look(1.0f - ms(t - fade_in - show_for) / ms(fade_out), 0.0f);
        }
        else {
            recompui::hide_context(context);
            shown = false;
        }
    }
}
