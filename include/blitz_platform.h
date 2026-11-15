// Every platform difference the app's own code decides, in one place (docs/PLATFORMS.md). The core (src/game, most of
// src/main) is the same on every device; a platform is a column of this table plus one host-services file
// (src/main/platform_<os>.cpp, declared in blitz_host.h). A new platform starts here.
//
//                        macOS  Windows  Linux  iOS  Android  PS5  PSP
//   desktop file dialog    x      x       x
//   ROM drop folder                             x      x       x    x   (a ROM put there is imported at startup; PSP: the
//                                                                        kit's <app>/rom/blitz.z64, docs/PSP_PORT.md §4.3)
//   local browser          x      x       x                             (else the Roster Room shows a LAN address)
//   file browser           x      x       x                             (Open Folder, Reveal)
//   Export ROM             x      x       x
//   Restart                x      x       x
//   engine switch          x      x                                     (Classic <-> Modern in one install)
//   screenshots (F12)      x      x                                     (phones and consoles have their own)
//   touch controls                              x      x
//   keyboard and mouse     x      x       x                  (x)        (Android: when one is plugged in)
//   two screens: none of these columns (BLITZ DS is its own build); the PSP has one 480x272 screen
#ifndef BLITZ_PLATFORM_H
#define BLITZ_PLATFORM_H

#if defined(__PSP__)
// The PSP-1000 (docs/PSP_PORT.md). Before the others: psp-gcc defines none of them, but keep it first to be sure. The name
// is case-sensitive: rosters_live.cpp's wanted() compares a manifest's targets.platforms entries with it.
#   define BLITZ_PLATFORM_PSP 1
#   define BLITZ_PLATFORM_NAME "PSP"
#elif defined(__PROSPERO__)
#   define BLITZ_PLATFORM_PS5 1
#   define BLITZ_PLATFORM_NAME "PS5"
#elif defined(__ANDROID__)
#   define BLITZ_PLATFORM_ANDROID 1
#   define BLITZ_PLATFORM_NAME "Android"
#elif defined(BLITZ_IOS)
#   define BLITZ_PLATFORM_IOS 1
#   define BLITZ_PLATFORM_NAME "iOS"
#elif defined(_WIN32)
#   define BLITZ_PLATFORM_WINDOWS 1
#   define BLITZ_PLATFORM_NAME "Windows"
#elif defined(__APPLE__)
#   define BLITZ_PLATFORM_MACOS 1
#   define BLITZ_PLATFORM_NAME "macOS"
#elif defined(__linux__)
#   define BLITZ_PLATFORM_LINUX 1
#   define BLITZ_PLATFORM_NAME "Linux"
#else
#   error "Unknown platform: add a column to include/blitz_platform.h"
#endif

#if defined(BLITZ_PLATFORM_IOS) || defined(BLITZ_PLATFORM_ANDROID)
#   define BLITZ_PLATFORM_PHONE 1
#endif
#if defined(BLITZ_PLATFORM_MACOS) || defined(BLITZ_PLATFORM_WINDOWS) || defined(BLITZ_PLATFORM_LINUX)
#   define BLITZ_PLATFORM_DESKTOP 1
#endif

// Preprocessor flags (they also decide includes and links); constexpr bools for ordinary code below.
#if defined(BLITZ_PLATFORM_DESKTOP)
#   define BLITZ_HAS_FILE_DIALOG_NFD 1
#   define BLITZ_HAS_LOCAL_BROWSER 1
#   define BLITZ_HAS_FILE_BROWSER 1
#endif
#if !defined(BLITZ_PLATFORM_DESKTOP)
#   define BLITZ_HAS_ROM_DROP_FOLDER 1
#endif
#if defined(BLITZ_PLATFORM_MACOS) || defined(BLITZ_PLATFORM_WINDOWS)
#   define BLITZ_HAS_ENGINE_SWITCH 1
#   define BLITZ_HAS_SCREEN_CAPTURE 1
#endif
#if defined(BLITZ_PLATFORM_PHONE)
#   define BLITZ_HAS_TOUCH_CONTROLS 1
#endif

namespace blitz::platform {
    inline constexpr const char* name = BLITZ_PLATFORM_NAME;
#if defined(BLITZ_PLATFORM_DESKTOP)
    inline constexpr bool desktop = true;
#else
    inline constexpr bool desktop = false;
#endif
#if defined(BLITZ_PLATFORM_PHONE)
    inline constexpr bool phone = true;
#else
    inline constexpr bool phone = false;
#endif
    // Export ROM (a .z64 for a cart or an emulator): only where the file is any use, a computer (team, 2026-10-21).
    inline constexpr bool export_rom = desktop;
    // Restart (the Settings header, Add-Ons): relaunching itself is what a computer can do (team, 2026-10-21).
    inline constexpr bool restart = desktop;
#if defined(BLITZ_HAS_FILE_BROWSER)
    inline constexpr bool file_browser = true;
#else
    inline constexpr bool file_browser = false;
#endif
#if defined(BLITZ_HAS_TOUCH_CONTROLS)
    inline constexpr bool touch_controls = true;
#else
    inline constexpr bool touch_controls = false;
#endif
}

#endif
