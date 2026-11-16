// When this install's signature runs out (iPhone and iPad, sideloaded): install_expiry.cpp. The pure parts are inline
// here so tests/provision_expiry_test.mm can check them without the app.
#ifndef BLITZ_INSTALL_EXPIRY_H
#define BLITZ_INSTALL_EXPIRY_H

#include <cmath>
#include <ctime>
#include <optional>
#include <string>

namespace blitz {
    // When the app's signature runs out: iOS reads its embedded.mobileprovision; nothing on other platforms, or with
    // no profile (the simulator). BLITZ_TEST_EXPIRES_IN_DAYS=<n> fakes one n days from now, on any platform.
    // Logs "[install] signed until <date> (<n> days)" the first time.
    std::optional<std::time_t> install_expires();
    // Calendar days left (0 today, 1 tomorrow), or nothing when there's no expiry.
    std::optional<int> install_days_left();
    // The launcher's line when 3 days or fewer are left, else empty.
    std::string install_expiry_line();
    // Settings → General → About's "Signed until 21 Nov 2026.", or empty.
    std::string install_signed_until();
    // Every UI tick (main.cpp): once a day, a toast when 2 days or fewer are left, once the game is up.
    void install_expiry_update();

    // Days from now's calendar date to when's, in local time (DST-proof: both at noon).
    inline int calendar_days_until(std::time_t now, std::time_t when) {
        std::tm a{}, b{};
#if defined(_WIN32)
        localtime_s(&a, &now);
        localtime_s(&b, &when);
#else
        localtime_r(&now, &a);
        localtime_r(&when, &b);
#endif
        a.tm_hour = b.tm_hour = 12;
        a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
        a.tm_isdst = b.tm_isdst = -1;
        return (int)std::lround(std::difftime(std::mktime(&b), std::mktime(&a)) / 86400.0);
    }

    // "today", "tomorrow", "in 3 days".
    inline std::string expiry_when(int days) {
        return days <= 0 ? "today" : days == 1 ? "tomorrow" : "in " + std::to_string(days) + " days";
    }

    // "Nov 21, 2026", in local time (American order, the team 2026-11-15).
    inline std::string short_date(std::time_t t) {
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char month[8] = {};
        std::strftime(month, sizeof month, "%b", &tm);
        return std::string(month) + " " + std::to_string(tm.tm_mday) + ", " + std::to_string(tm.tm_year + 1900);
    }

    constexpr int expiry_line_days = 3;   // the launcher's line from 3 days left
    constexpr int expiry_toast_days = 2;  // the toast from 2
}

#endif
