/* Advanced season on the cart (src/cart/season.c): 32 teams in 8 divisions of 4, the pack's 18-week schedule, NFL
 * tiebreakers and 7-team playoffs, run on the N64 from the default heap's old top (0x800E4700, any N64). Shared with Export ROM
 * (src/game/export_rom.cpp), which fills the league block in the built code before it goes into the ROM. */
#ifndef CART_SEASON_H
#define CART_SEASON_H
#include <stdint.h>

#define CS_TEAMS 32
#define CS_DIVISIONS 8
#define CS_DIVISION_SIZE 4
#define CS_REGULAR_WEEKS 18
#define CS_TOTAL_WEEKS 22
#define CS_MAX_GAMES 16
#define CS_SEEDS 7

#define CS_BASE 0x800E4700u      /* where the boot loader puts the code (engine32_layout.h cart::season) */
#define CS_LEAGUE 0x800E4740u    /* CartLeague, filled by the export */
#define CS_LEAGUE_MAGIC 0x425A4C47u  /* "BZLG" */

typedef struct {
    uint32_t magic;                                       /* CS_LEAGUE_MAGIC once filled */
    uint32_t team_table;                                  /* the 32-team records (cpu skill at +0x0C, 0x64 each) */
    uint8_t members[CS_DIVISIONS][CS_DIVISION_SIZE];      /* team slots: AFC East, North, South, West, then the NFC's */
    uint8_t games[CS_REGULAR_WEEKS];                      /* games per regular week */
    uint8_t pad[2];
    uint8_t schedule[CS_REGULAR_WEEKS][CS_MAX_GAMES][2];  /* {away, home} */
    char div_names[CS_DIVISIONS][12];                     /* "EAST", ... */
    char week_names[CS_TOTAL_WEEKS][14];                  /* "Week 1" .. "SuperBowl" */
} CartLeague;

#endif
