// Advanced mode's league model: standings, NFL tiebreakers, seeds and the playoff bracket.
//
// Tiebreakers follow the NFL's published procedures:
//   division: head-to-head; division record; common games; conference record; strength of
//     victory; strength of schedule; combined rank in points scored and allowed (conference, then
//     league); net points in common games; net points in all games; coin toss.
//   wild card (and seeding the division winners): one club per division first (the best of each
//     division's tied clubs); then head-to-head (two clubs if they met; three or more only a
//     sweep); conference record; common games (minimum four); strength of victory; strength of
//     schedule; combined points ranks; net points in conference games; net points in all games;
//     coin toss.
// When a step settles one club the order continues with the rest from step 1; when it eliminates
// some clubs but leaves several tied, the remaining clubs restart at step 1. Net touchdowns (a
// late NFL step) isn't recorded by the game and is skipped. The coin toss is a hash of the season
// seed, so it is the same every time it is asked.
#include "adv_league.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace adv {
    namespace {
        constexpr double eps = 1e-9;

        struct Played {
            int opp;
            int pf, pa;
            bool home;
        };

        enum class Mode { division, wildcard };

        struct Table {
            const League& lg;
            std::array<std::vector<Played>, teams> games;

            Table(const League& league, const Results& r) : lg(league) {
                for (size_t w = 0; w < lg.schedule.size() && w < (size_t)regular_weeks; w++) {
                    for (size_t g = 0; g < lg.schedule[w].size() && g < (size_t)max_games; g++) {
                        const Game& gm = lg.schedule[w][g];
                        const Score& s = r.score[w][g];
                        if (!s.played() || gm.away < 0 || gm.home < 0) continue;
                        games[gm.away].push_back({gm.home, s.away, s.home, false});
                        games[gm.home].push_back({gm.away, s.home, s.away, true});
                    }
                }
            }

            template <typename Pred>
            Record record(int t, Pred keep) const {
                Record rec;
                for (const Played& p : games[t]) {
                    if (!keep(p)) continue;
                    if (p.pf > p.pa) rec.w++;
                    else if (p.pf < p.pa) rec.l++;
                    else rec.t++;
                }
                return rec;
            }
            Record overall(int t) const { return record(t, [](const Played&) { return true; }); }

            std::set<int> opponents(int t) const {
                std::set<int> o;
                for (const Played& p : games[t]) o.insert(p.opp);
                return o;
            }

            // Combined W-L-T percentage of the given opponents (each game counted once).
            double combined(int t, bool wins_only) const {
                double w = 0, g = 0;
                for (const Played& p : games[t]) {
                    if (wins_only && p.pf <= p.pa) continue;
                    Record o = overall(p.opp);
                    w += o.w + 0.5 * o.t;
                    g += o.games();
                }
                return g > 0 ? w / g : 0.0;
            }

            int points_for(int t) const {
                int s = 0;
                for (const Played& p : games[t]) s += p.pf;
                return s;
            }
            int points_against(int t) const {
                int s = 0;
                for (const Played& p : games[t]) s += p.pa;
                return s;
            }

            // Rank (1 = best) in points scored plus rank in points allowed, among `pool`.
            int points_rank(int t, const std::vector<int>& pool) const {
                int pf = points_for(t), pa = points_against(t), rank_for = 1, rank_against = 1;
                for (int o : pool) {
                    if (points_for(o) > pf) rank_for++;
                    if (points_against(o) < pa) rank_against++;
                }
                return rank_for + rank_against;
            }

            double coin(int t) const {
                uint64_t x = lg.seed ^ (0x9E3779B97F4A7C15ull * (uint64_t)(t + 1));
                x ^= x >> 33;
                x *= 0xFF51AFD7ED558CCDull;
                x ^= x >> 33;
                return (double)(x >> 11);
            }

            // ---- tiebreaking ----

            std::map<int, std::vector<int>> division_order_cache;

            const std::vector<int>& division_order(int div) {
                auto it = division_order_cache.find(div);
                if (it == division_order_cache.end()) {
                    it = division_order_cache.emplace(div, order(lg.members[div], Mode::division)).first;
                }
                return it->second;
            }

            std::vector<int> order(std::vector<int> group, Mode mode) {
                std::vector<int> out;
                while (!group.empty()) {
                    double top = -1;
                    for (int t : group) top = std::max(top, overall(t).pct());
                    std::vector<int> tied;
                    for (int t : group) {
                        if (std::fabs(overall(t).pct() - top) < eps) tied.push_back(t);
                    }
                    int best = pick(tied, mode);
                    out.push_back(best);
                    group.erase(std::find(group.begin(), group.end(), best));
                }
                return out;
            }

            // Keep the clubs with the highest value; nullopt when the step doesn't apply.
            using Step = std::function<std::optional<std::vector<double>>(const std::vector<int>&)>;

            int pick(std::vector<int> tied, Mode mode) {
                if (tied.size() == 1) return tied[0];
                if (mode == Mode::wildcard) {
                    // Only the best-placed club of each division takes part.
                    std::vector<int> reduced;
                    for (int t : tied) {
                        const std::vector<int>& div = division_order(lg.division[t]);
                        bool better_in_group = false;
                        for (int o : div) {
                            if (o == t) break;
                            if (std::find(tied.begin(), tied.end(), o) != tied.end()) better_in_group = true;
                        }
                        if (!better_in_group) reduced.push_back(t);
                    }
                    tied = reduced;
                    if (tied.size() == 1) return tied[0];
                }
                for (const Step& step : steps(mode)) {
                    auto values = step(tied);
                    if (!values) continue;
                    double best = *std::max_element(values->begin(), values->end());
                    std::vector<int> kept;
                    for (size_t i = 0; i < tied.size(); i++) {
                        if (std::fabs((*values)[i] - best) < eps) kept.push_back(tied[i]);
                    }
                    if (kept.size() == 1) return kept[0];
                    if (kept.size() < tied.size()) return pick(kept, mode);  // restart at step 1
                }
                return tied[0];  // unreachable: the coin toss always separates
            }

            std::vector<Step> steps(Mode mode) {
                auto in = [](const std::vector<int>& g, int t) { return std::find(g.begin(), g.end(), t) != g.end(); };
                auto pct_of = [this](const std::vector<int>& g, auto keep) {
                    std::vector<double> v;
                    for (int t : g) v.push_back(record(t, [&](const Played& p) { return keep(t, p); }).pct());
                    return v;
                };
                Step head_to_head = [this, mode, in](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    if (mode == Mode::wildcard && g.size() > 2) {
                        // Sweep: one club beat each of the others (1), or lost to each of them (-1).
                        for (int t : g) {
                            bool beat_all = true, lost_all = true;
                            for (int o : g) {
                                if (o == t) continue;
                                Record vs = record(t, [&](const Played& p) { return p.opp == o; });
                                if (vs.games() == 0) { beat_all = lost_all = false; break; }
                                beat_all = beat_all && vs.w == vs.games();
                                lost_all = lost_all && vs.l == vs.games();
                            }
                            v.push_back(beat_all ? 1 : lost_all ? -1 : 0);
                        }
                        return v;
                    }
                    for (int t : g) {
                        Record vs = record(t, [&](const Played& p) { return p.opp != t && in(g, p.opp); });
                        if (vs.games() == 0) return std::nullopt;
                        v.push_back(vs.pct());
                    }
                    return v;
                };
                Step division_record = [this, pct_of](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    return pct_of(g, [this](int t, const Played& p) { return lg.division[p.opp] == lg.division[t]; });
                };
                Step conference_record = [this, pct_of](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    return pct_of(g, [this](int t, const Played& p) { return lg.conference_of(p.opp) == lg.conference_of(t); });
                };
                auto common = [this, in](const std::vector<int>& g) {
                    std::set<int> c = opponents(g[0]);
                    for (int t : g) {
                        std::set<int> o = opponents(t), keep;
                        for (int x : c) if (o.count(x) && !in(g, x)) keep.insert(x);
                        c = keep;
                    }
                    return c;
                };
                Step common_games = [this, mode, common](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::set<int> c = common(g);
                    std::vector<double> v;
                    for (int t : g) {
                        Record rec = record(t, [&](const Played& p) { return c.count(p.opp) > 0; });
                        if (rec.games() < (mode == Mode::wildcard ? 4 : 1)) return std::nullopt;
                        v.push_back(rec.pct());
                    }
                    return v;
                };
                Step victory = [this](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    for (int t : g) v.push_back(combined(t, true));
                    return v;
                };
                Step schedule = [this](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    for (int t : g) v.push_back(combined(t, false));
                    return v;
                };
                Step conference_points = [this](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    for (int t : g) {
                        std::vector<int> pool;
                        for (int o = 0; o < lg.team_count; o++) if (lg.conference_of(o) == lg.conference_of(t)) pool.push_back(o);
                        v.push_back(-points_rank(t, pool));
                    }
                    return v;
                };
                Step league_points = [this](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<int> pool;
                    for (int o = 0; o < lg.team_count; o++) pool.push_back(o);
                    std::vector<double> v;
                    for (int t : g) v.push_back(-points_rank(t, pool));
                    return v;
                };
                auto net = [this](int t, auto keep) {
                    int n = 0;
                    for (const Played& p : games[t]) if (keep(p)) n += p.pf - p.pa;
                    return n;
                };
                Step common_net = [net, common](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::set<int> c = common(g);
                    std::vector<double> v;
                    for (int t : g) v.push_back(net(t, [&](const Played& p) { return c.count(p.opp) > 0; }));
                    return v;
                };
                Step conference_net = [this, net](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    for (int t : g) v.push_back(net(t, [&](const Played& p) { return lg.conference_of(p.opp) == lg.conference_of(t); }));
                    return v;
                };
                Step all_net = [net](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    for (int t : g) v.push_back(net(t, [](const Played&) { return true; }));
                    return v;
                };
                Step coin_toss = [this](const std::vector<int>& g) -> std::optional<std::vector<double>> {
                    std::vector<double> v;
                    for (int t : g) v.push_back(coin(t));
                    return v;
                };
                if (mode == Mode::division) {
                    return {head_to_head, division_record, common_games, conference_record, victory, schedule,
                            conference_points, league_points, common_net, all_net, coin_toss};
                }
                return {head_to_head, conference_record, common_games, victory, schedule,
                        conference_points, league_points, conference_net, all_net, coin_toss};
            }
        };
    }

    std::vector<TeamLine> division_standings(const League& lg, const Results& r, int div) {
        Table tb(lg, r);
        std::vector<TeamLine> out;
        for (int t : tb.division_order(div)) {
            TeamLine line;
            line.team = t;
            line.all = tb.overall(t);
            line.home = tb.record(t, [](const Played& p) { return p.home; });
            line.away = tb.record(t, [](const Played& p) { return !p.home; });
            line.division = tb.record(t, [&](const Played& p) { return lg.division[p.opp] == lg.division[t]; });
            line.conf = tb.record(t, [&](const Played& p) { return lg.conference_of(p.opp) == lg.conference_of(t); });
            line.points_for = tb.points_for(t);
            line.points_against = tb.points_against(t);
            out.push_back(line);
        }
        return out;
    }

    std::vector<int> conference_seeds(const League& lg, const Results& r, int conf, int count) {
        Table tb(lg, r);
        std::vector<int> winners, others;
        for (size_t d = 0; d < lg.members.size(); d++) {
            if (lg.conference[d] != conf || lg.members[d].empty()) continue;
            const std::vector<int>& order = tb.division_order((int)d);
            winners.push_back(order[0]);
            others.insert(others.end(), order.begin() + 1, order.end());
        }
        std::vector<int> out = tb.order(winners, Mode::wildcard);
        std::vector<int> wc = tb.order(others, Mode::wildcard);
        for (size_t i = 0; i < wc.size() && (int)out.size() < count; i++) out.push_back(wc[i]);
        out.resize(count, -1);
        return out;
    }

    int winner(const Game& g, const Score& s) {
        return s.home > s.away ? g.home : g.away;
    }

    std::vector<Game> playoff_games(const std::array<std::array<int, seeds>, conferences>& seeds_of, const Results& r, int week) {
        auto seed_of = [&](int c, int t) {
            for (int i = 0; i < seeds; i++) if (seeds_of[c][i] == t) return i;
            return seeds;
        };
        auto host = [&](int c, int a, int b) {  // higher seed (lower number) at home
            return seed_of(c, a) < seed_of(c, b) ? Game{b, a} : Game{a, b};
        };
        // Winners of an earlier round, per conference (for the Super Bowl, conference 0 then 1).
        auto winners_of = [&](int round, int c) {
            std::vector<Game> prev = playoff_games(seeds_of, r, round);
            std::vector<int> out;
            for (size_t g = 0; g < prev.size(); g++) {
                const Score& s = r.score[round][g];
                int win = s.played() ? winner(prev[g], s) : -1;
                bool in_conf = seed_of(c, prev[g].away) < seeds || seed_of(c, prev[g].home) < seeds;
                if (in_conf && win >= 0) out.push_back(win);
            }
            return out;
        };
        std::vector<Game> games;
        if (week == wildcard) {
            for (int c = 0; c < conferences; c++) {
                const auto& s = seeds_of[c];
                games.push_back({s[6], s[1]});
                games.push_back({s[5], s[2]});
                games.push_back({s[4], s[3]});
            }
        }
        else if (week == divisional) {
            for (int c = 0; c < conferences; c++) {
                std::vector<int> left = winners_of(wildcard, c);
                if (left.size() != 3) continue;
                std::sort(left.begin(), left.end(), [&](int a, int b) { return seed_of(c, a) < seed_of(c, b); });
                games.push_back(host(c, seeds_of[c][0], left[2]));
                games.push_back(host(c, left[0], left[1]));
            }
        }
        else if (week == conference) {
            for (int c = 0; c < conferences; c++) {
                std::vector<int> left = winners_of(divisional, c);
                if (left.size() != 2) continue;
                games.push_back(host(c, left[0], left[1]));
            }
        }
        else if (week == super_bowl) {
            std::vector<int> afc = winners_of(conference, 0), nfc = winners_of(conference, 1);
            if (afc.size() == 1 && nfc.size() == 1) games.push_back({afc[0], nfc[0]});
        }
        return games;
    }
}
