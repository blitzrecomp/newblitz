# Notices

This project is released under the GNU General Public License, version 3 (LICENSE). It builds on the work below. Each
one's full licence text is in the folder named, which a clone with `--recurse-submodules` (or `make patches`) fetches.
Who did what, in plain words: CREDITS.md. Where each piece came from: docs/PROVENANCE.md.

This project is not affiliated with, endorsed by or sponsored by the NFL, the NFLPA, Midway Games, Warner Bros. or
Nintendo. It contains no game data: you supply your own cartridge dump of NFL Blitz 2000.

## Source for a build you were given

An app built from this project combines GPL-3.0 code (N64ModernRuntime, RecompFrontend and this repository), so the
whole app is under the GPL-3.0. The complete source is this repository at the commit shown in Settings → General →
About, with the library commits it pins (`git submodule status`) and our changes to them (`platform/*-patches`).
Build steps: README.md.

## Libraries

| Component | Copyright and licence | Where |
|---|---|---|
| N64Recomp | Copyright (c) 2024 Wiseguy. MIT | lib/N64Recomp/LICENSE |
| N64ModernRuntime | The N64Recomp contributors. GPL-3.0 | lib/N64ModernRuntime/COPYING |
| RecompFrontend (grown from the Zelda64Recomp UI) | The N64Recomp contributors. GPL-3.0 | lib/RecompFrontend/COPYING |
| RT64 | Copyright (c) 2024 RT64 Contributors. MIT | lib/rt64/LICENSE |
| n64sym (build tooling only) | Copyright (c) 2017-2020 shygoo. MIT | ext/n64sym/LICENSE.md |
| QR Code generator | Copyright (c) Project Nayuki. MIT | third_party/qrcodegen (file headers) |
| Monocypher 4.0.2 | Its authors (see the file). CC0-1.0 or BSD-2-Clause, at our choice | third_party/monocypher (LICENCE.md) |
| BanjoRecomp | GPL-3.0. `src/main/main.cpp` and the root `CMakeLists.txt` started from its structure | file headers |
| Zelda64Recomp patch headers | GPL-3.0, with RecompFrontend | patches/*.h |
| SDL2 | zlib licence | built by tools/build_deps.sh (tools/deps.sha256) |
| AppImage type2 runtime (the start of the Linux AppImage) | The AppImage project. MIT | github.com/AppImage/type2-runtime (tools/deps.sha256) |
| FreeType | The FreeType Project. FreeType License | built by tools/build_deps.sh |
| SDL_GameControllerDB | gabomdq and contributors. zlib licence | recompcontrollerdb.txt (its header names the source) |

Libraries those bring with them, each with its licence text in its own folder under `lib/*/lib`, `lib/*/thirdparty`
or `lib/rt64/src/contrib`: RmlUi, lunasvg, GamepadMotionHelpers, SlotMap (RecompFrontend); rabbitizer, ELFIO, fmt,
tomlplusplus, sljit (N64Recomp); xxHash, miniz, o1heap, concurrentqueue, nlohmann/json, sse2neon (N64ModernRuntime);
Dear ImGui, implot, im3d, hlslpp, stb, ddspp, utf8conv, plainargs, re-spirv, SPIRV-Cross, nativefiledialog-extended,
plume, zstd, and header-only API definitions from mupen64plus (RT64). Android builds also carry LLVM's libc++
(Apache-2.0 with the LLVM exception).

Python tools (requirements.txt; installed by `make venv`, not included here): splat, spimdisasm, rabbitizer, crunch64
and others, under their own licences.

## Fonts (SIL Open Font License 1.1)

| Font | Copyright | Licence text |
|---|---|---|
| Barlow Condensed | Copyright 2017 The Barlow Project Authors | assets/BarlowCondensed-OFL.txt |
| Inter | Copyright 2016 The Inter Project Authors | assets/Inter-OFL.txt |
| Noto Emoji | Copyright 2013 Google Inc. | assets/NotoEmoji-OFL.txt |
| PromptFont | Copyright 2018-2023 Yukari Hafner; 2011-2016 Severin Meyer | assets/promptfont/LICENSE.txt |

## The zlib licence (SDL2, SDL_GameControllerDB)

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter
it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software.
   If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
   software.
3. This notice may not be removed or altered from any source distribution.
