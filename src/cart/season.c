/* Advanced season on the cart: the app's Advanced mode (src/game/adv_engine.cpp, adv_league.cpp, adv_season.cpp)
 * for the N64, in C, on any N64 (no Expansion Pak). Built by tools/cart/build.sh (LLVM, MIPS II, o32) into
 * src/game/cart_season.inc; Export ROM appends it to the 32-team cart, and the boot loader (tools/rom_picker/cart32.asm)
 * copies it to 0x800E4700, the default heap's old top (the export cuts the heap to 0x45C00), and calls cart_init.
 *
 * The app hooks the game through its recompiler; here the same changes are instruction patches applied in RAM:
 * main's once at boot (cart_init), ovl_b's each time the game loads it (cart_ovl_b_loaded, called from the two
 * loaders). Four ovl_b functions are replaced outright (week driver, week builder, CPU skill, division standings), a
 * few spots jump to small assembly handlers (stubs.S), and the rest are limits and addresses (8 divisions, 4 rows,
 * 16 games, 18 + 4 weeks, team 31). Each patch checks the instruction it replaces first.
 *
 * The season record (0x802EF6F0, 0x774 bytes, which the game saves and loads itself) holds everything, as in the
 * app: +0 user team, +1 week (0-17 regular, 18-21 playoffs, 22 over), +4 stats f32[31][7] (team 31's at +0x704),
 * +0x368 season highs, +0x37F results [22][16] {home, away} (0xFF unplayed), +0x758 seeds [2][7]; and, the cart's
 * own, "BZ27" and the season's coin-toss seed at +0x740. */
#include "cart_season.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;

#define LEAGUE ((const CartLeague*)CS_LEAGUE)

/* ---- the game ---- */
#define REC 0x802EF6F0u
#define REC_WEEK (REC + 1)
#define REC_STATS (REC + 4)
#define REC_STATS31 (REC + 0x704)
#define REC_HIGHS (REC + 0x368)
#define REC_HIGH_TEAMS (REC + 0x376)
#define REC_RESULTS (REC + 0x37F)
#define REC_TAG (REC + 0x740)
#define REC_SEED (REC + 0x744)
#define REC_SEEDS (REC + 0x758)
#define SIM_SLOT 0x802EA1D0u
#define ABORT_FLAG 0x8028BFC4u
#define PAD_STORE 0x8028BFD4u
#define SIM_WEEK 0x8028BFD8u
#define CPU_SKILL 0x8033B988u
#define SEED_STORE 0x8032E91Cu
#define SEASON_TAG 0x425A3237u /* "BZ27" */

#define WILDCARD 18
#define DIVISIONAL 19
#define CONFERENCE 20
#define SUPER_BOWL 21
#define SEASON_OVER 22

#ifdef CART_HOST_TEST  /* tools/cart/host_test.c: the logic on the host, over an 8 MB array */
extern u8 host_ram[];
u8 rd8(u32 a);
void wr8(u32 a, u8 v);
s32 rd32(u32 a);
void wr32(u32 a, s32 v);
float rdf(u32 a);
void wrf(u32 a, float v);
#undef LEAGUE
#define LEAGUE ((const CartLeague*)(host_ram + (CS_LEAGUE - 0x80000000u)))
#else
static inline u8 rd8(u32 a) { return *(volatile u8*)a; }
static inline void wr8(u32 a, u8 v) { *(volatile u8*)a = v; }
static inline s32 rd32(u32 a) { return *(volatile s32*)a; }
static inline void wr32(u32 a, s32 v) { *(volatile s32*)a = v; }
static inline float rdf(u32 a) { return *(volatile float*)a; }
static inline void wrf(u32 a, float v) { *(volatile float*)a = v; }
#endif

typedef void (*fn0)(void);
typedef void (*fn1)(s32);
typedef s32 (*ifn0)(void);
typedef s32 (*ifn2)(s32, s32);
typedef s32 (*ifn4)(s32, s32, s32, s32);
typedef void (*fn5)(s32, s32, s32, s32, s32);
typedef void (*fn6)(s32, s32, s32, s32, s32, s32);
#define GAME(type, addr) ((type)(addr))

