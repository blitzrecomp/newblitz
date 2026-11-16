// Setup's screens (firstrun_screen.h): one context, rebuilt for each screen, in the Blitz '27 look of the Export
// screen (export_screen.cpp): a framed panel over a dark veil, the eyebrow, the title on a flame rule, the body, and
// the buttons with the default one big and focused. Back and Exit sit at the bottom; the hints under the panel show
// their buttons (Back: □ or ○, Backspace, Esc; Exit: Select, or the on-screen button).
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "SDL2/SDL.h"
#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "qrcodegen.hpp"
#include "ui_kit.h"
#include "firstrun_screen.h"

namespace {
    using namespace recompui;
    using blitz::firstrun_ui::Screen;

    ContextId context = ContextId::null();
    Element* root = nullptr;
    Screen current;  // what's up (its callbacks are called from here)
    bool up = false;

    const Color white{242, 242, 242, 255}, dim{184, 196, 218, 255}, teal{143, 239, 245, 255}, gold{246, 212, 0, 255};

    // Back and Exit from the controller and keyboard (the on-screen buttons call the same). The cancel key on the
    // first screen, which has no Back, leaves setup (the design's Skip).
    void go_back() {
        if (current.back) current.back();
        else if (current.exit) current.exit();
    }

    // When Esc was last pressed: Esc and a controller's Select both arrive as the Toggle action.
    std::atomic<Uint32> escape_at{0};
    int watch_keys(void*, SDL_Event* e) {
        if (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) escape_at = SDL_GetTicks() | 1;
        return 0;
    }
    void go_exit() {
        if (current.exit) current.exit();
    }

