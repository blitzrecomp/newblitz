// "This install expires in 2 days": players sideload NewBlitz.ipa with their own Apple ID (mostly with Sideloadly). A free
// Apple ID's signature lasts 7 days, a paid one's a year; after that the app won't open until it's signed again.
// Sideloadly's automatic refresh re-signs it over the same Wi-Fi (newblitz.com/start); players who never set that up
// were surprised, so the app warns them first:
//   - the launcher, from 3 days left: a line under the ROM line (launcher.cpp);
//   - a toast once a day, from 2 days left, once the game is up;
//   - Settings → General → About: "Signed until Nov 21, 2026."
// The date is the app's embedded.mobileprovision's ExpirationDate (provision_profile.mm, iOS only). Nothing anywhere
// else, or with no profile. BLITZ_TEST_EXPIRES_IN_DAYS=<n> fakes one n days away on any platform, for screenshots.
#include "install_expiry.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "recompui/config.h"
#include "ultramodern/ultramodern.hpp"
#include "blitz_platform.h"

namespace blitz {
#if defined(BLITZ_PLATFORM_IOS)
    std::optional<std::time_t> bundle_provision_expiry();  // provision_profile.mm
#endif
    void post_status_held(const std::string& msg);  // notices.cpp
    void release_status();
}

namespace {
    // The words players look for: when, that their saves are safe, and what to do.
    std::string advice() {
        return "Keep your computer on for Sideloadly's refresh, or re-sign it: newblitz.com/start";
    }

    std::string today_key() {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
        return buf;
    }
}

namespace blitz {
    std::optional<std::time_t> install_expires() {
        static std::once_flag once;
        static std::optional<std::time_t> when;
        std::call_once(once, [] {
            if (const char* fake = std::getenv("BLITZ_TEST_EXPIRES_IN_DAYS"); fake != nullptr && *fake != '\0') {
                when = std::time(nullptr) + (std::time_t)(std::atof(fake) * 86400.0);
                fprintf(stderr, "[install] BLITZ_TEST_EXPIRES_IN_DAYS=%s\n", fake);
            }
#if defined(BLITZ_PLATFORM_IOS)
            else {
                when = bundle_provision_expiry();
                if (!when) fprintf(stderr, "[install] no signing profile in the app (no expiry)\n");
            }
#endif
            if (when) {
                fprintf(stderr, "[install] signed until %s (%d days)\n", short_date(*when).c_str(),
                        calendar_days_until(std::time(nullptr), *when));
            }
        });
        return when;
    }

    std::optional<int> install_days_left() {
        std::optional<std::time_t> when = install_expires();
        if (!when) return std::nullopt;
        return calendar_days_until(std::time(nullptr), *when);
    }

    std::string install_expiry_line() {
        std::optional<int> days = install_days_left();
        if (!days || *days < 0 || *days > expiry_line_days) return {};
        return "This install expires " + expiry_when(*days) + " (your saves stay). " + advice();
    }

    std::string install_signed_until() {
        std::optional<std::time_t> when = install_expires();
        return when ? "Signed until " + short_date(*when) + "." : std::string{};
    }

    // The toast, once a day (Settings → General's hidden "install_notice_day" holds the last day it showed): once
    // the game is up, since toasts only show then (main.cpp). Held ten seconds: it's longer than a status.
    void install_expiry_update() {
        static bool done = false;
        static std::chrono::steady_clock::time_point held_at{};
        if (held_at != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() - held_at > std::chrono::seconds(10)) {
            held_at = {};
            release_status();
        }
        if (done || !ultramodern::is_game_started()) return;
        done = true;
        std::optional<int> days = install_days_left();
        if (!days || *days < 0 || *days > expiry_toast_days) return;
        recomp::config::Config& g = recompui::config::get_general_config();
        std::string today = today_key();
        if (std::get<std::string>(g.get_option_value("install_notice_day")) == today) return;
        g.update_option_value("install_notice_day", today);
        g.save_config();
        // "Eyebrow: Title" (toast.cpp): the when on top, the rest under it.
        post_status_held("This install expires " + expiry_when(*days) + ": Your saves stay. " + advice());
        held_at = std::chrono::steady_clock::now();
    }
}
