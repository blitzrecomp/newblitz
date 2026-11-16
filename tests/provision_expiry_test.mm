// src/main/provision_profile.mm (the iPhone's "signed until" date) and install_expiry.h's day counting, on the Mac.
// tests/test_provision_expiry.py builds and runs this. With no arguments: checks on made-up profiles. With profile
// paths (e.g. ~/Library/Developer/Xcode/UserData/Provisioning Profiles/*.mobileprovision): prints each one's
// ExpirationDate and nothing else, since a real profile holds team IDs and names.
//   c++ -std=c++20 -fobjc-arc -Isrc/main tests/provision_expiry_test.mm src/main/provision_profile.mm \
//       -framework Foundation -o /tmp/provision_expiry_test && /tmp/provision_expiry_test
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "install_expiry.h"

namespace blitz {
    std::optional<std::time_t> provision_expiry(const std::string& bytes);
}

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static std::string utc(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    return buf;
}

// A profile as Apple ships one: DER bytes (with NULs) around the XML plist, more after it.
static std::string fake_profile(const std::string& dict) {
    std::string der = std::string("\x30\x82\x2a\x1b\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02\xa0\x00\x00", 18);
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n" + dict + "</dict>\n</plist>";
    std::string tail = std::string("\xa0\x82\x0d\x00\x00\x30\x82\x04</plist", 13);  // a stray "</plist" can't confuse it
    return der + xml + tail;
}

static void self_test() {
    // 2026-11-22T10:00:00Z
    auto t = blitz::provision_expiry(fake_profile(
        "<key>AppIDName</key><string>X</string>\n"
        "<key>CreationDate</key><date>2026-11-15T10:00:00Z</date>\n"
        "<key>ExpirationDate</key><date>2026-11-22T10:00:00Z</date>\n"));
    check(t.has_value(), "a profile's ExpirationDate is read");
    check(t && *t == 1795341600, "ExpirationDate is 2026-11-22 10:00 UTC");
    check(!blitz::provision_expiry(fake_profile("<key>CreationDate</key><date>2026-11-15T10:00:00Z</date>\n")),
          "no ExpirationDate: nothing");
    check(!blitz::provision_expiry(fake_profile("<key>ExpirationDate</key><string>soon</string>\n")),
          "an ExpirationDate that isn't a date: nothing");
    check(!blitz::provision_expiry(std::string("\x30\x82\x00\x00 no plist here", 18)), "no plist: nothing");
    check(!blitz::provision_expiry(""), "empty: nothing");
    check(!blitz::provision_expiry("<?xml version=\"1.0\"?><plist><dict>"), "a cut-off plist: nothing");

    // Calendar days, in local time.
    std::time_t now = std::time(nullptr);
    std::tm noon{};
    localtime_r(&now, &noon);
    noon.tm_hour = 12;
    noon.tm_min = noon.tm_sec = 0;
    noon.tm_isdst = -1;
    std::time_t n = std::mktime(&noon);
    check(blitz::calendar_days_until(n, n + 3600) == 0, "later today: 0 days");
    check(blitz::calendar_days_until(n, n + 86400) == 1, "this time tomorrow: 1 day");
    check(blitz::calendar_days_until(n, n + 11 * 3600 + 59 * 60) == 0, "23:59 today: 0 days");
    check(blitz::calendar_days_until(n, n + 12 * 3600 + 60) == 1, "00:01 tomorrow: 1 day");
    check(blitz::calendar_days_until(n, n + 7 * 86400) == 7, "a free Apple ID's week: 7 days");
    check(blitz::calendar_days_until(n, n - 86400) == -1, "yesterday: -1");
    check(blitz::expiry_when(0) == "today", "today");
    check(blitz::expiry_when(1) == "tomorrow", "tomorrow");
    check(blitz::expiry_when(3) == "in 3 days", "in 3 days");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        self_test();
        printf(failures == 0 ? "ok\n" : "%d failed\n", failures);
        return failures == 0 ? 0 : 1;
    }
    for (int i = 1; i < argc; i++) {
        std::ifstream in(argv[i], std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto t = blitz::provision_expiry(bytes);
        printf("profile %d: %s\n", i, t ? ("expires " + utc(*t)).c_str() : "no ExpirationDate");
    }
    return 0;
}