/* libultra */
#define osWritebackDCache GAME(void (*)(void*, s32), 0x8001B8B0)
#define osInvalICache GAME(void (*)(void*, s32), 0x8001B830)
#ifdef CART_HOST_TEST
u32 count_register(void);
#else
static u32 count_register(void) {
    u32 c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
#endif

/* ---- results ---- */
static u8 score_home(int w, int g) { return rd8(REC_RESULTS + (u32)(w * CS_MAX_GAMES + g) * 2); }
static u8 score_away(int w, int g) { return rd8(REC_RESULTS + (u32)(w * CS_MAX_GAMES + g) * 2 + 1); }
static int played(int w, int g) { return score_home(w, g) != 0xFF && score_away(w, g) != 0xFF; }
static u32 stats_addr(int t) { return t == 31 ? REC_STATS31 : REC_STATS + 28u * (u32)t; }
static u32 season_seed(void) { return (u32)rd32(REC_SEED); }

typedef struct { s8 away, home; } Game;

static int division_of[CS_TEAMS];
static int conference_of_team(int t) { return division_of[t] / 4; }

static void league_index(void) {
    for (int d = 0; d < CS_DIVISIONS; d++)
        for (int k = 0; k < CS_DIVISION_SIZE; k++) division_of[LEAGUE->members[d][k] & 31] = d;
}

/* ---- standings and NFL tiebreakers (adv_league.cpp) ---- */
typedef struct { int w, l, t; } Record;
static int rec_games(Record r) { return r.w + r.l + r.t; }
static double rec_pct(Record r) { int g = rec_games(r); return g ? (r.w + 0.5 * r.t) / g : 0.0; }

typedef struct { s8 opp; u8 pf, pa, home; } Played;
static Played played_of[CS_TEAMS][CS_REGULAR_WEEKS];
static int played_count[CS_TEAMS];

static void build_table(void) {
    for (int t = 0; t < CS_TEAMS; t++) played_count[t] = 0;
    for (int w = 0; w < CS_REGULAR_WEEKS; w++) {
        for (int g = 0; g < LEAGUE->games[w] && g < CS_MAX_GAMES; g++) {
            if (!played(w, g)) continue;
            int a = LEAGUE->schedule[w][g][0], h = LEAGUE->schedule[w][g][1];
            if (a >= CS_TEAMS || h >= CS_TEAMS) continue;
            u8 sh = score_home(w, g), sa = score_away(w, g);
            if (played_count[a] < CS_REGULAR_WEEKS) played_of[a][played_count[a]++] = (Played){(s8)h, sa, sh, 0};
            if (played_count[h] < CS_REGULAR_WEEKS) played_of[h][played_count[h]++] = (Played){(s8)a, sh, sa, 1};
        }
    }
}

enum Keep { K_ALL, K_HOME, K_AWAY, K_DIVISION, K_CONFERENCE, K_MASK, K_VS };
static Record record_of(int t, int keep, u32 mask) {
    Record r = {0, 0, 0};
    for (int i = 0; i < played_count[t]; i++) {
        const Played* p = &played_of[t][i];
        int ok = keep == K_ALL || (keep == K_HOME && p->home) || (keep == K_AWAY && !p->home) ||
                 (keep == K_DIVISION && division_of[(int)p->opp] == division_of[t]) ||
                 (keep == K_CONFERENCE && conference_of_team(p->opp) == conference_of_team(t)) ||
                 ((keep == K_MASK || keep == K_VS) && (mask >> p->opp) & 1);
        if (!ok) continue;
        if (p->pf > p->pa) r.w++;
        else if (p->pf < p->pa) r.l++;
        else r.t++;
    }
    return r;
}
static Record overall(int t) { return record_of(t, K_ALL, 0); }
static u32 opponents(int t) {
    u32 m = 0;
    for (int i = 0; i < played_count[t]; i++) m |= 1u << played_of[t][i].opp;
    return m;
}
static double combined(int t, int wins_only) {
    double w = 0, g = 0;
    for (int i = 0; i < played_count[t]; i++) {
        const Played* p = &played_of[t][i];
        if (wins_only && p->pf <= p->pa) continue;
        Record o = overall(p->opp);
        w += o.w + 0.5 * o.t;
        g += rec_games(o);
    }
    return g > 0 ? w / g : 0.0;
}
static int points_for(int t) { int s = 0; for (int i = 0; i < played_count[t]; i++) s += played_of[t][i].pf; return s; }
static int points_against(int t) { int s = 0; for (int i = 0; i < played_count[t]; i++) s += played_of[t][i].pa; return s; }
static int points_rank(int t, u32 pool) {
    int pf = points_for(t), pa = points_against(t), rf = 1, ra = 1;
    for (int o = 0; o < CS_TEAMS; o++) {
        if (!((pool >> o) & 1)) continue;
        if (points_for(o) > pf) rf++;
        if (points_against(o) < pa) ra++;
    }
    return rf + ra;
}
static int net(int t, int keep, u32 mask) {
    int n = 0;
    for (int i = 0; i < played_count[t]; i++) {
        const Played* p = &played_of[t][i];
        int ok = keep == K_ALL || (keep == K_CONFERENCE && conference_of_team(p->opp) == conference_of_team(t)) ||
                 (keep == K_MASK && (mask >> p->opp) & 1);
        if (ok) n += p->pf - p->pa;
    }
    return n;
}
static double coin(int t) {
    u32 x = season_seed() ^ (0x9E3779B9u * (u32)(t + 1));
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return (double)x;
}

#define EPS 1e-9
static int same(double a, double b) { double d = a - b; return d < EPS && d > -EPS; }  /* |a - b| < EPS */
enum Mode { M_DIVISION, M_WILDCARD };
enum Step { S_H2H, S_DIVISION, S_CONFERENCE, S_COMMON, S_VICTORY, S_SCHEDULE, S_CONF_POINTS, S_LEAGUE_POINTS,
            S_COMMON_NET, S_CONF_NET, S_ALL_NET, S_COIN, S_END };
static const u8 division_steps[] = {S_H2H, S_DIVISION, S_COMMON, S_CONFERENCE, S_VICTORY, S_SCHEDULE,
                                    S_CONF_POINTS, S_LEAGUE_POINTS, S_COMMON_NET, S_ALL_NET, S_COIN, S_END};
static const u8 wildcard_steps[] = {S_H2H, S_CONFERENCE, S_COMMON, S_VICTORY, S_SCHEDULE, S_CONF_POINTS,
                                    S_LEAGUE_POINTS, S_CONF_NET, S_ALL_NET, S_COIN, S_END};

static int division_order_of[CS_DIVISIONS][CS_DIVISION_SIZE];
static u8 division_order_done[CS_DIVISIONS];

static int order(const int* group, int n, int mode, int* out);
static int pick(const int* tied_in, int n, int mode);

static const int* division_order(int div) {
    if (!division_order_done[div]) {
        int g[CS_DIVISION_SIZE];
        for (int k = 0; k < CS_DIVISION_SIZE; k++) g[k] = LEAGUE->members[div][k];
        order(g, CS_DIVISION_SIZE, M_DIVISION, division_order_of[div]);
        division_order_done[div] = 1;
    }
    return division_order_of[div];
}

static u32 common_of(const int* g, int n) {
    u32 inside = 0;
    for (int i = 0; i < n; i++) inside |= 1u << g[i];
    u32 c = opponents(g[0]);
    for (int i = 0; i < n; i++) c &= opponents(g[i]);
    return c & ~inside;
}

/* Values of a step for the tied clubs (higher is better); 0 when the step doesn't apply. */
static int step_values(int step, int mode, const int* g, int n, double* v) {
    u32 inside = 0;
    for (int i = 0; i < n; i++) inside |= 1u << g[i];
    switch (step) {
    case S_H2H:
        if (mode == M_WILDCARD && n > 2) {
            for (int i = 0; i < n; i++) {
                int beat_all = 1, lost_all = 1;
                for (int j = 0; j < n; j++) {
                    if (j == i) continue;
                    Record vs = record_of(g[i], K_VS, 1u << g[j]);
                    if (rec_games(vs) == 0) { beat_all = lost_all = 0; break; }
                    beat_all = beat_all && vs.w == rec_games(vs);
                    lost_all = lost_all && vs.l == rec_games(vs);
                }
                v[i] = beat_all ? 1 : lost_all ? -1 : 0;
            }
            return 1;
        }
        for (int i = 0; i < n; i++) {
            Record vs = record_of(g[i], K_VS, inside & ~(1u << g[i]));
            if (rec_games(vs) == 0) return 0;
            v[i] = rec_pct(vs);
        }
        return 1;
    case S_DIVISION:
        for (int i = 0; i < n; i++) v[i] = rec_pct(record_of(g[i], K_DIVISION, 0));
        return 1;
    case S_CONFERENCE:
        for (int i = 0; i < n; i++) v[i] = rec_pct(record_of(g[i], K_CONFERENCE, 0));
        return 1;
    case S_COMMON: {
        u32 c = common_of(g, n);
        for (int i = 0; i < n; i++) {
            Record r = record_of(g[i], K_MASK, c);
            if (rec_games(r) < (mode == M_WILDCARD ? 4 : 1)) return 0;
            v[i] = rec_pct(r);
        }
        return 1;
    }
    case S_VICTORY:
        for (int i = 0; i < n; i++) v[i] = combined(g[i], 1);
        return 1;
    case S_SCHEDULE:
        for (int i = 0; i < n; i++) v[i] = combined(g[i], 0);
        return 1;
    case S_CONF_POINTS:
        for (int i = 0; i < n; i++) {
            u32 pool = 0;
            for (int o = 0; o < CS_TEAMS; o++) if (conference_of_team(o) == conference_of_team(g[i])) pool |= 1u << o;
            v[i] = -points_rank(g[i], pool);
        }
        return 1;
    case S_LEAGUE_POINTS:
        for (int i = 0; i < n; i++) v[i] = -points_rank(g[i], 0xFFFFFFFFu);
        return 1;
    case S_COMMON_NET: {
        u32 c = common_of(g, n);
        for (int i = 0; i < n; i++) v[i] = net(g[i], K_MASK, c);
        return 1;
    }
    case S_CONF_NET:
        for (int i = 0; i < n; i++) v[i] = net(g[i], K_CONFERENCE, 0);
        return 1;
    case S_ALL_NET:
        for (int i = 0; i < n; i++) v[i] = net(g[i], K_ALL, 0);
        return 1;
    default:
        for (int i = 0; i < n; i++) v[i] = coin(g[i]);
        return 1;
    }
}

/* pick() restarts on a smaller group and division_order() may order another division meanwhile: scratch space per
 * depth, kept off the game thread's stack. A group is at most 12 clubs (a conference's non-winners; a division is 4,
 * the winners 4), so a row holds 16 (12 KB less than 32, which the 4 MB cart needs). */
#define PICK_DEPTH 48
#define PICK_WIDTH 16
static int pick_depth;
static int pick_tied[PICK_DEPTH][PICK_WIDTH], pick_kept[PICK_DEPTH][PICK_WIDTH];
static double pick_values[PICK_DEPTH][PICK_WIDTH];

static int pick_at(int* tied, int n, int mode, int d) {
    if (n == 1) return tied[0];
    if (mode == M_WILDCARD) {
        /* only the best-placed club of each division takes part */
        int m = 0;
        for (int i = 0; i < n; i++) {
            const int* div = division_order(division_of[tied[i]]);
            int better = 0;
            for (int k = 0; k < CS_DIVISION_SIZE && div[k] != tied[i]; k++)
                for (int j = 0; j < n; j++) if (tied[j] == div[k]) better = 1;
            if (!better) tied[m++] = tied[i];
        }
        n = m;
        if (n == 1) return tied[0];
    }
    const u8* steps = mode == M_DIVISION ? division_steps : wildcard_steps;
    double* v = pick_values[d];
    int* kept = pick_kept[d];
    for (int s = 0; steps[s] != S_END; s++) {
        if (!step_values(steps[s], mode, tied, n, v)) continue;
        double best = v[0];
        for (int i = 1; i < n; i++) if (v[i] > best) best = v[i];
        int k = 0;
        for (int i = 0; i < n; i++) if (same(v[i], best)) kept[k++] = tied[i];
        if (k == 1) return kept[0];
        if (k < n) return pick(kept, k, mode); /* restart at step 1 */
    }
    return tied[0];
}

static int pick(const int* tied_in, int n, int mode) {
    if (pick_depth >= PICK_DEPTH || n > PICK_WIDTH) return tied_in[0];
    int d = pick_depth++;
    int* tied = pick_tied[d];
    for (int i = 0; i < n; i++) tied[i] = tied_in[i];
    int best = pick_at(tied, n, mode, d);
    pick_depth--;
    return best;
}

static int order(const int* group_in, int n, int mode, int* out) {
    int group[CS_TEAMS];
    for (int i = 0; i < n; i++) group[i] = group_in[i];
    int count = 0;
    while (n > 0) {
        double top = -1;
        for (int i = 0; i < n; i++) { double p = rec_pct(overall(group[i])); if (p > top) top = p; }
        int tied[CS_TEAMS], k = 0;
        for (int i = 0; i < n; i++) {
            double p = rec_pct(overall(group[i]));
            if (same(p, top)) tied[k++] = group[i];
        }
        int best = pick(tied, k, mode);
        out[count++] = best;
        int m = 0;
        for (int i = 0; i < n; i++) if (group[i] != best) group[m++] = group[i];
        n = m;
    }
    return count;
}

static void new_table(void) {
    league_index();
    build_table();
    for (int d = 0; d < CS_DIVISIONS; d++) division_order_done[d] = 0;
}

static void conference_seeds(int conf, int* out) {
    int winners[4], others[12], nw = 0, no = 0;
    for (int d = conf * 4; d < conf * 4 + 4; d++) {
        const int* o = division_order(d);
        winners[nw++] = o[0];
        for (int k = 1; k < CS_DIVISION_SIZE; k++) others[no++] = o[k];
    }
    int w[4], wc[12];
    order(winners, nw, M_WILDCARD, w);
    order(others, no, M_WILDCARD, wc);
    for (int i = 0; i < 4; i++) out[i] = w[i];
    for (int i = 0; i < 3; i++) out[4 + i] = wc[i];
}

static int winner(Game g, u8 home, u8 away) { return home > away ? g.home : g.away; }

/* ---- the playoff bracket ---- */
static int seed_at(int c, int i) { return rd8(REC_SEEDS + (u32)(c * CS_SEEDS + i)); }
static int seeded(void) {
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < CS_SEEDS; i++) if (seed_at(c, i) >= CS_TEAMS) return 0;
    return 1;
}
static int seed_of(int c, int t) {
    for (int i = 0; i < CS_SEEDS; i++) if (seed_at(c, i) == t) return i;
    return CS_SEEDS;
}
static Game host(int c, int a, int b) { return seed_of(c, a) < seed_of(c, b) ? (Game){(s8)b, (s8)a} : (Game){(s8)a, (s8)b}; }

