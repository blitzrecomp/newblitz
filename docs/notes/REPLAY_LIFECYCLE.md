# Instant Replay: when a play starts and ends (1.0.4)

How the replay ring (docs/REPLAY.md) decides what counts as "the last play". Fixed for 1.0.4 after the team's report from an iPhone
on 1.0.3.

## The bug

The team ran a play, picked the next one, and opened Instant Replay from the pause menu before the snap. The replay showed the next
play's line-up, which hadn't started yet, not the play he had just run.

## Root cause

`update_keeping()` in `src/main/replay_screen.cpp` decided both what to keep and where a play starts, using play select alone:

- Capture was on whenever a game was up and play select and the pause menu weren't: `in_game && !select && !pause`. That included
  the camera's fly-in after play select and the whole line-up (audibles, waiting to snap).
- A play's start mark was set when play select went off screen (`was_select && !select`), not at the snap.

So once play select closed, a new "play" began in the ring and filled with the line-up. `begin()` opens the last marked play
unless it's under 1.5 s old, and a line-up is usually longer than that, so a replay opened at the line showed the line-up. The
line-up's frames also pushed the real play out of the ring: about 8 s at 15 fps on the small tier (handhelds), 10 s on the
large one (phones, PS5).

## The fix

The game's own play phase, `0x802EA4A8` (already used by `receiver_targets.cpp`, `tall_view.cpp`, `ds_stylus.cpp`), seen on
every frame at the dispatcher's loop head (`blitz_replay_gate`, `src/game/replay.cpp`):

| Phase | When |
|---|---|
| 6 | the coin toss, before the opening kickoff |
| 1 | at the line: the camera's fly-in and the line-up after play select; a kickoff's run-up |
| 2 | live: from the snap to the whistle; a kickoff or punt return |
| 5 | the opening kickoff's ball in the air (seen 1 -> 5 -> 2) |
| 3 | after the whistle, through play select |
| 4 | set once in ovl_a near 0x803022A4; never seen live (an end-of-period path, by its code) |

Outside a game (`0x802EA420` not 2) it's read as 0. Between quarters `0x802EA420` is 3.

- `replay.cpp` counts each entry into "live" (2 or 5) from anything else: `blitz::replay_snaps`, plus `blitz::replay_play_live`.
- `update_keeping()` marks the next kept frame as a play's start when the count moves, and keeps frames only while the play is
  live and for 2 s after its whistle (late hits, the ball carrier down). Nothing is kept from then until the next snap.
- So the previous play stays whole and last in the ring through play select, the fly-in and the line-up, and is replaced only
  once the ball is snapped (or kicked).

Two smaller fixes beside it:

- `begin()`: opened under 1.5 s into a play with only one earlier play in the ring, it opened at the new play anyway (the loop left
  `start` at the last mark it tried). It now falls back to the ring's first play.
- The ring was freed 5 s into a quarter break (`0x802EA420` is 3 then, not 2), so the quarter's last play was gone at the next
  line-up. Phase 3 now keeps it. It is still freed 5 s after the game goes back to the menus.

`BLITZ_REPLAY_TRACE=1` logs each phase change (`[replay] play phase 1 -> 2 (ms, kind, game phase)`) and, on opening, where each
play starts in the ring.

## Checked (Mac, Modern, `BLITZ_REPLAY_TIER=2`: the phone's 640x360 at 25 fps)

- Opened at the line before the snap (live command and the pause menu's REPLAY): the play just run, ending 2 s after its whistle
  (a "5 YARD LOSS" and "TURNOVER ON DOWNS" banner), none of the line-up. Closed: the game snapped and played on.
- Opened 0.7 s into a play: the play before.
- Scrimmage plays, the opening kickoff (6 -> 1 -> 5 -> 2 -> 3), kickoffs after scores (3 -> 1 -> 2, the run-up 35 ms), a field
  goal (1 -> 2 -> 3), touchdowns, turnovers on downs, the quarter break.

## Left as they were

- **Punts** weren't seen (the scripted games never punted). If a punt passes through a phase other than 2 or 5 mid-play (4,
  say), the return would be marked as a play of its own, and a gap over 2 s would stop keeping frames until the return. Worth a
  look in a real game with `BLITZ_REPLAY_TRACE=1`.
- **Redo Play** rewinds to the top of a play; the undone play stays in the ring as the last play until the next snap, so a replay
  opened after Redo shows the play that was undone. The ring has no way to drop frames (an RT64 patch change).
- **A save state loaded mid-play into a live play** (2 to 2) doesn't start a new play in the ring; one loaded at the line does.
- The music still moves on while a replay is up (docs/REPLAY.md, Audio).
