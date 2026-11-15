# Season schedule research prompt

Give this prompt to a Claude Code agent once the NFL schedule is out (usually May). Replace YEAR with the season year. It writes `build/analysis/schedule<YEAR>.json`. Then run:

```
.venv/bin/python tools/roster_pack.py season build/analysis/schedule<YEAR>.json packs/blitz2026
```

---

Collect the complete regular-season schedule of the YEAR NFL season: all 18 weeks, 272 games, with the away and home team of every game. This is public, factual data. Use web search and fetch reliable public sources (NFL.com / NFL Operations, ESPN, Pro-Football-Reference, Wikipedia "YEAR NFL season", team sites), and cross-check at least two of them.

Write `build/analysis/scheduleYEAR.json` in this shape:

```
{"season": YEAR, "sources": [...], "weeks": [[["AWAY","HOME"], ...], ...]}
```

There are 18 week lists. For neutral-site and international games, list the designated home team as home.

Team ids are: ARI ATL BAL BUF CAR CHI CIN CLE DAL DEN DET GB HOU IND JAX KC LV LAC LAR MIA MIN NE NO NYG NYJ PHI PIT SF SEA TB TEN WAS.

Validate with a short Python script, and fix anything that fails:
- 18 weeks and 272 games;
- every team plays exactly 17 games, never twice in a week;
- every team has exactly one bye;
- each team plays its 3 division rivals home and away;
- each team has 8 or 9 home games.

Report the sources, the validation results, the most byes in a single week, and any games you were unsure of. Never invent games.
