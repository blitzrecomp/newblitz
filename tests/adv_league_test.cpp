// Unit tests for the Advanced league model (src/game/adv_league.cpp). Run with `make test-adv`.
// Uses the built-in pack's 2026 divisions and schedule (packs/blitz2026/pack.json).
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>

#include "json/json.hpp"
#include "../src/game/adv_league.h"

using namespace adv;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static std::vector<std::string> ids;
static int id(const std::string& f) {
    for (size_t i = 0; i < ids.size(); i++) if (ids[i] == f) return (int)i;
    return -1;
}

static League load_league(const char* path) {
    nlohmann::json pack = nlohmann::json::parse(std::ifstream(path));
    for (const auto& f : pack["order"]) ids.push_back(f.get<std::string>());
    League lg;
    lg.members.assign(divisions, std::vector<int>(division_size));
    for (int d = 0; d < divisions; d++) lg.conference.push_back(d / 4);
    const auto& season = pack["season"];
    for (int d = 0; d < divisions; d++) {
        for (int k = 0; k < division_size; k++) {
            int t = id(season["divisions"][d]["teams"][k].get<std::string>());
            lg.members[d][k] = t;
            lg.division[t] = d;
        }
    }
    for (const auto& week : season["schedule"]) {
        auto& games = lg.schedule.emplace_back();
        for (const auto& g : week) games.push_back({id(g[0].get<std::string>()), id(g[1].get<std::string>())});
    }
    lg.seed = 1234;
    return lg;
}

// Every regular-season game decided by `pick(away, home)` (true = away wins).
template <typename Pick>
static Results play(const League& lg, Pick pick) {
    Results r;
    for (size_t w = 0; w < lg.schedule.size(); w++) {
        for (size_t g = 0; g < lg.schedule[w].size(); g++) {
            bool away_wins = pick(lg.schedule[w][g].away, lg.schedule[w][g].home);
            r.score[w][g] = away_wins ? Score{24, 17} : Score{10, 20};
        }
    }
    return r;
}

static void test_structure(const League& lg) {
    std::map<int, int> games;
    for (const auto& w : lg.schedule) for (const Game& g : w) { games[g.away]++; games[g.home]++; }
    CHECK(lg.schedule.size() == 18);
    for (int t = 0; t < teams; t++) CHECK(games[t] == 17);
}

static void test_random_seasons(const League& lg) {
    std::mt19937 rng(7);
    for (int run = 0; run < 200; run++) {
        Results r = play(lg, [&](int, int) { return rng() & 1; });
        for (int c = 0; c < conferences; c++) {
            auto s = conference_seeds(lg, r, c);
            std::set<int> uniq(s.begin(), s.end());
            CHECK(uniq.size() == 7);
            for (int t : s) CHECK(lg.conference_of(t) == c);
            // Seeds 1-4 are the division winners, one per division.
            std::set<int> divs;
            for (int i = 0; i < 4; i++) {
                divs.insert(lg.division[s[i]]);
                CHECK(division_standings(lg, r, lg.division[s[i]])[0].team == s[i]);
            }
            CHECK(divs.size() == 4);
            // Winners are ordered by record; wild cards have no worse record than any non-playoff team.
            for (int i = 0; i + 1 < 4; i++) {
                auto a = division_standings(lg, r, lg.division[s[i]])[0].all.pct();
                auto b = division_standings(lg, r, lg.division[s[i + 1]])[0].all.pct();
                CHECK(a + 1e-9 >= b);
            }
            double worst_wc = 1;
            for (int i = 4; i < 7; i++) {
                for (const auto& line : division_standings(lg, r, lg.division[s[i]])) {
                    if (line.team == s[i]) worst_wc = std::min(worst_wc, line.all.pct());
                }
            }
            for (int d = c * 4; d < c * 4 + 4; d++) {
                for (const auto& line : division_standings(lg, r, d)) {
                    if (std::find(s.begin(), s.end(), line.team) == s.end()) CHECK(line.all.pct() <= worst_wc + 1e-9);
                }
            }
        }
        // Standings are sorted by record and stable when asked twice.
        for (int d = 0; d < divisions; d++) {
            auto a = division_standings(lg, r, d), b = division_standings(lg, r, d);
            for (int i = 0; i < 4; i++) CHECK(a[i].team == b[i].team);
            for (int i = 0; i + 1 < 4; i++) CHECK(a[i].all.pct() + 1e-9 >= a[i + 1].all.pct());
            int wins = 0, losses = 0;
            for (const auto& l : a) { wins += l.all.w; losses += l.all.l; CHECK(l.all.games() == 17); }
            (void)wins; (void)losses;
        }
    }
}

