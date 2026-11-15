// The cart season's standings and seeds against the app's (src/game/adv_league.cpp) on the same results
// (build/cart/results.bin from tools/cart/host_test.c). Coin tosses differ by design (another hash).
#include <cstdio>
#include <vector>
#include "../../src/game/adv_league.h"
#include "../../src/cart/cart_season.h"
int main() {
    FILE* f = fopen("build/cart/results.bin", "rb");
    std::vector<uint8_t> res(22 * 16 * 2);
    CartLeague lg;
    fread(res.data(), 1, res.size(), f);
    fread(&lg, 1, sizeof(lg), f);
    adv::League L;
    L.members.assign(8, std::vector<int>(4));
    for (int d = 0; d < 8; d++) {
        L.conference.push_back(d / 4);
        for (int k = 0; k < 4; k++) { L.members[d][k] = lg.members[d][k]; L.division[lg.members[d][k]] = d; }
    }
    adv::Results r;
    for (int w = 0; w < 18; w++) {
        auto& g = L.schedule.emplace_back();
        for (int k = 0; k < lg.games[w]; k++) {
            g.push_back({lg.schedule[w][k][0], lg.schedule[w][k][1]});
            r.score[w][k].home = res[(w * 16 + k) * 2];
            r.score[w][k].away = res[(w * 16 + k) * 2 + 1];
        }
    }
    for (int d = 0; d < 8; d++) {
        printf("div %d:", d);
        for (auto& l : adv::division_standings(L, r, d)) printf(" %2d %d-%d", l.team, l.all.w, l.all.l);
        printf("\n");
    }
    for (int c = 0; c < 2; c++) {
        printf("seeds %d:", c);
        for (int t : adv::conference_seeds(L, r, c)) printf(" %d", t);
        printf("\n");
    }
}