static int playoff_games(int week, Game* games);

static int winners_of(int round, int c, int* out) {
    Game prev[8];
    int n = playoff_games(round, prev), k = 0;
    for (int g = 0; g < n; g++) {
        int win = played(round, g) ? winner(prev[g], score_home(round, g), score_away(round, g)) : -1;
        int in_conf = seed_of(c, prev[g].away) < CS_SEEDS || seed_of(c, prev[g].home) < CS_SEEDS;
        if (in_conf && win >= 0) out[k++] = win;
    }
    return k;
}

static int playoff_games(int week, Game* games) {
    int n = 0;
    if (week == WILDCARD) {
        for (int c = 0; c < 2; c++) {
            games[n++] = (Game){(s8)seed_at(c, 6), (s8)seed_at(c, 1)};
            games[n++] = (Game){(s8)seed_at(c, 5), (s8)seed_at(c, 2)};
            games[n++] = (Game){(s8)seed_at(c, 4), (s8)seed_at(c, 3)};
        }
    }
    else if (week == DIVISIONAL) {
        for (int c = 0; c < 2; c++) {
            int left[8];
            if (winners_of(WILDCARD, c, left) != 3) continue;
            for (int i = 0; i < 3; i++)  /* by seed */
                for (int j = i + 1; j < 3; j++)
                    if (seed_of(c, left[j]) < seed_of(c, left[i])) { int x = left[i]; left[i] = left[j]; left[j] = x; }
            games[n++] = host(c, seed_at(c, 0), left[2]);
            games[n++] = host(c, left[0], left[1]);
        }
    }
    else if (week == CONFERENCE) {
        for (int c = 0; c < 2; c++) {
            int left[8];
            if (winners_of(DIVISIONAL, c, left) != 2) continue;
            games[n++] = host(c, left[0], left[1]);
        }
    }
    else if (week == SUPER_BOWL) {
        int afc[8], nfc[8];
        if (winners_of(CONFERENCE, 0, afc) == 1 && winners_of(CONFERENCE, 1, nfc) == 1) games[n++] = (Game){(s8)afc[0], (s8)nfc[0]};
    }
    return n;
}

