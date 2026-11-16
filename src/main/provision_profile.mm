// The signing profile inside a sideloaded iPhone or iPad app (embedded.mobileprovision): when its signature runs out.
// A free Apple ID signs for 7 days, a paid one for a year; after that the app won't open until it's signed again
// (install_expiry.cpp warns before then). Built for iOS only (CMakeLists.txt); tools/ios/provision_check.mm runs the
// same parsing on the Mac against Xcode's profiles.
//
// The file is a CMS (PKCS#7) envelope around an XML plist. Nothing here checks the signature: the plist is found by
// its <?xml ... </plist> span in the bytes and read with NSPropertyListSerialization, for ExpirationDate only.
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#import <Foundation/Foundation.h>

namespace blitz {
    // ExpirationDate from a profile's bytes, or nothing when there's no plist or no date in it.
    std::optional<std::time_t> provision_expiry(const std::string& bytes) {
        std::string_view all{bytes};
        size_t start = all.find("<?xml");
        if (start == std::string_view::npos) return std::nullopt;
        constexpr std::string_view close = "</plist>";
        size_t end = all.find(close, start);
        if (end == std::string_view::npos) return std::nullopt;
        end += close.size();
        @autoreleasepool {
            NSData* xml = [NSData dataWithBytes:bytes.data() + start length:end - start];
            NSError* error = nil;
            id plist = [NSPropertyListSerialization propertyListWithData:xml options:NSPropertyListImmutable format:nil error:&error];
            if (![plist isKindOfClass:[NSDictionary class]]) return std::nullopt;
            id date = [(NSDictionary*)plist objectForKey:@"ExpirationDate"];
            if (![date isKindOfClass:[NSDate class]]) return std::nullopt;
            return (std::time_t)[(NSDate*)date timeIntervalSince1970];
        }
    }

    // This app's own profile (iOS): embedded.mobileprovision at the top of the bundle. Nothing without one (the
    // simulator, a build Xcode didn't sign with a profile).
    std::optional<std::time_t> bundle_provision_expiry() {
        @autoreleasepool {
            NSString* path = [[NSBundle mainBundle] pathForResource:@"embedded" ofType:@"mobileprovision"];
            if (path == nil) return std::nullopt;
            NSData* data = [NSData dataWithContentsOfFile:path];
            if (data == nil || data.length == 0) return std::nullopt;
            return provision_expiry(std::string((const char*)data.bytes, data.length));
        }
    }
}
