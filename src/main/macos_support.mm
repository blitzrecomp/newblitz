// macOS-only setup that needs Objective-C.
#include <filesystem>
#include "blitz_host.h"
#import <Foundation/Foundation.h>

namespace blitz {
    // Holding a key in a window that accepts text brings up macOS's accent picker ("press and hold"),
    // which swallows the held key. SDL keeps text input on (the settings UI has text fields), so turn
    // the picker off for this app instead; a held key then just repeats, which the game ignores.
    // registerDefaults only sets a fallback for this run; a value the user set with `defaults` wins.
    void disable_press_and_hold() {
        @autoreleasepool {
            [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"ApplePressAndHoldEnabled": @NO }];
        }
    }
}

#import <AppKit/AppKit.h>

namespace blitz {
    // The window server's number for an NSWindow (screencapture -l takes it).
    int window_number(void* nswindow) {
        return nswindow ? (int)[(__bridge NSWindow*)nswindow windowNumber] : 0;
    }
}

#include "SDL2/SDL.h"
#include <string>

// The app menu. SDL builds one only when it creates NSApp itself, and here AppKit is already up by then, so
// the app had no menu bar and Cmd+Q did nothing. Quit sends the same SDL_QUIT as the window's close button
// (the quit prompt: Save & Quit during a game).
@interface BlitzMenuTarget : NSObject
- (void)quit:(id)sender;
@end
@implementation BlitzMenuTarget
- (void)quit:(id)sender {
    SDL_Event e = {};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}
@end

namespace blitz {
    void install_app_menu(const std::string& app_name) {
        auto install = [name = app_name]() {
            @autoreleasepool {
                if (NSApp == nil || [NSApp mainMenu] != nil) return;
                static BlitzMenuTarget* target = [[BlitzMenuTarget alloc] init];
                NSString* title = [NSString stringWithUTF8String:name.c_str()];
                NSMenu* bar = [[NSMenu alloc] init];

                NSMenuItem* app_item = [[NSMenuItem alloc] init];
                NSMenu* app_menu = [[NSMenu alloc] initWithTitle:title];
                [app_menu addItemWithTitle:[@"Hide " stringByAppendingString:title] action:@selector(hide:) keyEquivalent:@"h"];
                NSMenuItem* others = [app_menu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
                [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
                [app_menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
                [app_menu addItem:[NSMenuItem separatorItem]];
                NSMenuItem* quit = [app_menu addItemWithTitle:[@"Quit " stringByAppendingString:title] action:@selector(quit:) keyEquivalent:@"q"];
                [quit setTarget:target];
                [app_item setSubmenu:app_menu];
                [bar addItem:app_item];

                NSMenuItem* window_item = [[NSMenuItem alloc] init];
                NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
                [window_menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
                [window_item setSubmenu:window_menu];
                [bar addItem:window_item];

                [NSApp setMainMenu:bar];
                [NSApp setWindowsMenu:window_menu];
            }
        };
        if ([NSThread isMainThread]) install();
        else dispatch_async(dispatch_get_main_queue(), ^{ install(); });
    }
}

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
    // A computer has file dialogs: no drop folder.
    std::filesystem::path rom_drop_folder() { return {}; }
    void open_app_folder_in_files(std::function<void(bool)> done) { if (done) done(false); }  // Finder: os_shell.cpp
}

namespace blitz {
    void window_created(SDL_Window*) {}
    bool relaunch_app() { return false; }  // blitz_host.h: Android only
}

namespace blitz {
    void early_init() {}
}
