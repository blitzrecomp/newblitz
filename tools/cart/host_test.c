/* The cart season's league logic on the host (src/cart/season.c, built with CART_HOST_TEST): standings, seeding and
 * the bracket over random results, with the league block from an exported ROM.
 *   cc -O1 -DCART_HOST_TEST -Isrc/cart tools/cart/host_test.c -o build/cart/host_test && build/cart/host_test ROM */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "season.c"

u8 host_ram[0x800000];
static u8* at(u32 a) { return &host_ram[a - 0x80000000u]; }
u8 rd8(u32 a) { return *at(a); }
void wr8(u32 a, u8 v) { *at(a) = v; }
s32 rd32(u32 a) { u8* p = at(a); return (s32)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]); }
void wr32(u32 a, s32 v) { u8* p = at(a); p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
float rdf(u32 a) { s32 b = rd32(a); float f; memcpy(&f, &b, 4); return f; }
void wrf(u32 a, float f) { s32 b; memcpy(&b, &f, 4); wr32(a, b); }
u32 count_register(void) { return 12345; }
void cs_fix_leaders_a(void) {}
void cs_fix_leaders_b(void) {}
void cs_menu_pos(void) {}
void cs_schedule_column(void) {}
void cs_new_season_tail(void) {}

int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    u8* rom = malloc((size_t)n);
    fseek(f, 0, SEEK_SET);
    fread(rom, 1, (size_t)n, f);
    long found = -1;
    for (long i = 0; i + 0x600 < n; i += 16)
        if (rom[i + 0x40] == 0x42 && rom[i + 0x41] == 0x5A && rom[i + 0x42] == 0x4C && rom[i + 0x43] == 0x47) found = i;
    if (found < 0) { puts("no league block"); return 1; }
    /* host is little endian: the league block is bytes only, except magic/team_table (unused here) */
    memcpy(at(CS_LEAGUE), rom + found + 0x40, sizeof(CartLeague));
    cs_new_season();
    league_index();
    clock_t t0 = clock();
    u8 out[0x28 * 4];
    for (int d = 0; d < 8; d++) {
        cs_standings(0x80500000u, d);
        memcpy(out, at(0x80500000u), sizeof(out));
        printf("div %d:", d);
        for (int i = 0; i < 4; i++) printf(" %2d", out[0x28 * i]);
        printf("\n");
    }
    printf("empty standings: %.3f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    srand(argc > 2 ? (unsigned)atoi(argv[2]) : 7);
    for (int w = 0; w < CS_REGULAR_WEEKS; w++)
        for (int g = 0; g < LEAGUE->games[w]; g++) {
            wr8(REC_RESULTS + (u32)(w * CS_MAX_GAMES + g) * 2, (u8)(rand() % 40));
            wr8(REC_RESULTS + (u32)(w * CS_MAX_GAMES + g) * 2 + 1, (u8)(rand() % 40));
        }
    FILE* rf = fopen("build/cart/results.bin", "wb");
    fwrite(at(REC_RESULTS), 1, CS_TOTAL_WEEKS * CS_MAX_GAMES * 2, rf);
    fwrite(LEAGUE, 1, sizeof(CartLeague), rf);
    fclose(rf);
    t0 = clock();
    for (int d = 0; d < 8; d++) {
        cs_standings(0x80500000u, d);
        memcpy(out, at(0x80500000u), sizeof(out));
        printf("div %d:", d);
        for (int i = 0; i < 4; i++) printf(" %2d %d-%d", out[0x28 * i], out[0x28 * i + 1], out[0x28 * i + 2]);
        printf("\n");
    }
    seed_playoffs();
    for (int c = 0; c < 2; c++) { printf("seeds %d:", c); for (int i = 0; i < 7; i++) printf(" %d", seed_at(c, i)); printf("\n"); }
    printf("full season: %.3f s\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    return 0;
}
