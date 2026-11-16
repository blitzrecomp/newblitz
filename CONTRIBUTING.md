# Contributing

Thanks for helping keep Blitz alive. Start in **#dev** on our [Discord](https://discord.gg/AtcKN9dY5): say what you're making, ask when
you're stuck. Bugs: #bugs, or an issue here with the zip from Settings → General → Save Logs. The
[contributor guide](https://newblitz.com/contribute) covers roster packs, uniform sets, ports to new devices, and
prompts for AI coding agents.

## The one rule: no game data

Never commit a ROM, anything extracted from one (images, sounds, text, models), or a save, and never post links to ROMs. `rom/` and `build/` are git-ignored for this reason. Art that replaces a game image must be drawn from scratch; `tools/` shows how the existing replacements are generated.

## Building

```
make env            # checks the toolchain (Xcode command line tools, cmake, ninja, python3)
make venv           # Python tools
tools/build_deps.sh # SDL2 and FreeType for macOS 13+ (once)
# put your NFL Blitz 2000 (USA) v1.1 dump in rom/
make rom-import && make app-modern   # the Mac app
make test
```

The 2026 Season pak isn't in this repository: the app downloads it (docs/design/SEASON_PAK.md).

## How the code is organised

- `blitz2000.toml`: hooks into the recompiled game (function, address, C text). Each hook calls host code in `src/game/`.
- `src/game/`: features (rules, seasons, packs, art, save states). `src/main/`: the app (launcher, controllers, audio).
- `lib/RecompFrontend`: the UI, patched by `platform/*-patches` (`tools/rf_patches.py` regenerates them after you edit it).
- `docs/` has one file per larger feature.

## Pull requests

- Say what you tested and how; a screenshot helps for anything visible.
- Keep a change to one feature, and match the surrounding code's style.
- By contributing you agree your code is released under the GPL-3.0 (LICENSE).
- Don't attach a ROM, a save, a save state or anything taken from the game to an issue or a pull request.
