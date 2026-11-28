// blitz_host.h secure_put/get/erase on macOS and iOS: a generic password item in the Keychain (Security framework),
// readable only after the device's first unlock and never migrated to another device. Cloud Sync's code
// (save_sync.cpp). On the Mac the item is in the login keychain: a build signed differently from the one that made the
// item (an ad-hoc rebuild, an update not signed with the same identity) makes macOS ask once whether New Blitz may read
// it; save_sync.cpp keeps the code in memory after the first read, so it's asked at most once a run.
#include <cstdio>
#include <string>

#include <Security/Security.h>

#include "blitz_host.h"

namespace {
    // BRANDING: the Keychain item's service name
    constexpr const char* service = "com.blitzrecomp.newblitz.secrets";

    CFStringRef cf(const std::string& s) {
        return CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), kCFStringEncodingUTF8, false);
    }

    // The item's identity: a generic password, our service, the name as its account.
    CFMutableDictionaryRef item(const std::string& name) {
        CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef svc = cf(service), acct = cf(name);
        CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(q, kSecAttrService, svc);
        CFDictionarySetValue(q, kSecAttrAccount, acct);
        CFRelease(svc);
        CFRelease(acct);
        return q;
    }
}

namespace blitz {
    bool secure_put(const std::string& name, const std::string& value) {
        CFMutableDictionaryRef q = item(name);
        SecItemDelete(q);  // replaced whole: an update would keep an older item's access settings
        CFDataRef data = CFDataCreate(nullptr, (const UInt8*)value.data(), (CFIndex)value.size());
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        OSStatus st = SecItemAdd(q, nullptr);
        CFRelease(data);
        CFRelease(q);
        if (st != errSecSuccess) fprintf(stderr, "[secure] Keychain add failed: %d\n", (int)st);
        return st == errSecSuccess;
    }

    bool secure_get(const std::string& name, std::string& value) {
        value.clear();
        CFMutableDictionaryRef q = item(name);
        CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef out = nullptr;
        OSStatus st = SecItemCopyMatching(q, &out);
        CFRelease(q);
        if (st == errSecSuccess && out != nullptr && CFGetTypeID(out) == CFDataGetTypeID()) {
            CFDataRef d = (CFDataRef)out;
            value.assign((const char*)CFDataGetBytePtr(d), (size_t)CFDataGetLength(d));
        }
        else if (st != errSecItemNotFound) fprintf(stderr, "[secure] Keychain read failed: %d\n", (int)st);
        if (out) CFRelease(out);
        return !value.empty();
    }

    void secure_erase(const std::string& name) {
        CFMutableDictionaryRef q = item(name);
        SecItemDelete(q);
        CFRelease(q);
    }

    const char* secure_kind() { return "the Keychain"; }
}