/* The games of a week: the schedule, or the playoff round from the seeds and earlier results. */
static int games_of(int week, Game* games) {
    if (week < CS_REGULAR_WEEKS) {
        int n = LEAGUE->games[week];
        for (int g = 0; g < n; g++) games[g] = (Game){(s8)LEAGUE->schedule[week][g][0], (s8)LEAGUE->schedule[week][g][1]};
        return n;
    }
    return seeded() ? playoff_games(week, games) : 0;
}

/* ---- the game's season functions, replaced ---- */
static s32 cpu_skill_of(int t) { return rd32(LEAGUE->team_table + 0x64u * (u32)t + 0x0C); }

/* ovl_b 802FBAAC: every team's CPU skill; from week 2 four random teams get 13 - skill; in the playoffs skill ramps up a
 * point a week (capped at 11). Seeded from the season and the week. */
void cs_cpu_skill(void) {
    for (int t = 0; t < CS_TEAMS; t++) wr32(CPU_SKILL + 4u * (u32)t, cpu_skill_of(t));
    u32 week = rd8(REC_WEEK);
    if (week != 0) {
        u32 seed = season_seed() ^ (week * 0x9E3779B9u);
        wr32(SEED_STORE, (s32)seed);
        GAME(fn1, 0x80026B44)((s32)seed); /* srand */
        for (int i = 0; i < 4; i++) {
            u32 r = (u32)GAME(s32 (*)(s32), 0x8027B950)(CS_TEAMS); /* rand_mod */
            u32 a = CPU_SKILL + 4u * (r % CS_TEAMS);
            wr32(a, 13 - rd32(a));
        }
    }
    if (week >= WILDCARD && week < CS_TOTAL_WEEKS) {
        for (int t = 0; t < CS_TEAMS; t++) {
            s32 v = rd32(CPU_SKILL + 4u * (u32)t) - CS_REGULAR_WEEKS + 1 + (s32)week;
            wr32(CPU_SKILL + 4u * (u32)t, v < 12 ? v : 11);
        }
    }
}

