// iOS host services (docs/IOS_FEASIBILITY.md): the functions macos_support.mm provides on a Mac, and the scene
// delegate the Info.plist names (iOS 27 requires the scene lifecycle; SDL's windows join this scene, see
// platform/ios/sdl2-uikit-scenes.patch).
#include <filesystem>
#include "blitz_host.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

// A file opened into the app from another (Files' Share → New Blitz, Open In; Info.plist CFBundleDocumentTypes). With
// the scene lifecycle, iOS hands it to the scene delegate, never to SDL's app delegate (whose openURL would have made
// an SDL_DROPFILE): at a cold start in the connection options, later in openURLContexts. The file is copied into tmp
// now, while the security-scoped URL is good, and the UI tick takes it (blitz::ios_take_opened_files, firstrun.cpp).
static std::mutex opened_mutex;
static std::vector<std::filesystem::path> opened_files;

static void blitz_take_opened_url(NSURL* url) {
    @autoreleasepool {
        if (url == nil || ![url isFileURL]) {
            NSLog(@"[rom] opened from another app: not a file (%@)", url);
            return;
        }
        NSString* name = [url lastPathComponent] ?: @"file";
        fprintf(stderr, "[rom] opened from another app: %s\n", [name UTF8String]);
        // In place (LSSupportsOpeningDocumentsInPlace): a security-scoped URL to the file where it is, maybe in iCloud
        // and not downloaded yet; the coordinated read waits for it. A copy sent by an app that doesn't share in place
        // lands in Documents/Inbox, which is ours.
        BOOL scoped = [url startAccessingSecurityScopedResource];
        NSString* folder = [NSTemporaryDirectory() stringByAppendingPathComponent:@"opened"];
        [[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL* dest = [NSURL fileURLWithPath:[folder stringByAppendingPathComponent:name]];
        __block BOOL copied = NO;
        __block NSError* copy_error = nil;
        NSError* coord_error = nil;
        NSFileCoordinator* coordinator = [[[NSFileCoordinator alloc] initWithFilePresenter:nil] autorelease];
        [coordinator coordinateReadingItemAtURL:url options:NSFileCoordinatorReadingWithoutChanges error:&coord_error
                                     byAccessor:^(NSURL* readable) {
            [[NSFileManager defaultManager] removeItemAtURL:dest error:nil];
            NSError* e = nil;
            copied = [[NSFileManager defaultManager] copyItemAtURL:readable toURL:dest error:&e];
            if (!copied) copy_error = [e retain];
        }];
        if (scoped) [url stopAccessingSecurityScopedResource];
        // The Inbox copy isn't needed once it's in tmp (16 MB each).
        NSString* inbox = [[NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject]
                           stringByAppendingPathComponent:@"Inbox"].stringByResolvingSymlinksInPath;
        if (copied && inbox != nil && [[[url URLByResolvingSymlinksInPath] path] hasPrefix:[inbox stringByAppendingString:@"/"]]) {
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        }
        // iOS makes Documents/Inbox when a file is opened into the app; an empty one would show in Files beside the
        // player's ROMs (rmdir leaves it if anything is in it).
        if (inbox != nil) rmdir([inbox fileSystemRepresentation]);
        if (!copied) {
            NSError* e = copy_error ?: coord_error;
            fprintf(stderr, "[rom] opened from another app: couldn't copy %s (scoped=%d): %s\n", [name UTF8String], (int)scoped,
                    e ? [[e localizedDescription] UTF8String] : "unknown");
            [copy_error release];
            return;
        }
        std::lock_guard lock{opened_mutex};
        opened_files.emplace_back([[dest path] UTF8String]);
    }
}

@interface BlitzSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, retain) UIWindow* window;
@end

@implementation BlitzSceneDelegate
@synthesize window;

- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)connectionOptions {
    for (UIOpenURLContext* context in connectionOptions.URLContexts) blitz_take_opened_url(context.URL);
}

- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)URLContexts {
    for (UIOpenURLContext* context in URLContexts) blitz_take_opened_url(context.URL);
}
@end

namespace blitz {
    // Files opened into the app since the last call (firstrun.cpp's UI tick imports them as Load ROM would).
    std::vector<std::filesystem::path> ios_take_opened_files() {
        std::lock_guard lock{opened_mutex};
        std::vector<std::filesystem::path> files;
        files.swap(opened_files);
        return files;
    }
}

namespace blitz {
    // macOS's press-and-hold accent picker; iOS has none.
    void disable_press_and_hold() {}

    // The Cocoa window's number (the Mac's screenshots); iOS has its own screenshot buttons.
    int window_number(void*) { return 0; }

    // The macOS app menu; iOS has none.
    void install_app_menu(const std::string&) {}

    // The launcher's "RECOMPILED FOR IPAD" (launcher.cpp).
    bool is_ipad() { return [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad; }
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
    // A blocking HTTP POST (call it off the main thread): true on a 2xx answer. Crash reports (crash_report.cpp).
    bool http_post(const std::string& url, const std::string& content_type, const std::string& body, int& status) {
        @autoreleasepool {
            status = 0;
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u == nil) return false;
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                           timeoutInterval:20];
            [req setHTTPMethod:@"POST"];
            // BRANDING: HTTP User-Agent
            [req setValue:@"BlitzRecomp" forHTTPHeaderField:@"User-Agent"];
            [req setValue:[NSString stringWithUTF8String:content_type.c_str()] forHTTPHeaderField:@"Content-Type"];
            [req setHTTPBody:[NSData dataWithBytes:body.data() length:body.size()]];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block NSInteger code = 0;
            NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req
                completionHandler:^(NSData*, NSURLResponse* r, NSError* e) {
                    if (e == nil && [r isKindOfClass:[NSHTTPURLResponse class]]) code = [(NSHTTPURLResponse*)r statusCode];
                    dispatch_semaphore_signal(done);
                }];
            [task resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            dispatch_release(done);
            status = (int)code;
            return code >= 200 && code < 300;
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

    // Add-Ons' Open Folder (blitz_host.h): the Files app at that Documents folder, by its shareddocuments:// address (the
    // Files app's own; it opens an app's shared folder as Files → On My iPhone → New Blitz shows it). The app folder itself
    // (Application Support) isn't one Files can show: a ROM or a .nbpak pak put in Documents goes in at the next start.
    void open_app_folder_in_files(std::function<void(bool)> done) {
        std::error_code ec;
        std::filesystem::path docs = rom_drop_folder();
        std::filesystem::create_directories(docs, ec);
        NSString* path = [NSString stringWithUTF8String:docs.string().c_str()];
        NSString* escaped = [path stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLPathAllowedCharacterSet]];
        NSURL* url = escaped ? [NSURL URLWithString:[@"shareddocuments://" stringByAppendingString:escaped]] : nil;
        if (url == nil) {
            if (done) done(false);
            return;
        }
        auto* cb = new std::function<void(bool)>(std::move(done));
        [url retain];
        dispatch_async(dispatch_get_main_queue(), ^{
            [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:^(BOOL ok) {
                fprintf(stderr, "[folder] Files app at %s: %s\n", [[url absoluteString] UTF8String], ok ? "opened" : "refused");
                if (*cb) (*cb)(ok);
                delete cb;
            }];
            [url release];
        });
    }
}

namespace blitz {
    void window_created(SDL_Window*) {}
    bool relaunch_app() { return false; }  // blitz_host.h: Android only
}

namespace blitz {
    void early_init() {}
}
