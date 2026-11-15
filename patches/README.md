Game-specific patch code, compiled for MIPS and linked into the recompiled game.

One file per system (e.g. `input.c`, `widescreen.c`, `rosters.c`). Season-pack loaders
come later; record candidate hook addresses in NOTES.md under "Data loaders" first.

Not built yet: the MIPS patch pipeline isn't in this repo's build (STATUS.md "Known gaps";
prototype in docs/patch_pipeline_prototype/). Game changes so far, content packs included, are
host-side hooks in src/game/ (hooked from blitz2000.toml, and tools/teamplus.py for Modern Blitz).