static int quarter_length(void) {
    s32 bank = GAME(ifn0, 0x8025A26C)();
    return GAME(ifn2, 0x8025F098)(bank, 1) >> 16;
}

/* Play (the user's game) or simulate one game into sim slot 0; home is the game's team A. */
static void play(Game g, int qlen) {
    GAME(fn6, 0x802FD094)(g.home, g.away, (s32)SIM_SLOT, qlen, cpu_skill_of(g.home), cpu_skill_of(g.away));
}

static u8 clamp_score(s32 s) { return (u8)(s < 0 ? 0 : s > 254 ? 254 : s); }

/* Store sim slot 0 as the result of (week, game); in the regular season also the teams' running per-game stat
 * averages and the season highs, as the game does (ovl_b 802FE29C). */
static void record(int week, int game, Game g) {
    wr8(REC_RESULTS + (u32)(week * CS_MAX_GAMES + game) * 2, clamp_score(rd32(SIM_SLOT + 0x1C)));
    wr8(REC_RESULTS + (u32)(week * CS_MAX_GAMES + game) * 2 + 1, clamp_score(rd32(SIM_SLOT + 0x48 + 0x1C)));
    if (week >= CS_REGULAR_WEEKS) return;
    for (int side = 0; side < 2; side++) {
        int team = side == 0 ? g.home : g.away;
        u32 block = SIM_SLOT + 0x48u * (u32)side;
        int n = 0;
        for (int w = 0; w < CS_REGULAR_WEEKS; w++)
            for (int k = 0; k < LEAGUE->games[w]; k++)
                if ((LEAGUE->schedule[w][k][0] == team || LEAGUE->schedule[w][k][1] == team) && played(w, k)) n++;
        if (n == 0) continue;
        u32 st = stats_addr(team);
        float before = (float)(n - 1);
        for (int k = 0; k < 7; k++) {
            float sum = rdf(st + 4u * (u32)k) * before + (float)rd32(block + 0x2C + 4u * (u32)k);
            wrf(st + 4u * (u32)k, 0.0f < sum ? sum / (float)n : sum);
        }
        for (int k = 0; k < 7; k++) {
            s32 x = rd32(block + 0x2C + 4u * (u32)k);
            s32 best = (rd8(REC_HIGHS + 2u * (u32)k) << 8) | rd8(REC_HIGHS + 2u * (u32)k + 1);
            if (best < x) {
                wr8(REC_HIGHS + 2u * (u32)k, (u8)(x >> 8));
                wr8(REC_HIGHS + 2u * (u32)k + 1, (u8)x);
                wr8(REC_HIGH_TEAMS + (u32)k, (u8)team);
            }
        }
    }
}

static void seed_playoffs(void) {
    new_table();
    for (int c = 0; c < 2; c++) {
        int s[CS_SEEDS];
        conference_seeds(c, s);
        for (int i = 0; i < CS_SEEDS; i++) wr8(REC_SEEDS + (u32)(c * CS_SEEDS + i), (u8)s[i]);
    }
}

static int seed_of_user(void) {
    int user = rd8(REC);
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < CS_SEEDS; i++) if (seed_at(c, i) == user) return i + 1;
    return 0;
}

/* The Super Bowl winner's season rating (ovl_b 80300BA4). */
static void season_rating(int qlen) {
    int user = rd8(REC), losses = 0, margin = 0, allowed = 0;
    for (int w = 0; w < CS_TOTAL_WEEKS; w++) {
        Game games[CS_MAX_GAMES];
        int n = games_of(w, games);
        for (int g = 0; g < n; g++) {
            if (!played(w, g) || (games[g].home != user && games[g].away != user)) continue;
            int own = games[g].home == user ? score_home(w, g) : score_away(w, g);
            int opp = games[g].home == user ? score_away(w, g) : score_home(w, g);
            allowed = (int)((float)allowed + (float)opp / ((float)qlen * 0.5f));
            if (own < opp) losses++;
            else margin += own - opp;
        }
    }
    float attempts = rdf(stats_addr(user) + 12), completions = rdf(stats_addr(user) + 8);
    int pct = attempts == 0.0f ? 0 : (int)((completions / attempts) * 100.0f);
    s32 rating = GAME(ifn4, 0x80300FA0)(losses, margin / CS_TOTAL_WEEKS, allowed / CS_TOTAL_WEEKS, pct);
    GAME(fn1, 0x8027BF50)((s32)0x80339FD8);
    GAME(fn5, 0x8027BF6C)((s32)0x80339FD8, 0, 0xFF, 0x7F, 0x8000);
    GAME(fn1, rating < 900 ? 0x80300858 : 0x8030052C)(rating);
    GAME(fn0, 0x80005718)();
    GAME(fn1, 0x8027BCF4)((s32)0x80339FD8);
}

