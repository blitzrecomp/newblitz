// Instant Replay: the last play, full screen, with the game stopped behind it (docs/REPLAY.md). Opened from the game's
// pause menu (src/game/replay.cpp), F9, or BLITZ DS's REPLAY button. The frames are RT64's (rt64_blitz_replay.cpp), which
// draws one in place of the game's picture; everything here is the Blitz '27 look over it, in its own UI context, plus
// the choosing of what's kept.
//
// Threads: replay_update is the main thread's. The buttons and the scrubber's drag call back on the present thread, which
// runs the UI, and replay_event runs wherever SDL takes the event (on Android, the Java thread), so they only set atomics.
// Anything may call replay_open, replay_close and replay_save.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "core/ui_context.h"
#include "elements/ui_clickable.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"
#include "util/file.h"

#include "blitz_audio.h"
#include "blitz_platform.h"
#include "blitz_replay.h"
#include "platform_compat.h"
#include "replay_gif.h"
#include "ui_kit.h"

extern "C" int blitz_hotkey_held(void);  // controllers.cpp: the hotkey (R3, or L3 on a one-stick handheld) held
extern "C" {
    extern volatile int blitz_ds_mode;
    extern volatile int blitz_ds_second_on_top;
    int blitz_replay_frame_tall(int index);
    extern volatile int blitz_replay_enabled;
    extern volatile int blitz_replay_capture_on;
    extern volatile int blitz_replay_width;
    extern volatile int blitz_replay_height;
    extern volatile int blitz_replay_length;
    extern volatile int blitz_replay_interval_ms;
    extern volatile int blitz_replay_mark_next;
    extern volatile int blitz_replay_show;
    extern volatile int blitz_replay_frame;
    extern volatile int blitz_replay_keep_presenting;
    extern volatile int blitz_replay_hold;
    int blitz_replay_count(void);
    int blitz_replay_frame_info(int index, float* aspect, int* ms, int* mark);
    int blitz_replay_is_bgra(void);
    void blitz_replay_read_start(int index, unsigned char* out);
    int blitz_replay_read_done(void);
    int blitz_play_select_on_screen(void);  // gfx_ext.cpp
}

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp: shown as a toast
    float fps_last_second();                   // fps_counter.cpp
#if defined(__ANDROID__)
    void host_add_to_gallery(const std::filesystem::path& file, const char* mime);  // platform_android.cpp
#endif
}

namespace {
    using Clock = std::chrono::steady_clock;
    using namespace recompui;

    enum class Screen { Closed, Stopping, Open };
    std::atomic<Screen> screen{Screen::Closed};
    Clock::time_point stopping_since;

    // From any thread.
    std::atomic_bool want_open{false}, want_close{false}, want_save{false};
    std::atomic_bool screen_up{false};  // replay_is_open
    // From the buttons, on the present thread.
    std::atomic_bool press_play{false}, press_speed{false}, press_show{false}, touched{false};
    std::atomic_bool press_hide{false};  // HIDE (Start, H): the panel away, or back (team)
    bool panel_hidden = false;           // the tick's: put away by HIDE, until HIDE or a tap on the picture
    std::atomic_int press_step{0};
    std::atomic<float> drag_to{-1.0f};  // 0-1 along the scrubber, while being dragged
    std::atomic_bool dragging{false};
    std::atomic<float> track_left{0.0f}, track_width{1.0f};

    // Saving, on its own thread; the ring holds still meanwhile.
    std::atomic_bool saving{false};
    std::atomic_int save_percent{0};

    struct FrameInfo {
        float aspect = 1.0f;
        int ms = 0;
        bool mark = false;
    };
    std::vector<FrameInfo> frames;
    std::vector<int> marks;  // frames that start a play, in order, and 0
    double playhead = 0.0;   // ms into the clip, from the first frame
    bool playing = true;
    int speed = 0;           // an index into speeds
    constexpr double speeds[] = {1.0, 0.5, 0.25};
    constexpr const char* speed_names[] = {"FULL SPEED", "1/2 SPEED", "1/4 SPEED"};
    int play_from = 0;       // the frame the loop starts at: this play's first
    Clock::time_point last_tick, opened_at, last_input, end_reached;
    bool at_end = false;
    int saved_ds_mode = 0;

    // The directions being held: keyboard, D-pad, stick.
    std::atomic_int held_keys{0}, held_dpad{0}, held_stick{0};
    Clock::time_point held_since;
    int held_last = 0;
    bool was_playing = false;

    // The look.
    ContextId context = ContextId::null();
    Element* root = nullptr;
    Element* flash = nullptr;
    Element* banner = nullptr;
    Element* bug = nullptr;
    Element* bug_dot = nullptr;
    Element* controls = nullptr;
    Element* fill = nullptr;
    Element* thumb = nullptr;
    Element* track = nullptr;
    Label* clock_label = nullptr;
    Label* speed_label = nullptr;
    Label* play_label = nullptr;
    Label* save_label = nullptr;
    constexpr int max_ticks = 16;
    Element* ticks[max_ticks] = {};
    struct Glyph {
        Label* label;
        SDL_GameControllerButton pad;
        SDL_Scancode key;
    };
    std::vector<Glyph> glyphs;
    float look_opacity = -1.0f, look_banner = -1.0f, look_flash = -1.0f, look_bug = -1.0f;
    int look_frame = -1, look_percent = -1;
    bool look_playing = false, look_first = true;
    int look_speed = -1;

