#include "ui_kit.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "util/file.h"

namespace {
    using namespace recompui;

    // Each panel remembers its nine frame images, so the frame can be swapped later.
    std::unordered_map<Element*, std::array<Image*, 9>> panel_pieces;
    const char* parts[9] = {"tl", "t", "tr", "l", "c", "r", "bl", "b", "br"};

    // An absolutely placed holder (any of left/top/right/bottom; NaN = unset) with an image filling it.
    Image* fill(ContextId ctx, Element* parent, const std::string& name, float l, float t, float r, float b, float w, float h) {
        Element* holder = ctx.create_element<Element>(parent, 0, "div", false);
        holder->set_position(Position::Absolute);
        if (l == l) holder->set_left(l);
        if (t == t) holder->set_top(t);
        if (r == r) holder->set_right(r);
        if (b == b) holder->set_bottom(b);
        if (w == w) holder->set_width(w);
        if (h == h) holder->set_height(h);
        holder->set_pointer_events(PointerEvents::None);
        Image* img = ctx.create_element<Image>(holder, blitz::ui::src(name));
        img->set_width(100.0f, Unit::Percent);
        img->set_height(100.0f, Unit::Percent);
        img->set_pointer_events(PointerEvents::None);
        return img;
    }
    constexpr float none = std::numeric_limits<float>::quiet_NaN();
}

namespace blitz::ui {
    // The two full-screen backgrounds come in three sizes: 4K for computers and the PS5, 1920x1080 on phones and tablets, and
    // 1366x768 in Potato Mode. As textures, the 4K title is 33 MB of GPU memory and the swoosh 15 MB.
    static bool big_background(const std::string& name) { return name == "bg_title" || name == "bg_overlay_swoosh"; }

    std::string src(const std::string& name) {
#if defined(__ANDROID__) || defined(BLITZ_IOS)
        if (big_background(name)) return "?/blitz/ui/" + name + (getenv("BLITZ_LEAN") != nullptr ? "_s" : "_m");
#endif
        return "?/blitz/ui/" + name;
    }

