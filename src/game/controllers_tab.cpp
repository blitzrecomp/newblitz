// Settings → Players: which controller plays as which player (src/main/controllers.cpp), with
// Identify (rumble and a white light bar), Make Player 1 and Swap, and phone controllers (QR code).
// The rows refresh while the tab is open.
#include <array>
#include <string>
#include <string_view>

#include "SDL2/SDL.h"
#include "blitz_platform.h"

#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_label.h"
#include "qrcodegen.hpp"
#include "../main/ui_kit.h"

namespace blitz {
    std::array<std::string, 4> port_descriptions();  // controllers.cpp
    void identify_port(int port);
    void swap_ports(int a, int b);
    std::string port_layout(int port);  // controllers.cpp: Auto, PlayStation, Xbox, Switch ("" no controller)
    void port_layout_next(int port);
    std::string phone_pads_url();  // phone_pads.cpp
    int phone_count();
}

namespace recompui {
    namespace {
        // The phone address on two lines, broken before its "?" (the part after it is the game's code).
        std::string two_lines(const std::string& url) {
            const size_t q = url.find('?');
            return q == std::string::npos ? url : url.substr(0, q) + "\n" + url.substr(q);
        }

        class ControllersPanel : public Element {
            std::array<Label*, 4> devices{};
            std::array<Button*, 4> identify{};
            std::array<Button*, 4> layout{};
            std::array<std::string, 4> shown_layout{};
            std::array<Element*, 4> cards{};
            std::array<std::string, 4> shown{"-", "-", "-", "-"};
            Button* phone_button = nullptr;
            Label* phone_text = nullptr;
            Label* phone_status = nullptr;
            Element* phone_status_box = nullptr;
            Element* qr = nullptr;
            std::string shown_url = "-";
            int shown_phones = -1;
            // Windows: the phone server's port brings up Windows Defender Firewall's "Allow access" the first time.
            // Said before it comes up, and again if no phone has connected 20 s after the code shows (the team's Windows test,
            // 2026-11-15).
            Uint32 code_shown_at = 0;
            bool firewall_hint = false;
            static bool on_windows() { return blitz::platform::name == std::string_view("Windows"); }

            static bool phones_on() {
                return std::get<bool>(recompui::config::get_general_config().get_option_value("phone_controllers"));
            }

            // The address as a QR code, one small box per dark module on a white square.
            void draw_qr(const std::string& url) {
                qr->clear_children();
                if (url.empty()) {
                    qr->set_display(Display::None);
                    return;
                }
                qr->set_display(Display::Block);
                ContextId context = get_current_context();
                auto code = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
                const float cell = 8.0f, quiet = 3 * cell;  // big enough to scan from the couch
                int n = code.getSize();
                qr->set_width(n * cell + 2 * quiet);
                qr->set_height(n * cell + 2 * quiet);
                for (int y = 0; y < n; y++) {
                    for (int x = 0; x < n; x++) {
                        if (!code.getModule(x, y)) continue;
                        auto m = context.create_element<Element>(qr, 0, "div", false);
                        m->set_position(Position::Absolute);
                        m->set_left(quiet + x * cell);
                        m->set_top(quiet + y * cell);
                        m->set_width(cell);
                        m->set_height(cell);
                        m->set_background_color(Color{0, 0, 0, 255});
                    }
                }
            }

        protected:
            std::string_view get_type_name() override { return "BlitzControllers"; }

