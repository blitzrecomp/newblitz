// iOS host services (docs/IOS_FEASIBILITY.md): the functions macos_support.mm provides on a Mac, and the scene
// delegate the Info.plist names (iOS 27 requires the scene lifecycle; SDL's windows join this scene, see
// platform/ios/sdl2-uikit-scenes.patch).
#include <filesystem>
#include "blitz_host.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <string>

@interface BlitzSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, retain) UIWindow* window;
@end

@implementation BlitzSceneDelegate
@synthesize window;
@end

namespace blitz {
    // macOS's press-and-hold accent picker; iOS has none.
    void disable_press_and_hold() {}

    // The Cocoa window's number (the Mac's screenshots); iOS has its own screenshot buttons.
    int window_number(void*) { return 0; }

    // The macOS app menu; iOS has none.
    void install_app_menu(const std::string&) {}
}

// HTTP: the same NSURLSession request as the Mac (macos_support.mm).
namespace blitz {
    // A blocking HTTP GET (call it off the main thread): true with the body when the server answers 200.
    // Rosters at launch (rosters_live.cpp).
    bool http_get(const std::string& url, const std::string& bearer, const std::string& accept, std::string& body, int& status) {
        @autoreleasepool {
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u == nil) return false;
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                           timeoutInterval:15];
            // BRANDING: HTTP User-Agent
            [req setValue:@"BlitzRecomp" forHTTPHeaderField:@"User-Agent"];
            if (!accept.empty()) [req setValue:[NSString stringWithUTF8String:accept.c_str()] forHTTPHeaderField:@"Accept"];
            if (!bearer.empty()) {
                [req setValue:[NSString stringWithFormat:@"Bearer %s", bearer.c_str()] forHTTPHeaderField:@"Authorization"];
            }
            // The data is copied out inside the handler: it's only valid there (this file isn't ARC).
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block std::string got;
            __block NSInteger code = 0;
            NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req
                completionHandler:^(NSData* d, NSURLResponse* r, NSError* e) {
                    if (e == nil && [r isKindOfClass:[NSHTTPURLResponse class]]) {
                        code = [(NSHTTPURLResponse*)r statusCode];
                        if (d != nil) got.assign((const char*)d.bytes, d.length);
                    }
                    dispatch_semaphore_signal(done);
                }];
            [task resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            dispatch_release(done);
            status = (int)code;
            if (code != 200) return false;
            body = std::move(got);
            return true;
        }
    }
}

namespace blitz {
    // Back from the screenshot sheet or the app switcher (both portrait), iOS can leave the game's scene in portrait:
    // SDL then reports a 393x852 window on a landscape screen, and the renderer and the UI disagree on its size (the
    // screen went magenta). The app is landscape-only, so ask the scene to rotate back.
    // Even with the scene already landscape, SDL can keep the portrait size it was told mid-unlock (team, 2026-10-28:
    // after unlocking, the log's last size was 393x852 until the app left again; the touch controls squished, the
    // screen went magenta). A layout pass makes SDL's view controller read its bounds again and report the real size.
    void relayout_scenes() {
        for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            for (UIWindow* window in ((UIWindowScene*)scene).windows) {
                UIView* view = window.rootViewController.view;
                [view setNeedsLayout];
                [view layoutIfNeeded];
            }
        }
    }

    void ios_keep_landscape() {
        dispatch_async(dispatch_get_main_queue(), ^{
            for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
                if (![scene isKindOfClass:[UIWindowScene class]]) continue;
                UIWindowScene* windowScene = (UIWindowScene*)scene;
                for (UIWindow* window in windowScene.windows) {
                    [window.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
                }
                if (UIInterfaceOrientationIsLandscape(windowScene.effectiveGeometry.interfaceOrientation)) continue;
                UIWindowSceneGeometryPreferencesIOS* landscape =
                    [[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape] autorelease];
                [windowScene requestGeometryUpdateWithPreferences:landscape errorHandler:^(NSError* error) {
                    NSLog(@"[ios] landscape request refused: %@", error);
                }];
            }
            relayout_scenes();
        });
        // Again once the rotation back has settled.
        for (double s : {0.3, 1.0}) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(s * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                relayout_scenes();
            });
        }
    }
}

namespace blitz {
    // The app's Documents folder (UIFileSharingEnabled: Finder and the Files app see it): a ROM put there is imported.
    std::filesystem::path rom_drop_folder() {
        const char* home = getenv("HOME");
        return std::filesystem::path(home ? home : "") / "Documents";
    }
}

namespace blitz {
    void window_created(SDL_Window*) {}
}

namespace blitz {
    void early_init() {}
}