    const Color yellow{246, 212, 0, 255};
    const Color cyan{143, 239, 245, 255};
    const Color steel{184, 196, 218, 255};
    const Color flame{255, 106, 61, 255};
    const Color red{226, 52, 64, 255};

    Element* place(Element* e, float l, float t, float r, float b) {
        e->set_position(Position::Absolute);
        if (l == l) e->set_left(l);
        if (t == t) e->set_top(t);
        if (r == r) e->set_right(r);
        if (b == b) e->set_bottom(b);
        return e;
    }
    constexpr float unset = std::numeric_limits<float>::quiet_NaN();

    Label* label(Element* parent, const std::string& text, float size, const Color& color, bool display_font = true) {
        Label* l = context.create_element<Label>(parent, text, theme::Typography::LabelXS);
        l->set_font_size(size);
        l->set_color(color);
        l->set_pointer_events(PointerEvents::None);
        if (display_font) {
            l->set_font_family("Barlow Condensed");
            l->set_font_style(FontStyle::Italic);
            l->set_font_weight(800);
        }
        return l;
    }

    // A chip with the press's glyph and a word: not focusable, so the controller's keys stay this file's.
    Label* button(Element* row, SDL_GameControllerButton pad, SDL_Scancode key, const std::string& text, std::function<void()> action) {
        Clickable* b = context.create_element<Clickable>(row, false);
        b->set_position(Position::Relative);
        b->set_display(Display::Flex);
        b->set_flex_direction(FlexDirection::Row);
        b->set_align_items(AlignItems::Center);
        b->set_gap(10.0f);
        b->set_height(62.0f);
        b->set_padding_left(20.0f);
        b->set_padding_right(22.0f);
        b->set_background_color(Color{13, 26, 58, 235});
        b->set_border_width(2.0f);
        b->set_border_color(Color{143, 239, 245, 97});
        b->set_border_radius(6.0f);
        b->add_pressed_callback([action](float, float) { action(); });
        if (pad != SDL_CONTROLLER_BUTTON_INVALID || key != SDL_SCANCODE_UNKNOWN) {
            Label* g = label(b, "", 38.0f, cyan, false);
            g->set_font_family("promptfont");
            glyphs.push_back({g, pad, key});
        }
        return label(b, text, 38.0f, Color{242, 242, 242, 255});
    }

    void track_moved(float x) {
        float width = track_width.load();
        if (width > 0.0f) drag_to = std::clamp((x - track_left.load()) / width, 0.0f, 1.0f);
    }