            void process_event(const Event& e) override {
                if (e.type != EventType::Update) return;
                auto now = blitz::port_descriptions();
                for (int i = 0; i < 4; i++) {
                    const std::string lay = blitz::port_layout(i);
                    if (lay != shown_layout[i]) {
                        shown_layout[i] = lay;
                        layout[i]->set_text(lay.empty() ? "Buttons" : "Buttons: " + lay);
                        layout[i]->set_enabled(!lay.empty());
                    }
                }
                for (int i = 0; i < 4; i++) {
                    if (now[i] == shown[i]) continue;
                    shown[i] = now[i];
                    // An empty slot is quiet ("No controller", steel at 70%); the card's rim is the player's
                    // colour while a controller is connected.
                    devices[i]->set_text(now[i].empty() ? "No controller" : now[i]);
                    devices[i]->set_color(theme::color::TextDim, now[i].empty() ? 179 : 255);
                    blitz::ui::set_panel_frame(get_current_context(), cards[i],
                        now[i].empty() ? "panel_frame" : (std::string("panel_frame_p") + std::to_string(i + 1)).c_str());
                    identify[i]->set_enabled(!now[i].empty() && now[i] != "Keyboard");
                }
                std::string url = blitz::phone_pads_url();
                int count = blitz::phone_count();
                bool hint_now = on_windows() && code_shown_at != 0 && count == 0 && SDL_GetTicks() - code_shown_at >= 20000;
                if (url != shown_url || count != shown_phones || hint_now != firewall_hint) {
                    if (url != shown_url) code_shown_at = url.empty() ? 0 : SDL_GetTicks();
                    firewall_hint = hint_now;
                    shown_url = url;
                    shown_phones = count;
                    bool on = phones_on();
                    phone_button->set_text(on ? "Turn off" : "Turn on");
                    phone_button->apply_button_style(on ? ButtonStyle::Danger : ButtonStyle::Primary);
                    phone_status->set_text(on ? std::to_string(count) + " ON" : "OFF");
                    phone_status->set_color(on ? theme::color::Success : theme::color::TextDim);
                    phone_status_box->set_border_color(on ? theme::color::Success : theme::color::BorderSoft);
                    if (!on) {
                        phone_text->set_text(std::string("Turn on, then scan the code with a phone on the same Wi-Fi.") +
                                             (on_windows() ? " Windows will ask to let New Blitz use your network: "
                                                             "choose Allow access so phones can connect." : ""));
                    }
                    else if (url.empty()) {
                        phone_text->set_text(std::string("On, but this ") + (blitz::platform::desktop ? "computer" : "device") +
                                             " isn't on a network.");
                    }
                    else if (firewall_hint) {
                        phone_text->set_text("Phone can't connect? Check it's on the same Wi-Fi as this computer.\nThe address:\n" +
                                             two_lines(url));
                    }
                    else {
                        // One short line each (white-space: pre-line), so the address sits on its own. The address has no
                        // spaces to wrap at and ran into the next column (the Mac sweep, 2026-11-27): two lines, at its "?".
                        phone_text->set_text("Scan the code with a phone,\nor open " + two_lines(url) +
                                             "\nTurn the phone sideways to play.");
                    }
                    draw_qr(url);
                }
                queue_update();
            }

