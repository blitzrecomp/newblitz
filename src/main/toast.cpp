// Toasts: a short status ("Slot 2: state saved") in a small Blitz '27 card at the bottom right, in a window
// or full screen, gone after a moment (docs/design/redesign/IMPLEMENTATION.md §4). A grid panel with chrome
// corners top-left and bottom-right and a flame rule along the bottom; "Eyebrow: Title" splits into a cyan
// eyebrow over a yellow title. Its own UI context that takes no input, so the game keeps every button
// press. On a phone it sits at the top centre instead (the bottom right is the A and B buttons), and a tap on it puts
// it away (toast_tap, from touch_controls.cpp). Main thread only (update_gfx in main.cpp).
//
// An offer (toast_offer) is a second card of the same look, stacked above the status card: a question with one action
// ("Pick up your last game?" with [△] RESUME, savestate.cpp). It doesn't block anything: it goes after its time (the
// flame rule runs down meanwhile), or when its owner closes it. A click or tap on it takes the action; the button is the
// owner's to watch (savestate.cpp reads it from the game's input), which calls toast_offer_accept.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

#include "SDL2/SDL.h"
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
    bool context_up = false;
    recompui::Element* card = nullptr;
    recompui::Element* content_box = nullptr;  // the panel itself, for taps
    recompui::Label* eyebrow = nullptr;
    recompui::Label* title = nullptr;
    bool shown = false;
    Clock::time_point started{};
    float opacity = -1.0f, shift = -1.0f;

    // The offer's card.
    recompui::Element* offer_card = nullptr;
    recompui::Element* offer_box = nullptr;
    recompui::Label* offer_eyebrow = nullptr;
    recompui::Label* offer_title = nullptr;
    recompui::Label* offer_detail = nullptr;
    recompui::Label* offer_note = nullptr;
    recompui::Label* offer_glyph = nullptr;
    recompui::Label* offer_action = nullptr;
    recompui::Element* offer_rule = nullptr;  // runs down with the offer's time
    bool offer_shown = false, offer_leaving = false;
    Clock::time_point offer_started{}, offer_leave_at{}, offer_paused_at{};
    Clock::duration offer_for{};
    std::function<void()> offer_accept;
    float offer_opacity = -1.0f, offer_shift = -1.0f;
    int offer_rule_pct = -1;

    // A left click, in window points (SDL's event watch), for toast_update to hit-test on the main thread's tick.
    std::mutex click_mutex;
    bool click_pending = false;
    SDL_Window* click_window = nullptr;
    float click_x = 0.0f, click_y = 0.0f;
    std::atomic<bool> offer_tapped{false};

    int on_mouse(void*, SDL_Event* e) {
        if (e->type != SDL_MOUSEBUTTONDOWN || e->button.button != SDL_BUTTON_LEFT) return 1;
        // A phone's taps come as touches too (toast_tap, from touch_controls.cpp): not twice.
        if (blitz::platform::touch_controls && e->button.which == SDL_TOUCH_MOUSEID) return 1;
        std::lock_guard lock{click_mutex};
        click_pending = true;
        click_window = SDL_GetWindowFromID(e->button.windowID);
        click_x = (float)e->button.x;
        click_y = (float)e->button.y;
        return 1;
    }

    // (x, y) in the UI's pixels on the panel (with a margin, for fingers).
    bool on_box(recompui::Element* box, float x, float y) {
        if (box == nullptr) return false;
        float margin = 16.0f * box->get_dp_to_pixel_ratio();
        float l = box->get_absolute_left() - margin, top = box->get_absolute_top() - margin;
        float r = box->get_absolute_left() + box->get_client_width() + margin;
        float b = box->get_absolute_top() + box->get_client_height() + margin;
        return x >= l && x <= r && y >= top && y <= b;
    }

    // A card: the toast's grid panel with its corners; returns the content column.
    recompui::Element* make_card(recompui::Element* parent, recompui::Element*& out_card) {
        using namespace recompui;
        out_card = context.create_element<Element>(parent);
        out_card->set_position(Position::Relative);
        out_card->set_max_width(blitz::ui::px(420));
        out_card->set_pointer_events(PointerEvents::None);
        out_card->set_display(Display::None);
        blitz::ui::PanelOptions o;
        o.absolute = true;
        o.corners_tl_br = true;
        o.corner_size = 14;
        Element* content = blitz::ui::panel(context, out_card, o);
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_gap(blitz::ui::px(4));
        content->set_padding_top(blitz::ui::px(14));
        content->set_padding_left(blitz::ui::px(20));
        content->set_padding_right(blitz::ui::px(20));
        content->set_padding_bottom(blitz::ui::px(12));
        // The column's own cap (the card's 420 less the 2 x 20 padding): the stack is a flex column across the screen, so
        // RmlUi measured a card's height at the full width and only then narrowed it to 420. A long status (the freeze
        // rescue's, four lines on the team's iPhone 2026-11-26) wrapped past the bottom of a two-line card. Capped here, the
        // text wraps at the width it's measured at.
        content->set_max_width(blitz::ui::px(380));
        return content;
    }

    recompui::Element* flame_rule(recompui::Element* content) {
        using namespace recompui;
        Element* rule = context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_margin_top(blitz::ui::px(4));
        rule->set_position(Position::Relative);
        blitz::ui::stretch(context, rule, "rule_flame");
        return rule;
    }

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
        // The cards in a column: the offer above the status.
        Element* stack = context.create_element<Element>(root);
        stack->set_position(Position::Absolute);
        stack->set_display(Display::Flex);
        stack->set_flex_direction(FlexDirection::Column);
        stack->set_gap(blitz::ui::px(12));
        stack->set_pointer_events(PointerEvents::None);
        if (blitz::platform::touch_controls) {
            // A phone: across the top, the cards centred, under the SAVE and LOAD buttons.
            stack->set_top(blitz::ui::px(70));
            stack->set_left(blitz::ui::px(0));
            stack->set_right(blitz::ui::px(0));
            stack->set_align_items(AlignItems::Center);
        }
        else {
            stack->set_right(blitz::ui::px(24));
            stack->set_bottom(blitz::ui::px(24));
            stack->set_align_items(AlignItems::FlexEnd);
        }

        Element* oc = make_card(stack, offer_card);
        offer_box = oc;
        offer_eyebrow = context.create_element<Label>(oc, "", theme::Typography::LabelXS);
        offer_eyebrow->set_color(Color{143, 239, 245, 255});
        offer_title = context.create_element<Label>(oc, "", theme::Typography::Header3);
        offer_title->set_color(theme::color::SecondaryL);
        offer_detail = context.create_element<Label>(oc, "", theme::Typography::LabelSM);
        offer_detail->set_color(Color{242, 242, 242, 255});
        offer_note = context.create_element<Label>(oc, "", theme::Typography::LabelXS);
        offer_note->set_color(Color{242, 242, 242, 200});
        Element* row = context.create_element<Element>(oc);
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_gap(blitz::ui::px(8));
        row->set_margin_top(blitz::ui::px(6));
        offer_glyph = context.create_element<Label>(row, "", theme::Typography::Body);
        offer_glyph->set_font_family("promptfont");  // as the hints draw buttons (hints.cpp)
        offer_glyph->set_font_style(FontStyle::Normal);
        offer_glyph->set_font_weight(400);
        offer_glyph->set_font_size(blitz::ui::px(30));
        offer_glyph->set_color(Color{255, 255, 255, 255});
        offer_action = context.create_element<Label>(row, "", theme::Typography::LabelMD);
        offer_action->set_color(theme::color::SecondaryL);
        offer_rule = flame_rule(oc);
        offer_rule->set_width(100.0f, Unit::Percent);

        Element* content = make_card(stack, card);
        content_box = content;
        eyebrow = context.create_element<Label>(content, "", theme::Typography::LabelXS);
        eyebrow->set_color(Color{143, 239, 245, 255});
        title = context.create_element<Label>(content, "", theme::Typography::Header3);
        title->set_color(theme::color::SecondaryL);
        flame_rule(content);
        context.close();
        SDL_AddEventWatch(on_mouse, nullptr);
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

    void set_offer_look(float o, float dx) {
        if (o == offer_opacity && dx == offer_shift) return;
        offer_opacity = o;
        offer_shift = dx;
        context.open();
        offer_card->set_opacity(o);
        offer_card->set_translate_2D(dx, 0.0f);
        context.close();
    }

    void set_shown(recompui::Element* c, bool on) {
        context.open();
        c->set_display(on ? recompui::Display::Block : recompui::Display::None);
        context.close();
    }

    // The context is up while either card is.
    void sync_context() {
        bool want = shown || offer_shown;
        if (want == context_up) return;
        context_up = want;
        if (want) recompui::show_context(context, "");
        else recompui::hide_context(context);
    }

    std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    void offer_leave() {
        if (!offer_shown || offer_leaving) return;
        offer_leaving = true;
        offer_leave_at = Clock::now();
        offer_accept = nullptr;
    }

    void offer_update() {
        if (!offer_shown) return;
        auto ms = [](auto d) { return std::chrono::duration<float, std::milli>(d).count(); };
        const Clock::time_point now = Clock::now();
        if (offer_leaving) {
            auto t = now - offer_leave_at;
            if (t < fade_out) {
                set_offer_look(std::min(offer_opacity, 1.0f - ms(t) / ms(fade_out)), 0.0f);
                return;
            }
            offer_shown = false;
            offer_leaving = false;
            set_shown(offer_card, false);
            fprintf(stderr, "[toast] offer gone\n");
            return;
        }
        // A menu over the game (Settings, a prompt): the offer's time waits for it.
        if (recompui::is_context_capturing_input()) {
            if (offer_paused_at == Clock::time_point{}) offer_paused_at = now;
            return;
        }
        if (offer_paused_at != Clock::time_point{}) {
            offer_started += now - offer_paused_at;
            offer_paused_at = {};
        }
        auto t = now - offer_started;
        if (t < fade_in) {
            float k = ms(t) / ms(fade_in);
            set_offer_look(k, 12.0f * (1.0f - k));
        }
        else set_offer_look(1.0f, 0.0f);
        float left = 1.0f - std::clamp(ms(t) / ms(fade_in + offer_for), 0.0f, 1.0f);
        int pct = (int)(left * 100.0f + 0.5f);
        if (pct != offer_rule_pct) {
            offer_rule_pct = pct;
            context.open();
            offer_rule->set_width((float)pct, recompui::Unit::Percent);
            context.close();
        }
        if (t >= fade_in + offer_for) {
            fprintf(stderr, "[toast] offer timed out\n");
            offer_leave();
        }
    }
}