    void build() {
        blitz::ui::load();
        context = create_context();
        context.open();
        context.set_captures_input(true);
        context.set_captures_mouse(true);
        root = place(context.create_element<Element>(context.get_root_element()), 0, 0, 0, 0);
        root->set_background_color(theme::color::Transparent);

        // A tap on the picture: the controls back, and pause or play.
        Clickable* picture = context.create_element<Clickable>(root, false);
        place(picture, 0, 0, 0, 0);
        picture->add_pressed_callback([](float, float) { press_show = true; });

        // The take-over: one white frame, gone at once.
        flash = place(context.create_element<Element>(root), 0, 0, 0, 0);
        flash->set_background_color(Color{255, 255, 255, 255});
        flash->set_pointer_events(PointerEvents::None);

        // INSTANT REPLAY slams in from the left, over a flame.
        banner = place(context.create_element<Element>(root), 48, 30, unset, unset);
        banner->set_width(640.0f);
        banner->set_height(118.0f);
        banner->set_pointer_events(PointerEvents::None);
        {
            Element* plate = place(context.create_element<Element>(banner), 0, 0, 0, 12.0f);
            plate->set_pointer_events(PointerEvents::None);
            blitz::ui::PanelOptions o;
            o.frame = "header_bevel";
            o.inset = 20;
            o.inset_x = 24;
            o.grid = false;
            o.absolute = true;
            Element* inside = blitz::ui::panel(context, plate, o);
            inside->set_height(100.0f, Unit::Percent);
            inside->set_display(Display::Flex);
            inside->set_align_items(AlignItems::Center);
            inside->set_justify_content(JustifyContent::Center);
            inside->set_pointer_events(PointerEvents::None);
            label(inside, "INSTANT REPLAY", 82.0f, yellow);
            Element* rule = place(context.create_element<Element>(banner, 0, "div", false), 0, unset, 0, 0);
            rule->set_height(8.0f);
            rule->set_pointer_events(PointerEvents::None);
            blitz::ui::stretch(context, rule, "rule_flame");
        }

        // With the controls away: a small mark, so it can't be taken for the game.
        bug = place(context.create_element<Element>(root), unset, 30, 48, unset);
        bug->set_display(Display::Flex);
        bug->set_flex_direction(FlexDirection::Row);
        bug->set_align_items(AlignItems::Center);
        bug->set_gap(12.0f);
        bug->set_pointer_events(PointerEvents::None);
        {
            bug_dot = context.create_element<Element>(bug);
            bug_dot->set_width(20.0f);
            bug_dot->set_height(20.0f);
            bug_dot->set_border_radius(10.0f);
            bug_dot->set_background_color(red);
            bug_dot->set_pointer_events(PointerEvents::None);
            label(bug, "REPLAY", 48.0f, yellow);
        }

        // The scrubber and the buttons, on the house's panel.
        controls = place(context.create_element<Element>(root), 48, unset, 48, 30);
        controls->set_height(212.0f);
        {
            blitz::ui::PanelOptions o;
            o.corners = true;
            o.absolute = true;
            Element* inside = blitz::ui::panel(context, controls, o);
            inside->set_height(100.0f, Unit::Percent);
            inside->set_display(Display::Flex);
            inside->set_flex_direction(FlexDirection::Column);
            inside->set_justify_content(JustifyContent::SpaceBetween);
            inside->set_padding_top(30.0f);
            inside->set_padding_bottom(26.0f);
            inside->set_padding_left(46.0f);
            inside->set_padding_right(46.0f);

            Element* top = context.create_element<Element>(inside);
            top->set_display(Display::Flex);
            top->set_flex_direction(FlexDirection::Row);
            top->set_align_items(AlignItems::Center);
            top->set_gap(30.0f);
            {
                // Tall enough for a thumb; the bar's in the middle of it.
                Clickable* bar = context.create_element<Clickable>(top, true);
                bar->set_position(Position::Relative);
                bar->set_flex_grow(1.0f);
                bar->set_height(64.0f);
                bar->add_pressed_callback([bar](float x, float) {
                    track_left = bar->get_absolute_left();
                    track_width = bar->get_client_width();
                    track_moved(x);
                });
                bar->add_dragged_callback([bar](float x, float, DragPhase phase) {
                    track_left = bar->get_absolute_left();
                    track_width = bar->get_client_width();
                    dragging = phase != DragPhase::End;
                    track_moved(x);
                });
                track = bar;
                Element* groove = place(context.create_element<Element>(bar), 0, 22, 0, unset);
                groove->set_height(20.0f);
                groove->set_background_color(Color{7, 15, 34, 255});
                groove->set_border_width(2.0f);
                groove->set_border_color(Color{143, 239, 245, 128});
                groove->set_border_radius(3.0f);
                groove->set_pointer_events(PointerEvents::None);
                fill = place(context.create_element<Element>(groove, 0, "div", false), 0, 0, unset, 0);
                fill->set_overflow(Overflow::Hidden);
                fill->set_pointer_events(PointerEvents::None);
                blitz::ui::stretch(context, fill, "rule_flame");
                for (Element*& t : ticks) {
                    t = place(context.create_element<Element>(bar), 0, 14, unset, unset);
                    t->set_width(4.0f);
                    t->set_height(36.0f);
                    t->set_margin_left(-2.0f);
                    t->set_background_color(cyan);
                    t->set_display(Display::None);
                    t->set_pointer_events(PointerEvents::None);
                }
                // The thumb: yellow between the house's chrome caps.
                thumb = place(context.create_element<Element>(bar), 0, 4, unset, unset);
                thumb->set_width(14.0f);
                thumb->set_height(56.0f);
                thumb->set_margin_left(-7.0f);
                thumb->set_background_color(yellow);
                thumb->set_pointer_events(PointerEvents::None);
                blitz::ui::selector_caps(context, thumb);
                clock_label = label(top, "0.0", 48.0f, yellow);
                clock_label->set_min_width(112.0f);
                clock_label->set_text_align(TextAlign::Right);
            }

            Element* row = context.create_element<Element>(inside);
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Row);
            row->set_align_items(AlignItems::Center);
            row->set_gap(18.0f);
            {
                play_label = button(row, SDL_CONTROLLER_BUTTON_A, SDL_SCANCODE_SPACE, "PAUSE", [] { press_play = true; });
                button(row, SDL_CONTROLLER_BUTTON_INVALID, SDL_SCANCODE_UNKNOWN, "<", [] { press_step = -1; });
                button(row, SDL_CONTROLLER_BUTTON_INVALID, SDL_SCANCODE_UNKNOWN, ">", [] { press_step = 1; });
                speed_label = button(row, SDL_CONTROLLER_BUTTON_B, SDL_SCANCODE_V, "FULL SPEED", [] { press_speed = true; });
                Element* gap = context.create_element<Element>(row);
                gap->set_flex_grow(1.0f);
                button(row, SDL_CONTROLLER_BUTTON_START, SDL_SCANCODE_H, "HIDE", [] { press_hide = true; });
                save_label = button(row, SDL_CONTROLLER_BUTTON_Y, SDL_SCANCODE_S, "SAVE", [] { blitz::replay_save(); });
                button(row, SDL_CONTROLLER_BUTTON_X, SDL_SCANCODE_BACKSPACE, "BACK", [] { blitz::replay_close(); });  // □, as every Back
            }
        }
        context.close();
    }

    // ---- what's kept, and how big: the ring's size comes from the memory there is, once a run ----

    struct Tier {
        const char* name;
        int width, height, length, interval_ms;
    };
    // 16:9 frames. A tall or 4:3 picture is stretched into them and back (RT64 keeps each frame's shape), so one size
    // serves every window and BLITZ DS. Memory is width x height x 4 x length.
    constexpr Tier tiers[] = {
        {"small", 320, 180, 120, 66},   //  28 MB, 8 s at 15 fps: 1-2 GB handhelds
        {"medium", 480, 270, 160, 50},  //  83 MB, 8 s at 20 fps
        {"large", 640, 360, 250, 40},   // 230 MB, 10 s at 25 fps
        {"full", 960, 540, 300, 33},    // 622 MB, 10 s at 30 fps: computers with room
    };

    constexpr Uint32 after_whistle_ms = 2000;  // kept past the whistle: the hit after it, the ball carrier getting up

    bool setting_on() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("instant_replay"));
    }

    // Too slow to spare it: half the frames, then none, as BLITZ DS's clip goes (ds_view.cpp). Never back up in a run.
    int too_slow_steps = 0;
    void hold_frame_rate(bool capturing) {
        static const float floor_fps = getenv("BLITZ_REPLAY_FPS_FLOOR") ? (float)atof(getenv("BLITZ_REPLAY_FPS_FLOOR")) : 26.0f;
        static Clock::time_point next = Clock::now() + std::chrono::seconds(30);
        static int slow = 0;
        if (!capturing || too_slow_steps >= 2 || Clock::now() < next) return;
        next = Clock::now() + std::chrono::seconds(5);
        float fps = blitz::fps_last_second();
        if (fps <= 0.0f) return;
        slow = fps < floor_fps ? slow + 1 : 0;
        if (slow < 3) return;
        slow = 0;
        next = Clock::now() + std::chrono::seconds(30);
        too_slow_steps++;
        if (too_slow_steps == 1) {
            blitz_replay_interval_ms = blitz_replay_interval_ms * 2;
            fprintf(stderr, "[replay] too slow (%.1f fps): a frame every %d ms\n", fps, (int)blitz_replay_interval_ms);
        }
        else {
            fprintf(stderr, "[replay] too slow (%.1f fps): off for this run\n", fps);
        }
    }

    void update_keeping() {
        static bool chosen = false;
        if (!chosen) {
            chosen = true;
            int mb = SDL_GetSystemRAM();
            int t = mb <= 0 ? 1 : mb < 2500 ? 0 : mb < 5000 ? 1 : mb < 12000 ? 2 : 3;
            if (blitz::platform::touch_controls) t = std::min(t, 2);  // a phone's GPU memory is everyone's
#if defined(BLITZ_PLATFORM_PS5)
            // SDL counts only part of a PS5's memory (2185 MB on the team's, 2026-11-15), which picked "small", the 1-2 GB
            // handhelds' tier: a 320x180 replay that looked rough on a TV. The console has far more: "large" at least.
            t = std::max(t, 2);
#endif
            if (const char* e = getenv("BLITZ_REPLAY_TIER")) t = std::clamp(atoi(e), 0, 3);
            const Tier& tier = tiers[t];
            blitz_replay_width = tier.width;
            blitz_replay_height = tier.height;
            blitz_replay_length = tier.length;
            blitz_replay_interval_ms = tier.interval_ms;
            fprintf(stderr, "[replay] %s: %d frames at %dx%d, one every %d ms (%d MB of RAM here)\n", tier.name, tier.length, tier.width, tier.height, tier.interval_ms, mb);
        }
        static Uint32 last_check = 0, left_game_at = 0;
        static bool on = false;
        Uint32 now = SDL_GetTicks();
        if (now - last_check < 50) return;
        last_check = now;
        if (!ultramodern::is_game_started()) return;
        // The small tier is about 28 MB; an RG DS (950 MB) has room for that with ~290 MB free in a game (2026-11-02). Below
        // 800 MB, none. Too slow still turns it down, then off (hold_frame_rate).
        on = setting_on() && SDL_GetSystemRAM() >= 800;
        const bool in_game = blitz::replay_phase.load() == 2;
        // Between quarters (and at halftime) the game's phase is 3 for ~12 s: the ring stays, so the quarter's last play is
        // still there at the next line-up. It was freed 5 s into the break before 1.0.4.
        if (in_game || blitz::replay_phase.load() == 3) left_game_at = now;
        // Its memory goes a moment after a game ends, so the walk from the end of one to its stats doesn't drop it.
        const bool keep = on && too_slow_steps < 2 && (in_game || now - left_game_at < 5000);
        // Kept: from the snap to a moment after the whistle (late hits), nothing between. The play just run stays whole in
        // the ring through play select and the next play's lining up, until the ball's snapped (team, 1.0.3 on an iPhone: a
        // replay opened at the line showed the next play's line-up, which the end of play select used to start).
        static unsigned seen_snaps = 0;
        static Uint32 live_until = 0;
        const unsigned snaps = blitz::replay_snaps.load();
        if (snaps != seen_snaps) {
            seen_snaps = snaps;
            blitz_replay_mark_next = 1;  // the snap: the next frame kept starts a play
        }
        if (in_game && blitz::replay_play_live.load()) live_until = now + after_whistle_ms;
        const bool in_play = in_game && (int32_t)(live_until - now) > 0;
        const bool select = blitz_play_select_on_screen() != 0;
        const bool capture = keep && in_play && !select && !blitz::replay_pause_menu.load() && screen == Screen::Closed;
        blitz_replay_enabled = keep ? 1 : 0;
        blitz_replay_capture_on = capture ? 1 : 0;
        hold_frame_rate(capture);
    }

    // ---- saving: to <app folder>/replays, this play, as a GIF ----

    void save_thread(int from, int to, std::vector<FrameInfo> infos) {
        const int src_w = blitz_replay_width, src_h = blitz_replay_height;
        const bool bgra = blitz_replay_is_bgra() != 0;
        // At most 270 lines, 480 wide, and about 15 frames a second: 3 to 15 MB for a play.
        float aspect = infos[(size_t)from].aspect;
        int h = std::min(src_h, 270), w = (int)std::lround(h * aspect);
        if (w > 480) {
            w = 480;
            h = (int)std::lround(w / aspect);
        }
        w = std::max(w, 2) & ~1;
        h = std::max(h, 2) & ~1;

        std::error_code ec;
        std::filesystem::path dir = recompui::file::get_app_folder_path() / "replays";
        std::filesystem::create_directories(dir, ec);
        time_t tt = time(nullptr);
        char name[64];
        strftime(name, sizeof name, "Blitz Replay %Y-%m-%d at %H.%M.%S.gif", localtime(&tt));
        std::filesystem::path path = dir / name, tmp = path;
        tmp += ".tmp";

        blitz::GifWriter gif;
        bool ok = gif.open(tmp, w, h);
        std::vector<uint8_t> src((size_t)src_w * src_h * 4), out((size_t)w * h * 4);
        int i = from;
        while (ok && i <= to) {
            int next = i + 1;
            while (next <= to && infos[(size_t)next].ms - infos[(size_t)i].ms < 60) next++;
            blitz_replay_read_start(i, src.data());
            auto waited = Clock::now();
            while (!blitz_replay_read_done() && Clock::now() - waited < std::chrono::seconds(3)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!blitz_replay_read_done()) {
                fprintf(stderr, "[replay] save: frame %d never came back\n", i);
                ok = false;
                break;
            }
            for (int y = 0; y < h; y++) {
                const uint8_t* row = &src[(size_t)(y * src_h / h) * src_w * 4];
                uint8_t* o = &out[(size_t)y * w * 4];
                for (int x = 0; x < w; x++) {
                    const uint8_t* p = row + (size_t)(x * src_w / w) * 4;
                    o[x * 4 + 0] = bgra ? p[2] : p[0];
                    o[x * 4 + 1] = p[1];
                    o[x * 4 + 2] = bgra ? p[0] : p[2];
                    o[x * 4 + 3] = 255;
                }
            }
            int ms = (next <= to ? infos[(size_t)next].ms : infos[(size_t)i].ms + 66) - infos[(size_t)i].ms;
            ok = gif.add_frame(out.data(), std::clamp(ms / 10, 2, 100));
            save_percent = 100 * (i - from + 1) / std::max(to - from + 1, 1);
            i = next;
        }
        ok = gif.close() && ok;
        if (ok) {
            blitz::sync_file(tmp);
            blitz::replace_file(tmp, path, ec);
            ok = !ec;
        }
        if (!ok) std::filesystem::remove(tmp, ec);
        blitz_replay_hold = 0;
        fprintf(stderr, "[replay] save: %s\n", ok ? path.string().c_str() : "failed");
#if defined(__ANDROID__)
        if (ok) blitz::host_add_to_gallery(path, "image/gif");  // the gallery's Pictures/New Blitz, as screenshots go
        // BRANDING: gallery folder name in a toast (Android: Pictures/New Blitz)
        blitz::post_status(ok ? "Replay: saved to Pictures/New Blitz" : "Replay: couldn't save it");
#else
        blitz::post_status(ok ? "Replay: saved to the replays folder" : "Replay: couldn't save it");
#endif
        saving = false;
    }

    // ---- the screen ----

    int frame_at(double ms) {
        int lo = 0, hi = (int)frames.size() - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (frames[(size_t)mid].ms <= ms) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

    // The play this frame's in, for looping and saving: its first frame, and its last.
    int play_start(int f) {
        int start = 0;
        for (int m : marks) {
            if (m <= f) start = m;
        }
        return start;
    }
    int play_end(int f) {
        for (int m : marks) {
            if (m > f) return m - 1;
        }
        return (int)frames.size() - 1;
    }

    double first_ms() { return frames.empty() ? 0.0 : frames.front().ms; }
    double last_ms() { return frames.empty() ? 0.0 : frames.back().ms; }

    // BLITZ DS: each frame where it was kept (rt64_blitz_replay.cpp), over both screens or on the upper one alone, not as the
    // screens are now: Stylus's plays were spread over both, small in the middle (team), and a play that switched mid-way
    // would have gone wrong.
    void place_frame(int f) {
        if (saved_ds_mode == 0) return;
        blitz_ds_mode = blitz_replay_frame_tall(f) ? 1 : (blitz_ds_second_on_top != 0) ? 3 : 2;
    }

    void begin() {
        frames.clear();
        marks.clear();
        int n = blitz_replay_count();
        int kept_at = 0, along = 0;
        for (int i = 0; i < n; i++) {
            FrameInfo f;
            int mark = 0;
            if (!blitz_replay_frame_info(i, &f.aspect, &f.ms, &mark)) break;
            // Its own clock, without the time between plays (play select, a pause), when nothing was kept.
            if (i > 0) along += std::min(f.ms - kept_at, 200);
            kept_at = f.ms;
            f.ms = along;
            f.mark = mark != 0;
            if (f.mark && i > 0) marks.push_back(i);
            frames.push_back(f);
        }
        // The last play, unless it's only just started (under a second and a half: the one before).
        int start = 0;
        for (auto it = marks.rbegin(); it != marks.rend(); ++it) {
            if (frames.back().ms - frames[(size_t)*it].ms >= 1500) {
                start = *it;
                break;
            }
        }
        play_from = start;
        playhead = frames.empty() ? 0.0 : frames[(size_t)start].ms;
        playing = true;
        speed = 0;
        at_end = false;
        held_keys = held_dpad = held_stick = 0;
        held_last = 0;
        dragging = false;
        drag_to = -1.0f;
        press_play = press_speed = press_show = touched = press_hide = false;
        panel_hidden = false;
        press_step = 0;

        if (context == ContextId::null()) build();
        context.open();
        const double span = std::max(last_ms() - first_ms(), 1.0);
        for (int i = 0; i < max_ticks; i++) {
            bool on = i < (int)marks.size();
            ticks[i]->set_display(on ? Display::Block : Display::None);
            if (on) ticks[i]->set_left((float)(100.0 * (frames[(size_t)marks[(size_t)i]].ms - first_ms()) / span), Unit::Percent);
        }
        // Glyphs for what's at hand: a controller's if there is one, else the keyboard's, and none for a finger.
        const bool pad = SDL_NumJoysticks() > 0;
        for (const Glyph& g : glyphs) {
            std::string text;
            if (pad && g.pad != SDL_CONTROLLER_BUTTON_INVALID) text = recompinput::InputField::controller_digital(g.pad).to_string();
            else if (!pad && blitz::platform::desktop && g.key != SDL_SCANCODE_UNKNOWN) text = recompinput::InputField::keyboard(g.key).to_string();
            g.label->set_text(text);
            g.label->set_display(text.empty() ? Display::None : Display::Block);
        }
        context.close();
        look_first = true;

        opened_at = last_input = last_tick = Clock::now();
        saved_ds_mode = blitz_ds_mode;
        blitz_replay_frame = frame_at(playhead);
        place_frame(blitz_replay_frame);
        blitz_replay_show = 1;
        blitz::audio::set_mute(true);
        recompui::show_context(context, "");
        fprintf(stderr, "[replay] open: %zu frames, %zu plays, from frame %d\n", frames.size(), marks.size() + 1, start);
        if (getenv("BLITZ_REPLAY_TRACE")) {
            std::string list;
            for (int m : marks) list += " " + std::to_string(m) + "@" + std::to_string(frames[(size_t)m].ms);
            fprintf(stderr, "[replay] plays start at frames:%s; the last frame at %d ms\n", list.c_str(), frames.empty() ? 0 : frames.back().ms);
        }
    }

    void end() {
        recompui::hide_context(context);
        blitz_replay_show = 0;
        if (saved_ds_mode != 0) blitz_ds_mode = saved_ds_mode;
        blitz::audio::set_mute(false);
        blitz_replay_keep_presenting = 0;
        blitz::replay_input_held = false;
        blitz::replay_gate_request = false;
        screen_up = false;
        fprintf(stderr, "[replay] closed\n");
    }

    void update_look(Clock::time_point now, int frame) {
        auto ms = [](auto d) { return std::chrono::duration<float, std::milli>(d).count(); };
        const float since_open = ms(now - opened_at);

        // Up for three seconds after anything's touched, while it plays on; always up while it doesn't.
        const bool up = !panel_hidden && (!playing || saving.load() || dragging.load() || ms(now - last_input) < 3000.0f);
        const float target = up ? 1.0f : 0.0f;
        const float fade = ms(now - last_tick) / 140.0f;
        const float o = look_opacity < 0.0f ? target : look_opacity < target ? std::min(look_opacity + fade, target) : std::max(look_opacity - fade, target);
        // The banner slams in, a touch too far, and settles.
        const float t = std::clamp(since_open / 220.0f, 0.0f, 1.0f);
        const float slam = t < 0.7f ? -760.0f * (1.0f - t / 0.7f) * (1.0f - t / 0.7f) + 26.0f * (t / 0.7f) : 26.0f * (1.0f - (t - 0.7f) / 0.3f);
        const float flash_now = std::clamp(0.85f * (1.0f - since_open / 160.0f), 0.0f, 0.85f);
        const float bug_now = (1.0f - o) * ((std::fmod(since_open, 1000.0f) < 620.0f) ? 1.0f : 0.25f);

        context.open();
        if (o != look_opacity) {
            controls->set_opacity(o);
            controls->set_display(o > 0.01f ? Display::Block : Display::None);
            banner->set_opacity(o);
            look_opacity = o;
        }
        if (slam != look_banner || look_first) {
            banner->set_translate_2D(slam, 0.0f);
            look_banner = slam;
        }
        if (flash_now != look_flash) {
            flash->set_opacity(flash_now);
            flash->set_display(flash_now > 0.0f ? Display::Block : Display::None);
            look_flash = flash_now;
        }
        if (bug_now != look_bug) {
            bug->set_opacity(1.0f - o);
            bug_dot->set_opacity(bug_now);
            look_bug = bug_now;
        }
        if (frame != look_frame || look_first) {
            const double span = std::max(last_ms() - first_ms(), 1.0);
            const float at = (float)(100.0 * (frames[(size_t)frame].ms - first_ms()) / span);
            fill->set_width(at, Unit::Percent);
            thumb->set_left(at, Unit::Percent);
            char text[16];
            snprintf(text, sizeof text, "%.1f", (frames[(size_t)frame].ms - frames.back().ms) / 1000.0);
            clock_label->set_text(text);
            look_frame = frame;
        }
        if (playing != look_playing || look_first) {
            play_label->set_text(playing ? "PAUSE" : "PLAY");
            look_playing = playing;
        }
        if (speed != look_speed) {
            speed_label->set_text(speed_names[speed]);
            look_speed = speed;
        }
        const int percent = saving.load() ? save_percent.load() : -1;
        if (percent != look_percent) {
            save_label->set_text(percent >= 0 ? "SAVING " + std::to_string(percent) + "%" : "SAVE");
            look_percent = percent;
        }
        context.close();
        look_first = false;
    }

    void tick() {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double, std::milli>(now - last_tick).count();

        if (touched.exchange(false)) last_input = now;
        if (press_play.exchange(false)) {
            playing = !playing;
            at_end = false;
            last_input = now;
        }
        if (press_speed.exchange(false)) {
            speed = (speed + 1) % 3;
            last_input = now;
        }
        if (press_hide.exchange(false)) {
            panel_hidden = !panel_hidden;
            last_input = now;
        }
        const bool tapped = press_show.exchange(false);
        if (tapped && panel_hidden) {
            panel_hidden = false;  // put away by HIDE: a tap only brings them back
            last_input = now;
        }
        else if (tapped) {
            // A tap when they're up pauses; when they're away it only brings them back.
            if (std::chrono::duration<double, std::milli>(now - last_input).count() < 3000.0 || !playing) {
                playing = !playing;
                at_end = false;
            }
            last_input = now;
        }
        int step = press_step.exchange(0);
        const int dir = held_keys.load() != 0 ? held_keys.load() : held_dpad.load() != 0 ? held_dpad.load() : held_stick.load();
        if (dir != held_last) {
            if (dir != 0 && held_last == 0) {
                was_playing = playing;
                playing = false;
                held_since = now;
                step = dir;  // A press steps one frame at once; held, it runs.
            }
            else if (dir == 0) {
                playing = was_playing;
            }
            held_last = dir;
            at_end = false;
            last_input = now;
        }

        int frame = frame_at(playhead);
        if (step != 0) {
            frame = std::clamp(frame + step, 0, (int)frames.size() - 1);
            playhead = frames[(size_t)frame].ms;
            playing = false;
            at_end = false;
            last_input = now;
        }
        else if (dir != 0 && now - held_since > std::chrono::milliseconds(220)) {
            playhead = std::clamp(playhead + dir * dt * 1.6, first_ms(), last_ms());
        }
        const float dragged = drag_to.exchange(-1.0f);
        if (dragged >= 0.0f) {
            playhead = first_ms() + dragged * (last_ms() - first_ms());
            playing = false;
            at_end = false;
            last_input = now;
        }
        else if (playing && dir == 0) {
            if (at_end) {
                // A beat on the last frame, then round again from the top of this play.
                if (now - end_reached > std::chrono::milliseconds(900)) {
                    at_end = false;
                    playhead = frames[(size_t)play_from].ms;
                }
            }
            else {
                playhead += dt * speeds[speed];
                const int end_frame = play_end(play_from);
                if (playhead >= frames[(size_t)end_frame].ms) {
                    playhead = frames[(size_t)end_frame].ms;
                    at_end = true;
                    end_reached = now;
                }
            }
        }
        frame = frame_at(playhead);
        play_from = play_start(frame);
        blitz_replay_frame = frame;
        place_frame(frame);
        update_look(now, frame);
        last_tick = now;
    }
}