        public:
            ControllersPanel(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update), "div", false) {
                ContextId context = get_current_context();
                set_display(Display::Flex);
                set_flex_direction(FlexDirection::Column);
                set_padding(24.0f);
                set_gap(10.0f);
                // When the rows and the phone panel are taller than the tab (4:3, a window with the taskbar showing),
                // the tab scrolls: every child keeps its height (flex-shrink 0) rather than being squashed.
                set_height(100.0f, Unit::Percent);
                set_overflow_y(Overflow::Auto);
                set_width(100.0f, Unit::Percent);
                // Controller/arrow navigation as a grid: each card is a row (Identify, Make Player 1, Swap) and up/down
                // keeps the column. Left to guessing, up/down jumped between rows unpredictably.
                set_as_navigation_container(NavigationType::GridCol);

                // Player colours (docs/design/redesign/tokens.json).
                static const Color chip[4] = {{58, 123, 255, 255}, {255, 77, 77, 255}, {47, 211, 107, 255}, {255, 210, 63, 255}};
                for (int i = 0; i < 4; i++) {
                    auto box = context.create_element<Element>(this, 0, "div", false);
                    blitz::ui::PanelOptions po;
                    po.grid = false;
                    auto card = blitz::ui::panel(context, box, po);
                    cards[i] = box;
                    box->set_flex_shrink(0.0f);
                    card->set_display(Display::Flex);
                    card->set_flex_direction(FlexDirection::Row);
                    card->set_align_items(AlignItems::Center);
                    card->set_gap(18.0f);
                    card->set_height(81.0f);  // 54 px
                    card->set_padding_left(18.0f);
                    card->set_padding_right(18.0f);
                    box->set_as_navigation_container(NavigationType::GridRow);

                    // A numbered chip in the player's colour, Midnight numeral.
                    auto dot = context.create_element<Element>(card, 0, "div", false);
                    dot->set_width(60.0f);
                    dot->set_height(54.0f);
                    dot->set_flex_shrink(0.0f);
                    dot->set_border_radius(4.5f);
                    dot->set_background_color(chip[i]);
                    dot->set_display(Display::Flex);
                    dot->set_align_items(AlignItems::Center);
                    dot->set_justify_content(JustifyContent::Center);
                    auto num = context.create_element<Label>(dot, std::to_string(i + 1), theme::Typography::Header3);
                    num->set_color(Color{2, 7, 18, 255});

                    auto who = context.create_element<Label>(card, "PLAYER " + std::to_string(i + 1), theme::Typography::Header3);
                    who->set_font_size(45.0f);
                    who->set_flex_shrink(0.0f);
                    who->set_white_space(WhiteSpace::Nowrap);
                    devices[i] = context.create_element<Label>(card, "No controller", theme::Typography::Body);
                    devices[i]->set_font_size(30.0f);
                    devices[i]->set_font_weight(600);
                    devices[i]->set_flex_grow(1.0f);
                    devices[i]->set_flex_shrink(1.0f);
                    devices[i]->set_white_space(WhiteSpace::Nowrap);
                    devices[i]->set_overflow(Overflow::Hidden);
                    // Which way its face buttons go (controllers.cpp apply_layout): a pad that says it's a Switch Pro but has
                    // PlayStation buttons can be told (team, 2026-11-27).
                    layout[i] = context.create_element<Button>(card, "Buttons: Auto", ButtonStyle::Tertiary, ButtonSize::Card);
                    layout[i]->set_flex_shrink(0.0f);
                    layout[i]->set_white_space(WhiteSpace::Nowrap);
                    layout[i]->set_enabled(false);
                    layout[i]->add_pressed_callback([i]() { blitz::port_layout_next(i); });
                    identify[i] = context.create_element<Button>(card, "Identify", ButtonStyle::Secondary, ButtonSize::Card);
                    identify[i]->set_flex_shrink(0.0f);
                    identify[i]->set_enabled(false);
                    identify[i]->add_pressed_callback([i]() { blitz::identify_port(i); });
                    // Player 1 drives the game's menus, so any controller (a phone at a party) can take it. A hidden
                    // button keeps its width, so the columns line up for up and down.
                    auto first = context.create_element<Button>(card, "Make Player 1", ButtonStyle::Tertiary, ButtonSize::Card);
                    first->set_flex_shrink(0.0f);
                    first->set_white_space(WhiteSpace::Nowrap);
                    if (i > 0) first->add_pressed_callback([i]() { blitz::swap_ports(0, i); });
                    else {
                        first->set_visibility(Visibility::Hidden);
                        first->set_enabled(false);
                    }
                    auto swap = context.create_element<Button>(card, i < 3 ? "Swap with " + std::to_string(i + 2) : "Swap", ButtonStyle::Tertiary, ButtonSize::Card);
                    swap->set_min_width(200.0f);
                    swap->set_flex_shrink(0.0f);
                    swap->set_white_space(WhiteSpace::Nowrap);
                    if (i < 3) swap->add_pressed_callback([i]() { blitz::swap_ports(i, i + 1); });
                    else {
                        swap->set_visibility(Visibility::Hidden);
                        swap->set_enabled(false);  // never focused while hidden
                    }
                }

                // Phones as controllers (src/main/phone_pads.cpp): a grid panel, the controls on the left and how
                // players join on the right.
                auto phones_box = context.create_element<Element>(this, 0, "div", false);
                phones_box->set_margin_top(6.0f);
                phones_box->set_flex_shrink(0.0f);
                auto phones = blitz::ui::panel(context, phones_box, blitz::ui::PanelOptions{});
                phones->set_display(Display::Flex);
                phones->set_flex_direction(FlexDirection::Row);
                phones->set_align_items(AlignItems::FlexStart);
                phones->set_gap(30.0f);
                phones->set_padding(24.0f);
                phones_box->set_as_navigation_container(NavigationType::GridRow);
                auto left = context.create_element<Element>(phones, 0, "div", false);
                left->set_display(Display::Flex);
                left->set_flex_direction(FlexDirection::Row);
                left->set_gap(24.0f);
                left->set_flex_grow(1.0f);
                left->set_flex_shrink(1.0f);
                qr = context.create_element<Element>(left, 0, "div", false);
                qr->set_position(Position::Relative);
                qr->set_flex_shrink(0.0f);
                qr->set_background_color(Color{255, 255, 255, 255});
                qr->set_display(Display::None);
                auto phone_col = context.create_element<Element>(left, 0, "div", false);
                phone_col->set_display(Display::Flex);
                phone_col->set_flex_direction(FlexDirection::Column);
                phone_col->set_align_items(AlignItems::Stretch);
                phone_col->set_gap(15.0f);
                phone_col->set_flex_grow(1.0f);
                phone_col->set_flex_shrink(1.0f);
                phone_col->set_min_width(420.0f);  // its longest line
                // The heading and its count badge: side by side, the badge on its own line below when they don't fit.
                auto title_row = context.create_element<Element>(phone_col, 0, "div", false);
                title_row->set_display(Display::Flex);
                title_row->set_flex_wrap(FlexWrap::Wrap);
                title_row->set_align_items(AlignItems::Center);
                title_row->set_column_gap(18.0f);
                title_row->set_row_gap(9.0f);
                auto title = context.create_element<Label>(title_row, "PHONE CONTROLLERS", theme::Typography::Header3);
                title->set_color(theme::color::Secondary);
                title->set_white_space(WhiteSpace::Nowrap);
                phone_status_box = context.create_element<Element>(title_row, 0, "div", false);
                phone_status_box->set_height(42.0f);
                phone_status_box->set_flex_shrink(0.0f);
                phone_status_box->set_padding_left(12.0f);
                phone_status_box->set_padding_right(12.0f);
                phone_status_box->set_border_width(3.0f);
                phone_status_box->set_border_radius(theme::border::radius_sm);
                phone_status_box->set_border_color(theme::color::BorderSoft);
                phone_status_box->set_display(Display::Flex);
                phone_status_box->set_align_items(AlignItems::Center);
                phone_status = context.create_element<Label>(phone_status_box, "OFF", theme::Typography::LabelMD);
                phone_status->set_font_size(27.0f);
                phone_status->set_white_space(WhiteSpace::Nowrap);
                phone_text = context.create_element<Label>(phone_col, "", theme::Typography::Body);
                phone_text->set_color(theme::color::TextDim);
                phone_text->set_white_space(WhiteSpace::Preline);  // a line each (and room for one more below)
                phone_text->set_font_size(27.0f);
                phone_text->set_line_height(39.0f);
                auto button_row = context.create_element<Element>(phone_col, 0, "div", false);  // the button at its own width
                button_row->set_display(Display::Flex);
                button_row->set_flex_direction(FlexDirection::Row);
                phone_button = context.create_element<Button>(button_row, "Turn on", ButtonStyle::Primary, ButtonSize::Large);
                phone_button->set_white_space(WhiteSpace::Nowrap);
                phone_button->add_pressed_callback([]() {
                    recomp::config::Config& g = recompui::config::get_general_config();
                    g.update_option_value("phone_controllers", !std::get<bool>(g.get_option_value("phone_controllers")));
                    g.save_config();
                });

                auto right = context.create_element<Element>(phones, 0, "div", false);
                right->set_display(Display::Flex);
                right->set_flex_direction(FlexDirection::Column);
                right->set_gap(9.0f);
                right->set_width(540.0f);  // 360 px; narrow windows down to 300 px (bullets wrap)
                right->set_min_width(450.0f);
                right->set_max_width(720.0f);  // wide windows: a bullet a line
                right->set_flex_grow(1.0f);
                right->set_flex_shrink(1.0f);
                auto join = context.create_element<Label>(right, "HOW PLAYERS JOIN", theme::Typography::LabelXS);
                join->set_color(Color{143, 239, 245, 255});
                join->set_margin_bottom(6.0f);
                for (const char* line : {
                        "Turn a pad on: it takes the next free player.",
                        "Reconnect and you get your player back.",
                        "The keyboard always plays as player 1.",
                        "Pick sides on the game's Controller Select.",
                        "DualSense: hold Create + PS, then pair in Bluetooth."}) {
                    auto bullet = context.create_element<Label>(right, std::string("\xE2\x80\xA2  ") + line, theme::Typography::Body);
                    bullet->set_font_size(27.0f);
                    bullet->set_line_height(33.0f);
                    bullet->set_color(theme::color::TextDim);
                }
                queue_update();
            }
        };
    }
}

namespace blitz {
    void create_controllers_tab() {
        recompui::config::create_tab("Players", "players", [](recompui::ContextId context, recompui::Element* parent) {
            context.create_element<recompui::ControllersPanel>(parent);
        });
    }
}