// Head-to-head decides a two-club division tie: across random seasons, every division with
// exactly two clubs level on record, where one has the better record against the other, must list
// that one first.
static void test_head_to_head(const League& lg) {
    std::mt19937 rng(11);
    int cases = 0;
    for (int run = 0; run < 3000; run++) {
        Results r = play(lg, [&](int, int) { return rng() & 1; });
        for (int d = 0; d < divisions; d++) {
            auto st = division_standings(lg, r, d);
            for (int i = 0; i < 4; i++) {
                for (int j = i + 1; j < 4; j++) {
                    int level = 0;
                    for (int k = 0; k < 4; k++) level += st[k].all.pct() == st[i].all.pct();
                    if (st[i].all.pct() != st[j].all.pct() || level != 2) continue;
                    int a = st[i].team, b = st[j].team, a_wins = 0, b_wins = 0;
                    for (size_t w = 0; w < lg.schedule.size(); w++) {
                        for (size_t g = 0; g < lg.schedule[w].size(); g++) {
                            const Game& gm = lg.schedule[w][g];
                            if (!((gm.away == a && gm.home == b) || (gm.away == b && gm.home == a))) continue;
                            (winner(gm, r.score[w][g]) == a ? a_wins : b_wins)++;
                        }
                    }
                    if (a_wins != b_wins) {
                        CHECK(a_wins > b_wins);  // the one listed first won the season series
                        cases++;
                    }
                }
            }
        }
    }
    CHECK(cases > 100);
    printf("head-to-head: %d two-club ties checked\n", cases);
}

static void test_bracket(const League& lg) {
    Results r = play(lg, [](int away, int home) { return away < home; });
    std::array<std::array<int, seeds>, conferences> s{};
    for (int c = 0; c < 2; c++) {
        auto v = conference_seeds(lg, r, c);
        std::copy(v.begin(), v.end(), s[c].begin());
    }
    auto wc = playoff_games(s, r, wildcard);
    CHECK(wc.size() == 6);
    CHECK(wc[0].away == s[0][6] && wc[0].home == s[0][1]);
    CHECK(wc[3].away == s[1][6] && wc[3].home == s[1][1]);
    CHECK(playoff_games(s, r, divisional).empty());  // wild card round not played yet
    // Home teams win every playoff game: seeds 2-4 advance, so 1 hosts 4 and 2 hosts 3.
    for (size_t g = 0; g < wc.size(); g++) r.score[wildcard][g] = {10, 20};
    auto dv = playoff_games(s, r, divisional);
    CHECK(dv.size() == 4);
    CHECK(dv[0].home == s[0][0] && dv[0].away == s[0][3]);
    CHECK(dv[1].home == s[0][1] && dv[1].away == s[0][2]);
    // Upsets: 7 seeds win, so in the divisional round 1 hosts 7.
    for (size_t g = 0; g < wc.size(); g++) r.score[wildcard][g] = (g % 3 == 0) ? Score{30, 3} : Score{10, 20};
    dv = playoff_games(s, r, divisional);
    CHECK(dv[0].home == s[0][0] && dv[0].away == s[0][6]);
    for (size_t g = 0; g < dv.size(); g++) r.score[divisional][g] = {10, 20};
    auto cf = playoff_games(s, r, conference);
    CHECK(cf.size() == 2);
    CHECK(cf[0].home == s[0][0]);
    for (size_t g = 0; g < cf.size(); g++) r.score[conference][g] = {10, 20};
    auto sb = playoff_games(s, r, super_bowl);
    CHECK(sb.size() == 1 && sb[0].away == s[0][0] && sb[0].home == s[1][0]);
}

int main(int argc, char** argv) {
    League lg = load_league(argc > 1 ? argv[1] : "packs/blitz2026/pack.json");
    test_structure(lg);
    test_random_seasons(lg);
    test_head_to_head(lg);
    test_bracket(lg);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("adv_league: all checks passed\n");
    return 0;
}