namespace blitz {
    void replay_open() { want_open = true; }
    void replay_close() { want_close = true; }
    void replay_save() { want_save = true; }
    bool replay_is_open() { return screen_up.load(); }

    // Main thread, every tick (main.cpp's update_gfx).
    void replay_update() {
        update_keeping();
        switch (screen.load()) {
            case Screen::Closed:
                want_close = false;
                want_save = false;
                if (want_open.exchange(false)) {
                    if (blitz_replay_count() < 8) {
                        post_status(blitz_replay_enabled ? "Replay: nothing to show yet" : "Replay: off (Settings, General)");
                        break;
                    }
                    if (saving.load()) {
                        post_status("Replay: still saving the last one");
                        break;
                    }
                    if (recompui::is_context_capturing_input() || replay_phase.load() != 2) break;  // Settings is up, or no game
                    blitz_replay_capture_on = 0;
                    blitz_replay_keep_presenting = 1;
                    screen_up = true;
                    replay_input_held = true;
                    replay_gate_request = true;
                    stopping_since = std::chrono::steady_clock::now();
                    screen = Screen::Stopping;
                }
                break;
            case Screen::Stopping:
                want_open = false;
                if (replay_gated.load()) {
                    begin();
                    screen = Screen::Open;
                }
                else if (std::chrono::steady_clock::now() - stopping_since > std::chrono::seconds(1)) {
                    // Loading something: the dispatcher's not coming round.
                    fprintf(stderr, "[replay] the game didn't stop\n");
                    post_status("Replay: not while it's loading");
                    blitz_replay_keep_presenting = 0;
                    replay_input_held = false;
                    replay_gate_request = false;
                    screen_up = false;
                    screen = Screen::Closed;
                }
                break;
            case Screen::Open:
                want_open = false;
                if (want_save.exchange(false) && !saving.load() && !frames.empty()) {
                    const int f = frame_at(playhead);
                    saving = true;
                    save_percent = 0;
                    blitz_replay_hold = 1;
                    std::thread(save_thread, play_start(f), play_end(f), frames).detach();
                    last_input = std::chrono::steady_clock::now();
                }
                if (want_close.exchange(false)) {
                    end();
                    screen = Screen::Closed;
                    break;
                }
                // Every few milliseconds is plenty: each pass takes the UI's lock.
                if (std::chrono::steady_clock::now() - last_tick >= std::chrono::milliseconds(8)) tick();
                break;
        }
    }