static void message(u32 fn, s32 pad) { GAME(fn1, fn)(pad); }

/* One week. Nested weeks are the rest of the playoffs, simulated after the user is out: no messages and no week++.
 * Returns 0 when the user's match was aborted. */
static int run_week(s32 pad, int nested) {
    int week = rd8(REC_WEEK);
    if (week >= SEASON_OVER) return 1;
    cs_cpu_skill();
    int qlen = quarter_length();
    int user = rd8(REC);
    int show = rd32(SIM_WEEK) == 0 && !nested;
    Game games[CS_MAX_GAMES];
    int n = games_of(week, games);
    int user_plays = 0;
    for (int i = 0; i < n; i++) user_plays = user_plays || games[i].home == user || games[i].away == user;

    if (week == SUPER_BOWL && user_plays && show) message(0x8030050C, pad);
    for (int i = 0; i < n; i++) {
        if (played(week, i)) continue; /* already recorded (a week resumed after an abort) */
        play(games[i], qlen);
        record(week, i, games[i]);
        if (rd32(ABORT_FLAG) != 0) return 0;
    }

    int finish_from = -1;
    if (week < CS_REGULAR_WEEKS) {
        if (!user_plays && show) message(0x803003AC, pad);
        if (week == CS_REGULAR_WEEKS - 1) {
            seed_playoffs();
            int seed = seed_of_user();
            if (seed >= 1 && seed <= 4 && show) message(0x8030046C, pad);
            if (seed >= 5 && show) message(0x803004AC, pad);
            if (seed == 0) {
                if (show) message(0x8030040C, pad);
                finish_from = WILDCARD;
            }
        }
    }
    else if (!nested) {
        int won = 0;
        for (int i = 0; i < n; i++)
            if (games[i].home == user || games[i].away == user) won = winner(games[i], score_home(week, i), score_away(week, i)) == user;
        if (week == SUPER_BOWL) {
            if (user_plays && show) {
                if (won) season_rating(qlen);
                else message(0x8030044C, pad);
            }
        }
        else if (!user_plays) {
            if (week == WILDCARD && show) message(0x803003DC, pad); /* the top seed rests */
        }
        else if (won) {
            if (show) message(week == WILDCARD ? 0x8030048C : week == DIVISIONAL ? 0x803004CC : 0x803004EC, pad);
        }
        else {
            if (show) message(0x8030042C, pad);
            finish_from = week + 1;
        }
    }
    if (finish_from >= 0) {
        for (int w = finish_from; w < CS_TOTAL_WEEKS; w++) {
            wr8(REC_WEEK, (u8)w);
            if (!run_week(pad, 1)) return 0;
        }
        wr8(REC_WEEK, SEASON_OVER);
        return 1;
    }
    if (nested) return 1;
    if (rd32(ABORT_FLAG) == 1) return 0;
    wr8(REC_WEEK, (u8)(week + 1));
    return 1;
}

/* ovl_b 802FBBE4 (a0 = pointer to the controller/pad value). */
void cs_week_driver(s32* pad_ptr) {
    s32 pad = *pad_ptr;
    wr32(PAD_STORE, pad);
    league_index();
    run_week(pad, 0);
}

/* ovl_b 802FDEF4 (week, out): 18 entries {home, away, home score, away score, u16 flags}; games first, then byes (flag
 * 0x80), then empty entries. Undecided playoff games are {-1, -1, .., 0x401}. Returns 0 past the season. */
s32 cs_build_week(s32 week, u32 out) {
    if (week < 0 || week >= CS_TOTAL_WEEKS) return 0;
    league_index();
    u8 e[18][4];
    u16 f[18];
    int count = 0;
    Game games[CS_MAX_GAMES];
    int n = games_of(week, games);
    u32 busy = 0;
    for (int g = 0; g < n && count < 18; g++) {
        e[count][0] = (u8)games[g].home;
        e[count][1] = (u8)games[g].away;
        e[count][2] = score_home(week, g);
        e[count][3] = score_away(week, g);
        u16 flag = 1;
        if (division_of[(int)games[g].home] == division_of[(int)games[g].away]) flag |= 0x600;
        else if (conference_of_team(games[g].home) == conference_of_team(games[g].away)) flag |= 0x400;
        f[count++] = flag;
        busy |= 1u << games[g].home | 1u << games[g].away;
    }
    if (week >= CS_REGULAR_WEEKS) {
        static const int round_games[] = {6, 4, 2, 1};
        while (count < round_games[week - CS_REGULAR_WEEKS]) {
            e[count][0] = e[count][1] = e[count][2] = e[count][3] = 0xFF;
            f[count++] = 0x401;
        }
        if (week == WILDCARD && seeded()) {
            for (int c = 0; c < 2; c++) {
                e[count][0] = (u8)seed_at(c, 0);
                e[count][1] = e[count][2] = e[count][3] = 0xFF;
                f[count++] = 0x80;
            }
        }
    }
    else {
        for (int t = 0; t < CS_TEAMS && count < 18; t++) {
            if ((busy >> t) & 1) continue;
            e[count][0] = (u8)t;
            e[count][1] = e[count][2] = e[count][3] = 0xFF;
            f[count++] = 0x80;
        }
    }
    for (int i = 0; i < 18; i++) {
        u32 a = out + 6u * (u32)i;
        for (int k = 0; k < 4; k++) wr8(a + (u32)k, i < count ? e[i][k] : 0xFF);
        *(volatile u16*)(a + 4) = i < count ? f[i] : 0;
    }
    return 1;
}