    void load() {
        static std::once_flag once;
        std::call_once(once, [] {
            std::error_code ec;
            std::filesystem::path dir = recompui::file::get_asset_path("ui");
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.path().extension() != ".png") continue;
                // Only the size of each background that src() hands out.
                const std::string stem = e.path().stem().string();
                const size_t cut = stem.size() > 2 && stem[stem.size() - 2] == '_' ? stem.size() - 2 : stem.size();
                const std::string base = stem.substr(0, cut);
                if (big_background(base) && src(base) != "?/blitz/ui/" + stem) continue;
                std::ifstream f(e.path(), std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                recompui::queue_image_from_bytes_file(src(e.path().stem().string()), bytes);
            }
        });
    }

    Image* image(ContextId ctx, Element* parent, const std::string& name, float left, float top, float w, float h) {
        return fill(ctx, parent, name, px(left), px(top), none, none, px(w), px(h));
    }

    Image* stretch(ContextId ctx, Element* parent, const std::string& name) {
        return fill(ctx, parent, name, 0, 0, 0, 0, none, none);
    }

    Element* panel(ContextId ctx, Element* box, const PanelOptions& o) {
        load();
        if (!o.absolute) box->set_position(Position::Relative);
        float s = px(o.inset), sx = px(o.inset_x >= 0 ? o.inset_x : o.inset);
        std::string f = o.frame;
        std::array<Image*, 9> p{};
        p[0] = fill(ctx, box, f + ".tl", 0, 0, none, none, sx, s);
        p[1] = fill(ctx, box, f + ".t", sx, 0, sx, none, none, s);
        p[2] = fill(ctx, box, f + ".tr", none, 0, 0, none, sx, s);
        p[3] = fill(ctx, box, f + ".l", 0, s, none, s, sx, none);
        p[4] = fill(ctx, box, f + ".c", sx, s, sx, s, none, none);
        p[5] = fill(ctx, box, f + ".r", none, s, 0, s, sx, none);
        p[6] = fill(ctx, box, f + ".bl", 0, none, none, 0, sx, s);
        p[7] = fill(ctx, box, f + ".b", sx, none, sx, 0, none, s);
        p[8] = fill(ctx, box, f + ".br", none, none, 0, 0, sx, s);
        panel_pieces[box] = p;
        if (o.grid) {
            // The teal grid, repeated over the frame's centre piece (an image decorator on its holder).
            {
                Element* g = ctx.create_element<Element>(box, 0, "div", false);
                g->set_position(Position::Absolute);
                g->set_left(px(6));
                g->set_top(px(6));
                g->set_right(px(6));
                g->set_bottom(px(6));
                g->set_pointer_events(PointerEvents::None);
                g->set_decorator("image(" + src("grid_tile") + " repeat)");
            }
        }
        if (o.corners || o.corners_tl_br) {
            float c = px(o.corner_size), out = px(-5.0f);
            fill(ctx, box, "chrome_corner_tl", out, out, none, none, c, c);
            if (!o.corners_tl_br) fill(ctx, box, "chrome_corner_tr", none, out, out, none, c, c);
            if (!o.corners_tl_br) fill(ctx, box, "chrome_corner_bl", out, none, none, out, c, c);
            fill(ctx, box, "chrome_corner_br", none, none, out, out, c, c);
        }
        Element* content = ctx.create_element<Element>(box, 0, "div", false);
        content->set_position(Position::Relative);
        content->set_width(100.0f, Unit::Percent);  // its own height; a fixed-size box sets it to 100%
        return content;
    }

    void set_panel_frame(ContextId ctx, Element* box, const char* frame) {
        (void)ctx;
        auto it = panel_pieces.find(box);
        if (it == panel_pieces.end()) return;
        for (int i = 0; i < 9; i++) it->second[i]->set_src(src(std::string(frame) + "." + parts[i]));
    }

    void register_decorators() {
        recompui::set_panel_decorator([](ContextId ctx, Element* box, const char* kind) -> Element* {
            std::string k = kind;
            if (k == "prompt") {
                PanelOptions o;
                o.corners = true;
                return panel(ctx, box, o);
            }
            if (k == "modal" || k == "description") {
                // Decorations only; the framework keeps adding its own children to the box. The frame and
                // grid draw first (absolute), so in-flow content paints over them.
                PanelOptions o;
                o.corners = k == "modal";
                o.absolute = k == "modal";
                Element* content = panel(ctx, box, o);
                if (k == "modal") {
                    content->set_display(Display::None);  // unused: the modal fills itself
                    return content;                       // != box: the framework skips its own border
                }
                return content;
            }
            if (k == "modal_backdrop") {
                stretch(ctx, box, "bg_overlay_swoosh")->get_parent()->set_opacity(0.85f);
                return box;
            }
            if (k == "modal_header") {
                // A darker band for the tabs and buttons, with a soft cyan line under it.
                box->set_background_color(Color{2, 7, 18, 200});
                return box;
            }
            if (k == "prompt_title") {
                PanelOptions o;
                o.frame = "header_bevel";
                o.inset = 20;
                o.inset_x = 24;
                o.grid = false;
                return panel(ctx, box, o);
            }
            return box;
        });
    }

    std::pair<Element*, Element*> selector_caps(ContextId ctx, Element* row) {
        load();
        auto cap = [&](const char* name, bool left) {
            Element* holder = ctx.create_element<Element>(row, 0, "div", false);
            holder->set_position(Position::Absolute);
            // Each cap overlaps the bar's end by 6 px and stands 2 px proud above and below (review P2).
            holder->set_top(px(-2));
            holder->set_bottom(px(-2));
            holder->set_width(px(12));
            if (left) holder->set_left(px(-6));
            else holder->set_right(px(-6));
            holder->set_display(Display::Flex);
            holder->set_flex_direction(FlexDirection::Column);
            holder->set_pointer_events(PointerEvents::None);
            for (const char* part : {"t", "c", "b"}) {
                Element* seg = ctx.create_element<Element>(holder, 0, "div", false);
                if (part[0] == 'c') seg->set_flex_grow(1.0f);
                else seg->set_height(px(14));
                seg->set_width(px(12));
                Image* img = ctx.create_element<Image>(seg, src(std::string(name) + "." + part));
                img->set_width(100.0f, Unit::Percent);
                img->set_height(100.0f, Unit::Percent);
            }
            return holder;
        };
        return {cap("selector_cap_l", true), cap("selector_cap_r", false)};
    }
}
