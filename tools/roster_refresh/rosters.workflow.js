// Roster refresh research (Claude Code workflow). Run it from Claude Code with
//   args: {today: "YYYY-MM-DD", season: "2027"}
// and save its result as build/analysis/rosters<season>/raw.json, then
//   .venv/bin/python tools/roster_pack.py build build/analysis/rosters<season>/raw.json packs/blitz2026
// (packs/blitz2026/README.md "Refreshing"). Public sources only; never from ROM hacks.
export const meta = {
  name: 'blitz-roster-refresh',
  description: 'Compile current NFL rosters (32 teams) into a blitz-pack/2 roster pack from public sources, then fact-check a sample',
  phases: [
    { title: 'Compile', detail: 'one agent per division' },
    { title: 'Check', detail: 'independent fact-check of a sample' },
  ],
}

const DIVISIONS = [
  ['AFC East', ['BUF', 'MIA', 'NE', 'NYJ']],
  ['AFC North', ['BAL', 'CIN', 'CLE', 'PIT']],
  ['AFC South', ['HOU', 'IND', 'JAX', 'TEN']],
  ['AFC West', ['DEN', 'KC', 'LV', 'LAC']],
  ['NFC East', ['DAL', 'NYG', 'PHI', 'WAS']],
  ['NFC North', ['CHI', 'DET', 'GB', 'MIN']],
  ['NFC South', ['ATL', 'CAR', 'NO', 'TB']],
  ['NFC West', ['ARI', 'LAR', 'SF', 'SEA']],
]
// Slot roles are the game's fixed layout (docs/CONTENT_MAP.md §4). The exact position in a flexible
// slot (WR or TE; CB, S or LB) is the researcher's call: the current starter who best fits.
const ROLES = ['OL(unnamed)', 'OL(unnamed)', 'OL(unnamed)', 'QB', 'RB', 'WR/TE', 'WR/TE', 'DL(unnamed)', 'DL(unnamed)',
  'DL(unnamed)', 'LB/DB', 'LB/DB', 'LB/DB', 'LB/DB', 'K', 'unused']
const TODAY = args && args.today ? args.today : 'today'
const SEASON = args && args.season ? args.season : 'current'

const COMMON = `
You are compiling current NFL roster data for a personal, non-distributed native port of NFL Blitz 2000 (N64). Today is ${TODAY}; use the CURRENT ${SEASON} NFL season rosters / depth charts (load WebSearch/WebFetch via ToolSearch; prefer team official sites, NFL.com, ESPN depth charts, Pro-Football-Reference, Wikipedia team pages). Do NOT copy from any NFL Blitz ROM hack (Resurrection, NBIE) — compile from public sports sources only. Don't contact anyone.
The game has 16 player slots per team with fixed roles: ${JSON.stringify(ROLES)}.
For every slot 0-14:
- named slots: the team's current starter (slot 3 QB, 4 RB, 5 WR1, 6 WR2 or the top TE if he is the bigger target, 10-13 the four most prominent defenders among LB/CB/S, 14 K). Give "last" (last name only, ASCII, letters/space/./'/- only, max 15 chars, ideally <= 12; transliterate accents; e.g. "St. Brown", "Smith-Njigba" is 12) and "number" (current jersey 0-99).
- unnamed slots (OL, DL): just "number" of a current starting OL/DL (distinct players). Omit "last".
- skip slot 15 entirely.
Do not include skin tone, size or any physical attribute. Each player entry: {"slot": n, "last": "...", "number": n} (or without "last" for unnamed slots).
Team ratings (0-5 each) from the most recent complete regular season (complete data), using league ranks among 32 teams: passing = passing yards per game (offense), rushing = rushing yards per game (offense), linemen = sacks allowed (fewest = best), defense = points allowed per game (fewest = best), special_teams = field goal percentage. Map rank to rating: 1-5 -> 5, 6-11 -> 4, 12-17 -> 3, 18-23 -> 2, 24-28 -> 1, 29-32 -> 0. State the source of each stat table.
Return confidence per team and list any slot where you're unsure (injury, depth-chart ambiguity).`

const TEAM_SCHEMA = {
  type: 'object',
  properties: {
    teams: { type: 'array', items: { type: 'object', properties: {
      franchise: { type: 'string' },
      ratings: { type: 'object', properties: { passing: { type: 'integer' }, rushing: { type: 'integer' }, linemen: { type: 'integer' }, defense: { type: 'integer' }, special_teams: { type: 'integer' } }, required: ['passing', 'rushing', 'linemen', 'defense', 'special_teams'] },
      players: { type: 'array', items: { type: 'object', properties: { slot: { type: 'integer' }, last: { type: 'string' }, number: { type: 'integer' } }, required: ['slot', 'number'] } },
      as_of: { type: 'string' },
      sources: { type: 'array', items: { type: 'string' } },
      unsure: { type: 'array', items: { type: 'string' } },
    }, required: ['franchise', 'ratings', 'players', 'as_of', 'sources', 'unsure'] } },
    rating_sources: { type: 'string' },
  },
  required: ['teams', 'rating_sources'],
}
const CHECK_SCHEMA = {
  type: 'object',
  properties: { results: { type: 'array', items: { type: 'object', properties: {
    franchise: { type: 'string' }, slot: { type: 'integer' }, claimed: { type: 'string' }, correct: { type: 'boolean' }, fix: { type: 'string' }, evidence: { type: 'string' } },
    required: ['franchise', 'slot', 'claimed', 'correct', 'fix', 'evidence'] } } },
  required: ['results'],
}

phase('Compile')
const divs = await parallel(DIVISIONS.map(([name, teams]) => () =>
  agent(`${COMMON}
Division: ${name}. Teams: ${teams.join(', ')}.`,
    { label: `compile:${name}`, phase: 'Compile', schema: TEAM_SCHEMA })))
const teams = divs.filter(Boolean).flatMap(d => d.teams)
log(`compiled ${teams.length} teams`)

phase('Check')
// Fact-check: every QB and RB, plus WR1, for all teams, split into 4 batches.
const claims = teams.flatMap(t => (t.players || []).filter(p => p.last && [3, 4, 5].includes(p.slot)).map(p => ({ franchise: t.franchise, slot: p.slot, claimed: `${p.last} #${p.number}` })))
const batches = [0, 1, 2, 3].map(i => claims.filter((_, k) => k % 4 === i))
const checks = await parallel(batches.map((b, i) => () =>
  agent(`Fact-check these current (${TODAY}, ${SEASON} NFL season) roster claims against public sources (load WebSearch/WebFetch via ToolSearch; team sites, ESPN, NFL.com). Slot 3 = starting QB, 4 = starting RB, 5 = WR1. For each: correct (right player for that role on that team now, and right jersey number)? If not, give the fix ("Last #nn"). Be skeptical; default correct=false if you cannot confirm.
${JSON.stringify(b)}`, { label: `check:${i}`, phase: 'Check', schema: CHECK_SCHEMA })))
const results = checks.filter(Boolean).flatMap(c => c.results)
return { teams, rating_sources: divs.filter(Boolean).map(d => d.rating_sources), check: results }
