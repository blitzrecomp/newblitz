// The Blitz '27 house style for everything around the game (docs/design/redesign: tokens.json,
// IMPLEMENTATION.md §0): midnight and navy grounds, dark glass panels with a cyan rim, Blitz yellow
// headings and one hot accent, the Blitz red focus bar. Display type is Barlow Condensed italic (bundled,
// OFL; registered in main.cpp). Applied before the UI is built (config.cpp).
#include "elements/ui_theme.h"

namespace blitz {
    const char* display_font = "Barlow Condensed";

    void apply_ui_theme() {
        using recompui::Color;
        using C = recompui::theme::color;
        auto set = [](C c, Color v) { recompui::theme::set_theme_color(c, v); };
        set(C::Background1, {2, 7, 18, 255});      // Midnight
        set(C::Background2, {7, 15, 34, 255});     // Navy: inputs, tracks, the tab strip
        set(C::Background3, {13, 26, 58, 255});    // Deep navy: idle buttons, pills and rows
        set(C::ModalOverlay, {2, 7, 18, 214});
        set(C::BGOverlay, {2, 7, 18, 170});         // behind prompts: dims the game (was a pale wash)
        set(C::BGShadow2, {2, 7, 18, 200});
        set(C::Text, {242, 242, 242, 255});
        set(C::TextDim, {184, 196, 218, 255});     // Steel
        set(C::Border, {143, 239, 245, 178});      // the cyan rim
        set(C::BorderSoft, {143, 239, 245, 61});
        set(C::Elevated, {6, 18, 44, 246});        // panel glass (review S2: ~4% of the game shows through)
        set(C::ElevatedSoft, {13, 26, 58, 140});
        // Accents with their alpha steps.
        auto family = [&](C base, C l, C d, C a5, C a20, C a30, C a50, C a80, Color rgb, Color light, Color dark) {
            set(base, rgb);
            set(l, light);
            set(d, dark);
            set(a5, {rgb.r, rgb.g, rgb.b, 13});
            set(a20, {rgb.r, rgb.g, rgb.b, 51});
            set(a30, {rgb.r, rgb.g, rgb.b, 77});
            set(a50, {rgb.r, rgb.g, rgb.b, 128});
            set(a80, {rgb.r, rgb.g, rgb.b, 204});
        };
        // Primary: the Blitz red focus bar. Secondary: Blitz yellow (L: the selected yellow, on the red bar).
        family(C::Primary, C::PrimaryL, C::PrimaryD, C::PrimaryA5, C::PrimaryA20, C::PrimaryA30, C::PrimaryA50, C::PrimaryA80,
               {179, 16, 30, 255}, {226, 52, 64, 255}, {110, 8, 18, 255});
        family(C::Secondary, C::SecondaryL, C::SecondaryD, C::SecondaryA5, C::SecondaryA20, C::SecondaryA30, C::SecondaryA50,
               C::SecondaryA80, {246, 212, 0, 255}, {255, 225, 74, 255}, {200, 160, 0, 255});
        family(C::Success, C::SuccessL, C::SuccessD, C::SuccessA5, C::SuccessA20, C::SuccessA30, C::SuccessA50, C::SuccessA80,
               {69, 224, 74, 255}, {155, 247, 151, 255}, {18, 157, 12, 255});
        family(C::Danger, C::DangerL, C::DangerD, C::DangerA5, C::DangerA20, C::DangerA30, C::DangerA50, C::DangerA80,
               {255, 106, 61, 255}, {255, 160, 130, 255}, {163, 40, 10, 255});
        family(C::Warning, C::WarningL, C::WarningD, C::WarningA5, C::WarningA20, C::WarningA30, C::WarningA50, C::WarningA80,
               {246, 212, 0, 255}, {255, 225, 74, 255}, {200, 160, 0, 255});

        recompui::theme::set_border_radius_sm(4.5f);  // design 3 / 4 / 6 / 2 px, x1.5 (dp)
        recompui::theme::set_border_radius_md(6.0f);
        recompui::theme::set_border_radius_lg(9.0f);
        recompui::theme::set_border_width(3.0f);

        using T = recompui::theme::Typography;
        using S = recompui::FontStyle;
        // Sizes in design px at 960x720; recompui's dp is height / 1080, so x1.5.
        auto type = [](T t, float size, float track, uint32_t weight, S style, const char* family) {
            recompui::theme::set_typography_preset(t, size * 1.5f, track, weight, style, family);
        };
        type(T::Header1, 56, .02f, 800, S::Italic, display_font);
        type(T::Header2, 40, .02f, 800, S::Italic, display_font);
        type(T::Header3, 30, .03f, 800, S::Italic, display_font);
        type(T::LabelLG, 34, .03f, 800, S::Italic, display_font);
        type(T::LabelMD, 24, .04f, 700, S::Italic, display_font);
        type(T::LabelSM, 20, .10f, 700, S::Normal, nullptr);
        type(T::LabelXS, 18, .12f, 700, S::Normal, nullptr);
        type(T::Body, 20, 0, 500, S::Normal, nullptr);
    }
}