/* ovl_b 802FEA74 (out, div): the division's 4 teams in standings order, 0x28 bytes each. */
void cs_standings(u32 out, s32 div) {
    if (div < 0 || div >= CS_DIVISIONS) return;
    new_table();
    const int* o = division_order(div);
    for (int i = 0; i < CS_DIVISION_SIZE; i++) {
        int t = o[i];
        u32 e = out + 0x28u * (u32)i;
        Record recs[5] = {overall(t), record_of(t, K_HOME, 0), record_of(t, K_AWAY, 0), record_of(t, K_DIVISION, 0),
                          record_of(t, K_CONFERENCE, 0)};
        wr8(e, (u8)t);
        for (int k = 0; k < 5; k++) {
            wr8(e + 1 + 2u * (u32)k, (u8)recs[k].w);
            wr8(e + 2 + 2u * (u32)k, (u8)recs[k].l);
            wrf(e + 0x14 + 4u * (u32)k, (float)rec_pct(recs[k]));
        }
        wr8(e + 0xB, 0);
        wr32(e + 0xC, points_for(t));
        wr32(e + 0x10, points_against(t));
    }
}

/* The season record's reset (main 0x80263AEC, stubs.S): results unplayed, no seeds, team 31's stats zero (the
 * reset's 0xFF fill would be NaN), and a new coin-toss seed. */
void cs_new_season(void) {
    for (u32 a = REC_RESULTS; a < REC_RESULTS + CS_TOTAL_WEEKS * CS_MAX_GAMES * 2; a++) wr8(a, 0xFF);
    for (u32 a = REC_STATS31; a < REC_STATS31 + 28; a++) wr8(a, 0);
    for (u32 a = REC_SEEDS; a < REC_SEEDS + 2 * CS_SEEDS; a++) wr8(a, 0xFF);
    wr32(REC_TAG, (s32)SEASON_TAG);
    wr32(REC_SEED, (s32)(count_register() * 0x9E3779B9u ^ 0x5EA5011u));
}

/* ---- screens: names ---- */
static u32 div_name_ptrs[CS_DIVISIONS];
static u32 week_name_ptrs[CS_TOTAL_WEEKS];

/* ---- patches ---- */
extern void cs_fix_leaders_a(void), cs_fix_leaders_b(void), cs_menu_pos(void), cs_schedule_column(void),
    cs_new_season_tail(void);
void cs_ovl_b_loaded(s32 a0);

typedef struct { u32 addr, old, now; } Patch;
#define J(t) (0x08000000u | (((u32)(t) >> 2) & 0x03FFFFFFu))
#define JAL(t) (0x0C000000u | (((u32)(t) >> 2) & 0x03FFFFFFu))
#define HI(a) ((((u32)(a)) + 0x8000u) >> 16)
#define LO(a) (((u32)(a)) & 0xFFFFu)
#define NOP 0u

static int apply(const Patch* p, int n) {
    int done = 0;
    for (int i = 0; i < n; i++) {
        volatile u32* w = (volatile u32*)p[i].addr;
        if (*w != p[i].old) continue;
        *w = p[i].now;
        osWritebackDCache((void*)p[i].addr, 4);
        osInvalICache((void*)p[i].addr, 4);
        done++;
    }
    return done;
}

static Patch ovl_b_patches[64];
static int ovl_b_count;

static void add(Patch* list, int* n, u32 addr, u32 old, u32 now) { list[(*n)++] = (Patch){addr, old, now}; }

