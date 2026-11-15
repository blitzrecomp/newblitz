# Blitz rosters

Weekly NFL rosters for Modern Blitz (BlitzRecomp). Private, internal. The design is `docs/design/ROSTER_UPDATES.md` in the blitz repo.

| File | What |
|---|---|
| `players.json` | Every player on the 32 clubs, plus anyone who has since left one (`team: null`), keyed by ESPN id. `skin: null` means the player needs a shade picked in Roster Room. |
| `lineups/full.json` | Full strength: each team's 15 game slots as ESPN ids, injuries ignored. |
| `lineups/2026-wNN.json` | One checkpoint per week: healthy starters for that week's games. |
| `psp/pack.txt` | The PSP's copy (blitz `docs/PSP_PORT.md` §6): the players the 4 latest checkpoints and full strength use, plus those lineups, as one small text file. Written with the manifest, from the files above. |
| `manifest.json` | What the app checks at launch: the newest checkpoint, every file's hash, and `targets`: `psp/pack.txt` goes to the PSP only, everything else to the other platforms only (`JSON_PLATFORMS` in `roster_season.py`: **a new platform must be added there**, or it gets no rosters). A list set with `tools/roster_season.py target <path> --platforms …` is marked `by_hand` and the job keeps it. |

**The job:** `.github/workflows/rosters.yml` runs every day at 6:17 a.m. Pacific from open data ([nflverse](https://github.com/nflverse/nflverse-data), CC-BY-4.0; Sleeper's public player list fills missing ESPN ids). It also runs by hand: Actions → Rosters → Run workflow, with an optional week number. Each run reads the 32 rosters, adds new faces, marks departures as free agents, writes the week's checkpoint (each club's slots freeze at its kickoff) and full strength, and commits only when something changed. **The backup:** `rosters-espn.yml`, the original ESPN flow (tag `espn-flow`), run by hand; it writes the same files.

**The scripts:** `tools/` is a copy of `roster_season.py`, `roster_fetch.py` and `roster_nflverse.py` from the blitz repo, plus `psp_pack.py` (blitz's `tools/psp/make_pack.py`). Edit them there, then run `tools/rosters_publish.sh` in the blitz repo, which copies them here and pushes.

**By hand** (in a checkout):
- `python3 tools/roster_season.py new --dir .`: who needs a shade (`*` marks players in a lineup).
- `python3 tools/roster_season.py shades EXPORT.json --dir .`: shades from a Roster Room export.
- `python3 tools/roster_season.py load --dir . --pack <blitz>/packs/blitz2026`: the yearly load, after cuts.