    // Main thread, for every SDL event (capture.cpp), before F5 and the rest. True: it was ours.
    bool replay_event(const SDL_Event& e) {
        const Screen now_up = screen.load();
        if (now_up != Screen::Open) {
            if (now_up == Screen::Closed && e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F9) {
                replay_open();
                return true;
            }
            return now_up != Screen::Closed;
        }
        switch (e.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                const bool down = e.type == SDL_KEYDOWN;
                const auto key = e.key.keysym.scancode;
                touched = true;
                if (key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT) {
                    const int d = key == SDL_SCANCODE_LEFT ? -1 : 1;
                    if (down) held_keys = d;
                    else if (held_keys.load() == d) held_keys = 0;
                }
                else if (down && !e.key.repeat) {
                    if (key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_RETURN) press_play = true;
                    else if (key == SDL_SCANCODE_S) replay_save();
                    else if (key == SDL_SCANCODE_V) press_speed = true;
                    else if (key == SDL_SCANCODE_H) press_hide = true;
                    else if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_F9) replay_close();
                }
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
                const auto b = e.cbutton.button;
                if (down && blitz_hotkey_held()) break;  // L3 / R3 held: the press is a hotkey combo's, not the replay's
                touched = true;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                    const int d = b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1;
                    if (down) held_dpad = d;
                    else if (held_dpad.load() == d) held_dpad = 0;
                }
                else if (down) {
                    if (b == SDL_CONTROLLER_BUTTON_A) press_play = true;
                    else if (b == SDL_CONTROLLER_BUTTON_X) replay_close();
                    else if (b == SDL_CONTROLLER_BUTTON_Y) replay_save();
                    else if (b == SDL_CONTROLLER_BUTTON_B) press_speed = true;
                    else if (b == SDL_CONTROLLER_BUTTON_START) press_hide = true;
                }
                break;
            }
            case SDL_CONTROLLERAXISMOTION:
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    const float v = (float)e.caxis.value / 32768.0f;
                    const int d = v > 0.5f ? 1 : v < -0.5f ? -1 : 0;
                    if (d != held_stick.load()) touched = true;
                    held_stick = d;
                }
                break;
            case SDL_MOUSEMOTION:
            case SDL_FINGERDOWN:
                touched = true;
                break;
            default:
                break;
        }
        return true;
    }
}