static void build_ovl_b_patches(void) {
    Patch* p = ovl_b_patches;
    int n = 0;
    /* the four functions */
    add(p, &n, 0x802FBBE4, 0x27BDFFB0, J(cs_week_driver));
    add(p, &n, 0x802FBBE8, 0x00003021, NOP);
    add(p, &n, 0x802FDEF4, 0x27BDFFF0, J(cs_build_week));
    add(p, &n, 0x802FDEF8, 0xAFB20008, NOP);
    add(p, &n, 0x802FBAAC, 0x27BDFFE0, J(cs_cpu_skill));
    add(p, &n, 0x802FBAB0, 0xAFB00010, NOP);
    add(p, &n, 0x802FEA74, 0x27BDFE70, J(cs_standings));
    add(p, &n, 0x802FEA78, 0xF7B40188, NOP);
    /* the game sim: team indices up to 31 */
    add(p, &n, 0x802FD0D0, 0x2BC2001F, 0x2BC20020);
    add(p, &n, 0x802FD0FC, 0x241E001E, 0x241E001F);
    add(p, &n, 0x802FD104, 0x2942001F, 0x29420020);
    add(p, &n, 0x802FD10C, 0x2409001E, 0x2409001F);
    /* league leaders: 32 teams, team 31's stats at +0x704 */
    add(p, &n, 0x802FFED8, 0xC4400000, J(cs_fix_leaders_a));
    add(p, &n, 0x802FFEEC, 0x28E2001F, 0x28E20020);
    add(p, &n, 0x802FFF00, 0x24AB001F, 0x24AB0020);
    add(p, &n, 0x802FFF34, 0x28E2001F, 0x28E20020);
    add(p, &n, 0x802FFF88, 0xC4400000, J(cs_fix_leaders_b));
    add(p, &n, 0x802FFF9C, 0x28E2001F, 0x28E20020);
    /* standings: 8 division names and positions, 4 rows each */
    add(p, &n, 0x802FF348, 0x3C148033, 0x3C140000u | HI(div_name_ptrs));
    add(p, &n, 0x802FF34C, 0x2694E978, 0x26940000u | LO(div_name_ptrs));
    add(p, &n, 0x802FF464, 0x2652000C, 0x26520000); /* entries past the game's 6 keep reading entry 0's position floats:
                                                      * past its table they'd be garbage, a floating-point exception on
                                                      * hardware (cs_menu_pos sets the position anyway) */
    add(p, &n, 0x802FF480, 0x0C09E59C, JAL(cs_menu_pos));
    add(p, &n, 0x802FF4AC, 0x2A220006, 0x2A220008);
    add(p, &n, 0x802FF4E4, 0x27A80038, 0x27A80040);
    add(p, &n, 0x802FF524, 0x27A40038, 0x27A40040);
    add(p, &n, 0x802FF660, 0x2A420006, 0x2A420004);
    add(p, &n, 0x802FF6C0, 0x2A620006, 0x2A620008);
    add(p, &n, 0x802FF6C8, 0x24130005, 0x24130007);
    add(p, &n, 0x802FF6EC, 0x2A220006, 0x2A220008);
    /* schedule browser: weeks to the Super Bowl (22), team 31 */
    add(p, &n, 0x80301128, 0x28420015, 0x28420016);
    add(p, &n, 0x80301134, 0x24020014, 0x24020015);
    add(p, &n, 0x8032EC48, 0x0000001E, 0x0000001F); /* the browser's last team */
    add(p, &n, 0x8032EC4C, 0x00000014, 0x00000015); /* and last week */
    /* week results: week names, scores from the builder's entries, 16 rows of 14 px */
    add(p, &n, 0x803016C8, 0x3C078033, 0x3C070000u | HI(week_name_ptrs));
    add(p, &n, 0x803016D0, 0x8CE7EC50, 0x8CE70000u | LO(week_name_ptrs));
    add(p, &n, 0x803017C0, 0x00731821, 0x00711821);
    add(p, &n, 0x803017C4, 0x9063FA71, 0x90630000u | LO(0x802E950B));
    add(p, &n, 0x803017EC, 0x00F33821, 0x00F13821);
    add(p, &n, 0x803017F0, 0x90E7FA71, 0x90E70000u | LO(0x802E950B));
    add(p, &n, 0x803018C8, 0x00731821, 0x00711821);
    add(p, &n, 0x803018CC, 0x9063FA70, 0x90630000u | LO(0x802E950A));
    add(p, &n, 0x803018F4, 0x00F33821, 0x00F13821);
    add(p, &n, 0x803018F8, 0x90E7FA70, 0x90E70000u | LO(0x802E950A));
    add(p, &n, 0x80301960, 0x2A82000F, 0x2A820010);
    add(p, &n, 0x80301954, 0x2652FFF1, 0x2652FFF2);
    add(p, &n, 0x8030190C, 0x2652FFF1, 0x2652FFF2);
    /* team schedule: 11 + 11 weeks per column, 16 games, 18 regular weeks, 22 in all, week names */
    add(p, &n, 0x80301AF8, 0x01099023, JAL(cs_schedule_column));
    add(p, &n, 0x80301C30, 0x2AA2000F, 0x2AA20010);
    add(p, &n, 0x80301C3C, 0x2A820011, 0x2A820012);
    add(p, &n, 0x80301C9C, 0x2A830011, 0x2A830012);
    add(p, &n, 0x80301CE0, 0x2A820015, 0x2A820016);
    add(p, &n, 0x80301C80, 0x3C078033, 0x3C070000u | HI(week_name_ptrs));
    add(p, &n, 0x80301C88, 0x8CE7EC50, 0x8CE70000u | LO(week_name_ptrs));
    ovl_b_count = n;
}

/* After each ovl_b load (main's two loaders, before their call to 0x80014100, which this goes on to). */
void cs_ovl_b_loaded(s32 a0) {
    apply(ovl_b_patches, ovl_b_count);
    GAME(fn1, 0x80014100)(a0);
}

/* Boot (tools/rom_picker/cart32.asm's loader, 8 MB only): the names for the screens, main's patches, and the hooks
 * that patch ovl_b each time it loads. */
void cart_init(void) {
    if (LEAGUE->magic != CS_LEAGUE_MAGIC) return;
    for (int d = 0; d < CS_DIVISIONS; d++) div_name_ptrs[d] = (u32)LEAGUE->div_names[d];
    for (int w = 0; w < CS_TOTAL_WEEKS; w++) week_name_ptrs[w] = (u32)LEAGUE->week_names[w];
    build_ovl_b_patches();
    Patch main_patches[] = {
        {0x8024B80C, 0x2CE20011, 0x2CE20012}, /* VS banner: 18 regular weeks */
        {0x8024B834, 0x2CE20014, 0x2CE20015}, /* the Super Bowl is week 22 */
        {0x802599BC, 0x2C420015, 0x2C420016}, /* season header: over after week 22 */
        {0x80263AEC, 0x8FBF0014, J(cs_new_season_tail)},
        {0x80262774, 0x0C005040, JAL(cs_ovl_b_loaded)},
        {0x80262B34, 0x0C005040, JAL(cs_ovl_b_loaded)},
    };
    apply(main_patches, (int)(sizeof(main_patches) / sizeof(main_patches[0])));
}

#ifndef CART_HOST_TEST
/* The league block, filled by Export ROM (cart_season.h CS_LEAGUE). */
__attribute__((section(".league"), used)) u8 cs_league_space[0x5C0];

/* freestanding helpers the compiler may call */
void* memset(void* d, int c, unsigned n) { u8* p = d; while (n--) *p++ = (u8)c; return d; }
void* memcpy(void* d, const void* s, unsigned n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }
#endif
