// Unit tests for Settings → Two Screens' presets and Custom Layout (src/game/two_screens.h). Run with `make test-screens`.
// Checks the rule that picks each dial's value, and that every built-in value is exactly tall_view.cpp's own start value
// (read from that file), so a player who never touches Custom Layout sees what 1.0.4 showed.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "../src/game/two_screens.h"

using namespace blitz::two_screens;

static int failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-5; }

static std::map<std::string, float> as_map(const std::vector<std::pair<std::string, float>>& v) {
    std::map<std::string, float> m;
    for (const auto& [k, x] : v) m[k] = x;
    return m;
}

// tall_view.cpp's dials: the name TALL takes -> its variable (set_setting), and each variable's start value.
static std::map<std::string, double> tall_view_start_values(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    std::map<std::string, std::string> var_of;
    const std::regex setter(R"re(strcmp\(name, "([a-z_]+)"\)\)\s*([a-z_]+) = value;)re");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), setter); it != std::sregex_iterator(); ++it) var_of[(*it)[1]] = (*it)[2];
    std::map<std::string, double> start;
    for (const auto& [name, var] : var_of) {
        const std::regex init("[ ,]" + var + R"re( = (-?[0-9.]+)f)re");
        std::smatch m;
        if (std::regex_search(src, m, init)) start[name] = std::stod(m[1]);
    }
    return start;
}

int main(int argc, char** argv) {
    const std::string tall_view = argc > 1 ? argv[1] : "src/game/tall_view.cpp";

    // The table: unique ids, each a dial tall_view.cpp knows, built-ins inside their own ranges.
    {
        const auto start = tall_view_start_values(tall_view);
        CHECK(start.size() >= 30);  // read it at all
        std::set<std::string> ids;
        for (const Dial& d : dials()) {
            CHECK(ids.insert(d.id).second);
            CHECK(start.count(d.tall) == 1);
            CHECK(d.builtin >= d.min && d.builtin <= d.max);
            if (d.choices != nullptr) {
                int n = 0;
                while (d.choices[n] != nullptr) n++;
                CHECK(near(d.max, n - 1));
            }
            // Built-in values: tall_view.cpp's own, except the camera's two, which the presets set on two screens.
            const std::string id = d.id;
            if (id != tilt_id && id != zoom_id && start.count(d.tall)) {
                if (!near(d.builtin, start.at(d.tall))) fprintf(stderr, "  %s: built-in %g, tall_view.cpp %g\n", d.id, d.builtin, start.at(d.tall));
                CHECK(near(d.builtin, start.at(d.tall)));
            }
        }
        // tall_view's own tilt and zoom (0.65, 1.15) are the dev kit's; two screens always took them from the presets.
        CHECK(find(tilt_id) != nullptr && near(find(tilt_id)->builtin, preset_tilt(2)));       // Field Ahead's default
        CHECK(find(zoom_id) != nullptr && near(find(zoom_id)->builtin, preset_zoom_percent(3)));  // Camera Zoom Out's
    }

    // Custom Layout off: the presets, as config.cpp set them before 1.0.5.
    {
        const float zoom_before[] = {1.0f, 1.05f, 1.10f, 1.125f, 1.15f, 1.20f, 1.25f, 1.30f};
        for (uint32_t t = 0; t < 6; t++) {
            for (uint32_t z = 0; z < 8; z++) {
                auto m = as_map(resolve(false, t, z, {}));
                CHECK(near(m["tilt"], 0.40f + 0.05f * (float)t));
                CHECK(near(m["zoom"], zoom_before[z]));
                CHECK(near(m["panel"], -29.0));
                CHECK(near(m["score_lift"], 8.0));
                CHECK(near(m["msg_up"], 10.0));
            }
        }
        // Whatever Custom Layout saved is ignored while it's off.
        auto m = as_map(resolve(false, 2, 3, {{"panel", 12.0}, {tilt_id, 0.9}}));
        CHECK(near(m["panel"], -29.0));
        CHECK(near(m["tilt"], 0.50));
    }

    // Turning it on starts from what was showing: nothing moves until a dial does.
    for (uint32_t t = 0; t < 6; t++) {
        for (uint32_t z = 0; z < 8; z++) {
            auto off = as_map(resolve(false, t, z, {}));
            auto on = as_map(resolve(true, t, z, seed_from_presets(t, z)));
            for (const auto& [k, v] : off) CHECK(near(on[k], v));
        }
    }

    // Custom Layout on: the player's values, kept inside each dial's range; one missing is its built-in.
    {
        auto m = as_map(resolve(true, 0, 0, {{"panel", 12.0}, {tilt_id, 0.8}, {zoom_id, 40.0}, {"msg_up", 999.0}, {"score", 1.0}}));
        CHECK(near(m["panel"], 12.0));
        CHECK(near(m["tilt"], 0.8));
        CHECK(near(m["zoom"], 1.40));
        CHECK(near(m["msg_up"], 60.0));    // clamped to its max
        CHECK(near(m["score"], 1.0));
        CHECK(near(m["score_lift"], 8.0)); // not saved: built-in
        // Past the presets' range: the point of Custom Layout.
        CHECK(m["tilt"] > preset_tilt(5) + 0.1);
    }

    // Out-of-range preset choices (a hand-edited file) stay on the last choice, as ds_zoom_for did.
    CHECK(near(preset_zoom_percent(99), 30.0));
    CHECK(near(preset_tilt(99), 0.65));

    if (failures) {
        fprintf(stderr, "two_screens: %d failure(s)\n", failures);
        return 1;
    }
    printf("two_screens: all passed (%zu dials)\n", dials().size());
    return 0;
}