    class Root : public Element {
    protected:
        std::string_view get_type_name() override { return "FirstRunRoot"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::MenuAction) return;
            MenuAction a = std::get<EventMenuAction>(e.variant).action;
            if (a == MenuAction::Back) go_back();
            // Toggle is Select on a controller and Esc on a keyboard: Select leaves setup, Esc goes back (the
            // design's cancel key on a computer).
            else if (a == MenuAction::Toggle) {
                Uint32 esc = escape_at.exchange(0);
                if (esc != 0 && SDL_GetTicks() - esc < 1000) go_back();
                else go_exit();
            }
        }
    public:
        Root(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::MenuAction), "div", false) {}
    };

    class Hints : public Element {
        recompinput::InputDevice shown_device = recompinput::InputDevice::COUNT;
        int shown_profile = -2;
        bool back = false, exit = false;
    protected:
        std::string_view get_type_name() override { return "FirstRunHints"; }
        void process_event(const Event& e) override {
            if (e.type != EventType::Update) return;
            auto [device, profile] = hint_input_info();
            if (device != shown_device || profile != shown_profile) {
                shown_device = device;
                shown_profile = profile;
                std::vector<HintItem> items{{{MenuAction::Accept}, "Choose"}};
                if (back) items.push_back({{MenuAction::Back}, "Back"});
                else if (exit && device == recompinput::InputDevice::Keyboard) items.push_back({{MenuAction::Back}, "Exit"});
                // Select is Exit on a controller; a keyboard's Esc (the same action) goes back, so there Exit is the
                // on-screen button only.
                if (exit && device != recompinput::InputDevice::Keyboard) items.push_back({{MenuAction::Toggle}, "Exit"});
                build_hint_row(get_current_context(), this, items, device, profile);
            }
            queue_update();
        }
    public:
        Hints(ResourceId rid, Element* parent, bool has_back, bool has_exit)
            : Element(rid, parent, Events(EventType::Update), "div", false), back(has_back), exit(has_exit) {
            set_position(Position::Absolute);
            set_left(0);
            set_right(0);
            set_bottom(0);
            set_height(64.0f);
            set_display(Display::Flex);
            set_justify_content(JustifyContent::Center);
            set_align_items(AlignItems::Center);
            set_gap(45.0f);
            queue_update();
        }
    };

    Label* text(Element* parent, const std::string& s, theme::Typography t, float size, Color color) {
        Label* l = context.create_element<Label>(parent, s, t);
        l->set_font_size(blitz::ui::px(size));
        l->set_color(color);
        return l;
    }

    Label* para(Element* parent, const std::string& s, float size, Color color) {
        Label* l = text(parent, s, theme::Typography::Body, size, color);
        l->set_white_space(WhiteSpace::Normal);
        l->set_line_height(blitz::ui::px(size * 1.35f));
        l->set_text_align(TextAlign::Left);
        return l;
    }

    // The address as a QR code: one box per dark module on a white square (as the Players tab draws its own,
    // controllers_tab.cpp). Drawn here, so the computer needs no network to show it.
    void draw_qr(Element* parent, const std::string& url) {
        auto code = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const float cell = blitz::ui::px(6.0f), quiet = 3 * cell;
        int n = code.getSize();
        Element* qr = context.create_element<Element>(parent, 0, "div", false);
        qr->set_position(Position::Relative);
        qr->set_flex_shrink(0.0f);
        qr->set_background_color(Color{255, 255, 255, 255});
        qr->set_width(n * cell + 2 * quiet);
        qr->set_height(n * cell + 2 * quiet);
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                if (!code.getModule(x, y)) continue;
                Element* m = context.create_element<Element>(qr, 0, "div", false);
                m->set_position(Position::Absolute);
                m->set_left(quiet + x * cell);
                m->set_top(quiet + y * cell);
                m->set_width(cell);
                m->set_height(cell);
                m->set_background_color(Color{0, 0, 0, 255});
            }
        }
    }

    // A check box row: the box (filled with a tick when on), the label and its smaller line. Flipped in place.
    // (No tick glyph in the UI's fonts: a filled box, and ON / OFF beside it as the settings' switches say.)
    void restyle_check(Element* box, Label* state, bool on) {
        box->set_background_color(on ? Color{246, 212, 0, 255} : Color{7, 15, 34, 255});
        state->set_text(on ? "ON" : "OFF");
        state->set_color(on ? Color{246, 212, 0, 255} : Color{184, 196, 218, 255});
    }

    void build() {
        const Screen& s = current;
        root = context.create_element<Root>(context.get_root_element());
        root->set_position(Position::Absolute);
        root->set_top(0);
        root->set_left(0);
        root->set_right(0);
        root->set_bottom(0);
        root->set_display(Display::Flex);
        root->set_align_items(AlignItems::Center);
        root->set_justify_content(JustifyContent::Center);
        root->set_background_color(Color{4, 10, 26, 245});
        Element* card = context.create_element<Element>(root);
        card->set_position(Position::Relative);
        card->set_width(blitz::ui::px(720));
        card->set_max_width(96.0f, Unit::Percent);  // a narrow window: the card narrows, the buttons wrap
        card->set_margin_bottom(blitz::ui::px(30));  // clear of the hints
        blitz::ui::PanelOptions o;
        o.corners = true;
        Element* content = blitz::ui::panel(context, card, o);
        content->set_display(Display::Flex);
        content->set_flex_direction(FlexDirection::Column);
        content->set_gap(blitz::ui::px(8));
        content->set_padding(blitz::ui::px(20));
        content->set_as_navigation_container(NavigationType::GridCol);

        if (!s.eyebrow.empty()) text(content, s.eyebrow, theme::Typography::LabelXS, 11, teal);
        Label* title = context.create_element<Label>(content, s.title, theme::Typography::Header2);
        title->set_color(theme::color::SecondaryL);
        Element* rule = context.create_element<Element>(content, 0, "div", false);
        rule->set_height(blitz::ui::px(4));
        rule->set_position(Position::Relative);
        blitz::ui::stretch(context, rule, "rule_flame");
        size_t from = 0;
        while (!s.body.empty() && from <= s.body.size()) {
            size_t nl = s.body.find('\n', from);
            std::string line = s.body.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            if (line.empty()) {
                Element* gap = context.create_element<Element>(content, 0, "div", false);
                gap->set_height(blitz::ui::px(4));
            }
            else para(content, line, 15, white);
            if (nl == std::string::npos) break;
            from = nl + 1;
        }

        if (!s.qr_url.empty()) {
            Element* row = context.create_element<Element>(content, 0, "div", false);
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Row);
            row->set_align_items(AlignItems::Center);
            row->set_justify_content(JustifyContent::Center);
            row->set_gap(blitz::ui::px(24));
            row->set_margin_top(blitz::ui::px(6));
            draw_qr(row, s.qr_url);
            Label* cap = text(row, s.qr_caption, theme::Typography::Header3, 20, teal);
            cap->set_white_space(WhiteSpace::Nowrap);
        }

        for (const auto& c : s.checks) {
            Button* b = context.create_element<Button>(content, "", ButtonStyle::Secondary, ButtonSize::Small);
            b->hide_focus_ring();
            b->set_height(blitz::ui::px(64));
            b->set_min_height(blitz::ui::px(64));
            b->set_max_height(blitz::ui::px(64));
            b->set_padding(blitz::ui::px(10));
            b->set_display(Display::Flex);
            b->set_flex_direction(FlexDirection::Row);
            b->set_align_items(AlignItems::Center);
            b->set_justify_content(JustifyContent::FlexStart);
            b->set_gap(blitz::ui::px(12));
            Element* box = context.create_element<Element>(b, 0, "div", false);
            box->set_width(blitz::ui::px(24));
            box->set_height(blitz::ui::px(24));
            box->set_min_width(blitz::ui::px(24));
            box->set_border_width(blitz::ui::px(2));
            box->set_border_radius(blitz::ui::px(3));
            box->set_border_color(gold);
            box->set_display(Display::Flex);
            box->set_align_items(AlignItems::Center);
            box->set_justify_content(JustifyContent::Center);
            Element* words = context.create_element<Element>(b, 0, "div", false);
            words->set_display(Display::Flex);
            words->set_flex_direction(FlexDirection::Column);
            words->set_align_items(AlignItems::FlexStart);
            words->set_gap(blitz::ui::px(2));
            text(words, c.label, theme::Typography::Header3, 17, white);
            Label* about = text(words, c.about, theme::Typography::Body, 12, dim);
            about->set_white_space(WhiteSpace::Normal);
            about->set_text_align(TextAlign::Left);
            words->set_flex_grow(1.0f);
            Label* tick = text(b, "", theme::Typography::LabelXS, 12, dim);
            tick->set_min_width(blitz::ui::px(34));
            restyle_check(box, tick, c.get());
            auto get = c.get;
            auto set = c.set;
            b->add_pressed_callback([box, tick, get, set] {
                bool v = !get();
                set(v);
                restyle_check(box, tick, v);
            });
        }

        Button* first_block = nullptr;
        if (!s.long_text.empty()) {
            // A long text (what a crash report holds) in blocks the D-pad can step through: focusing one scrolls the
            // box to it. The mouse wheel and touch drag scroll it too.
            Element* scroll = context.create_element<Element>(content, 0, "div", false);
            scroll->set_position(Position::Relative);
            scroll->set_height(blitz::ui::px(300));
            scroll->set_min_height(blitz::ui::px(300));
            scroll->set_flex_shrink(0.0f);
            scroll->set_overflow(Overflow::Hidden);
            scroll->set_overflow_y(Overflow::Auto);
            scroll->set_display(Display::Block);
            scroll->set_background_color(Color{2, 7, 18, 200});
            scroll->set_padding(blitz::ui::px(6));
            scroll->set_as_navigation_container(NavigationType::GridCol);
            for (const std::string& block : s.long_text) {
                // A button keeps its size class's height whatever is in it: each block is sized to its lines, one
                // label a line, cut at the box's edge rather than wrapped (so the count holds).
                std::vector<std::string> lines;
                for (size_t from = 0; from < block.size();) {
                    size_t nl = block.find('\n', from);
                    lines.push_back(block.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
                    if (nl == std::string::npos) break;
                    from = nl + 1;
                }
                const float line_h = blitz::ui::px(15.0f), pad = blitz::ui::px(4.0f);
                const float h = line_h * (float)lines.size() + 2 * pad;
                Button* b = context.create_element<Button>(scroll, "", ButtonStyle::Tertiary, ButtonSize::Small);
                b->hide_focus_ring();
                b->set_display(Display::Flex);
                b->set_flex_direction(FlexDirection::Column);
                b->set_align_items(AlignItems::FlexStart);
                b->set_justify_content(JustifyContent::FlexStart);
                b->set_height(h);
                b->set_min_height(h);
                b->set_max_height(h);
                b->set_margin_bottom(blitz::ui::px(2));
                b->set_padding(pad);
                // No fill or rim at rest: a button's own is drawn past the box's clip (only the focused one, scrolled
                // into view, shows its look).
                b->set_background_color(Color{0, 0, 0, 0});
                b->set_border_color(Color{0, 0, 0, 0});
                b->set_overflow(Overflow::Hidden);
                for (const std::string& line : lines) {
                    Label* l = text(b, line.empty() ? " " : line, theme::Typography::Body, 11, dim);
                    l->set_white_space(WhiteSpace::Nowrap);
                    l->set_height(line_h);
                    l->set_line_height(line_h);
                    l->set_text_align(TextAlign::Left);
                }
                b->set_focus_callback([b](bool f) { if (f) b->scroll_into_view(); });
                if (first_block == nullptr) first_block = b;
            }
        }

        if (!s.note.empty()) para(content, s.note, 13, gold);

        Button* first = first_block;
        Element* acts = context.create_element<Element>(content, 0, "div", false);
        acts->set_display(Display::Flex);
        acts->set_flex_direction(FlexDirection::Column);
        acts->set_gap(blitz::ui::px(6));
        acts->set_margin_top(blitz::ui::px(6));
        acts->set_as_navigation_container(NavigationType::GridCol);
        for (size_t i = 0; i < s.actions.size(); i++) {
            Button* b = context.create_element<Button>(acts, s.actions[i].label, i == 0 ? ButtonStyle::Primary : ButtonStyle::Secondary,
                                                       ButtonSize::Card);
            b->hide_focus_ring();
            b->set_min_height(blitz::ui::px(i == 0 ? 46 : 38));
            auto pressed = s.actions[i].pressed;
            b->add_pressed_callback([pressed] { if (pressed) pressed(); });
            if ((int)i == s.focus || first == nullptr || first == first_block) first = b;
        }

        if ((s.back && s.back_button) || s.exit) {
            Element* bar = context.create_element<Element>(content, 0, "div", false);
            bar->set_display(Display::Flex);
            bar->set_flex_direction(FlexDirection::Row);
            bar->set_justify_content(JustifyContent::SpaceBetween);
            bar->set_flex_wrap(FlexWrap::Wrap);
            bar->set_row_gap(blitz::ui::px(6));
            bar->set_margin_top(blitz::ui::px(6));
            bar->set_as_navigation_container(NavigationType::Horizontal);
            if (s.back && s.back_button) {
                Button* back = context.create_element<Button>(bar, "Back", ButtonStyle::Tertiary, ButtonSize::Card);
                back->hide_focus_ring();
                back->set_min_width(blitz::ui::px(110));
                back->add_pressed_callback(go_back);
            }
            else context.create_element<Element>(bar, 0, "div", false);  // Exit stays on the right
            if (s.exit) {
                Button* ex = context.create_element<Button>(bar, s.exit_label, ButtonStyle::Tertiary, ButtonSize::Card);
                ex->hide_focus_ring();
                ex->set_min_width(blitz::ui::px(110));
                ex->add_pressed_callback(go_exit);
                if (first == nullptr) first = ex;
            }
        }
        context.create_element<Hints>(root, (bool)s.back, (bool)s.exit);
        if (first != nullptr) context.set_autofocus_element(first);
    }
}

namespace blitz::firstrun_ui {
    void show(const Screen& screen) {
        static bool watching = false;
        if (!watching) {
            watching = true;
            SDL_AddEventWatch(watch_keys, nullptr);
        }
        blitz::ui::load();
        if (context == ContextId::null()) context = create_context();
        bool was_up = up && recompui::is_context_shown(context);
        if (was_up) recompui::hide_context(context);  // shown again below, so the new default button takes focus
        context.open();
        // The last screen goes (clear_children also takes it out of the root's child list, which destroy_resource
        // alone doesn't: navigation then walked a freed element).
        if (root != nullptr) {
            context.clear_children();
            root = nullptr;
        }
        current = screen;
        build();
        context.close();
        recompui::show_context(context, "");
        up = true;
    }

    void hide() {
        if (!up) return;
        up = false;
        if (context != ContextId::null() && recompui::is_context_shown(context)) recompui::hide_context(context);
    }

    bool shown() { return up && context != ContextId::null() && recompui::is_context_shown(context); }

    void update() {}
}