namespace blitz {
    std::string hint_token_glyph(const std::string& token, bool& glyph);  // hints.cpp

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
            set_shown(card, true);
            shown = true;
            sync_context();
            started = Clock::now();
        }
        else {
            started = Clock::now() - fade_in;  // already up: just restart its time on screen
        }
    }

    // Held (a load, notices.cpp post_status_held): it stays up until released, a tap, or the next message.
    bool held = false;
    void toast_hold(bool on) { held = on; }

    // The offer (see the top): eyebrow and title as a status's, then a detail line and a note (either may be ""), and the
    // action under them with the button that takes it: `token` is a hint token (hints.cpp: "CLEFT" is △ by default) drawn
    // as player 1's button for it; on a phone played by touch alone, "Tap to <action>" instead. Up for `seconds` unless
    // taken or closed. A new offer replaces one that's up.
    void toast_offer(const std::string& top, const std::string& main, const std::string& detail, const std::string& note,
                     const std::string& action, const std::string& token, int seconds, std::function<void()> on_accept) {
        if (context == recompui::ContextId::null()) build();
        bool glyph = false;
        std::string button = blitz::ui::touch_only() ? std::string() : hint_token_glyph(token, glyph);
        context.open();
        offer_eyebrow->set_text(upper(top));
        offer_eyebrow->set_display(top.empty() ? recompui::Display::None : recompui::Display::Block);
        offer_title->set_text(upper(main));
        offer_detail->set_text(detail);
        offer_detail->set_display(detail.empty() ? recompui::Display::None : recompui::Display::Block);
        offer_note->set_text(note);
        offer_note->set_display(note.empty() ? recompui::Display::None : recompui::Display::Block);
        offer_glyph->set_text(glyph ? button : std::string());
        offer_glyph->set_display(glyph ? recompui::Display::Block : recompui::Display::None);
        // No glyph for the button (unbound): its name in words; touch alone: tap.
        offer_action->set_text(upper(button.empty() ? "Tap to " + action : glyph ? action : button + ": " + action));
        offer_rule->set_width(100.0f, recompui::Unit::Percent);
        context.close();
        offer_rule_pct = 100;
        offer_accept = std::move(on_accept);
        offer_for = std::chrono::seconds(std::max(1, seconds));
        offer_started = Clock::now();
        offer_paused_at = {};
        offer_leaving = false;
        offer_tapped = false;
        {
            std::lock_guard lock{click_mutex};
            click_pending = false;
        }
        if (!offer_shown) {
            offer_opacity = offer_shift = -1.0f;
            set_offer_look(0.0f, 12.0f);
            set_shown(offer_card, true);
            offer_shown = true;
            sync_context();
        }
        fprintf(stderr, "[toast] offer: %s %s (%s, %d s)\n", top.c_str(), main.c_str(), action.c_str(), seconds);
    }

    bool toast_offer_up() { return offer_shown && !offer_leaving; }

    // The offer's action, now (its button, a click or a tap): it goes, and its callback runs. False if none was up.
    bool toast_offer_accept() {
        if (!toast_offer_up()) return false;
        std::function<void()> f = std::move(offer_accept);
        offer_accept = nullptr;
        offer_leave();
        fprintf(stderr, "[toast] offer taken\n");
        if (f) f();
        return true;
    }

    // Put away without its action (its owner: the player started something else).
    void toast_offer_close() {
        if (toast_offer_up()) fprintf(stderr, "[toast] offer closed\n");
        offer_leave();
    }

    // A tap at (x, y), in the UI's pixels (touch_controls.cpp): on the offer, its action (on the next tick); on the status
    // card, it fades out now. True if it was on either.
    bool toast_tap(float x, float y) {
        if (toast_offer_up() && on_box(offer_box, x, y)) {
            offer_tapped = true;
            return true;
        }
        if (!shown || content_box == nullptr) return false;
        auto t = Clock::now() - started;
        if (t >= fade_in + show_for) return false;  // already going
        if (!on_box(content_box, x, y)) return false;
        started = Clock::now() - fade_in - show_for;  // straight to the fade
        held = false;
        return true;
    }

    // Every UI tick: slide in, hold, fade out, then take the context off screen.
    void toast_update() {
        if (context == recompui::ContextId::null()) return;
        // A click (desktop) or tap on the offer: its action. Not under a menu: that click was the menu's.
        bool clicked = false;
        {
            std::lock_guard lock{click_mutex};
            if (click_pending && toast_offer_up() && click_window != nullptr) {
                int ww = 0, wh = 0, pw = 0, ph = 0;
                SDL_GetWindowSize(click_window, &ww, &wh);
                SDL_GetWindowSizeInPixels(click_window, &pw, &ph);
                float sx = ww > 0 ? (float)pw / (float)ww : 1.0f, sy = wh > 0 ? (float)ph / (float)wh : 1.0f;
                clicked = on_box(offer_box, click_x * sx, click_y * sy);
            }
            click_pending = false;
        }
        if (offer_tapped.exchange(false)) clicked = true;
        if (clicked && !recompui::is_context_capturing_input()) toast_offer_accept();
        offer_update();
        if (shown) {
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
                set_shown(card, false);
                shown = false;
            }
        }
        sync_context();
    }
}
