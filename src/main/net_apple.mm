// Downloads that show their progress, and whether the network costs by the byte, on a Mac and an iPhone (blitz_host.h;
// Add-Ons' paks, src/game/season_pak.cpp), 1.0.6. Not ARC, as macos_support.mm and platform_ios.mm.
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "blitz_host.h"
#import <Foundation/Foundation.h>
#import <Network/Network.h>

namespace {
    // The system's view of the path, kept by a monitor started on the first ask: -1 not known yet, 0 no, 1 yes.
    std::atomic<int> metered{-1};
    std::once_flag monitor_once;

    void start_monitor() {
        std::call_once(monitor_once, [] {
            nw_path_monitor_t monitor = nw_path_monitor_create();
            dispatch_queue_t queue = dispatch_queue_create("newblitz.path", DISPATCH_QUEUE_SERIAL);
            nw_path_monitor_set_queue(monitor, queue);
            nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
                // Expensive: mobile data, a phone's hotspot. Constrained: Low Data Mode. Either way the player pays.
                bool m = nw_path_is_expensive(path) || nw_path_is_constrained(path);
                int was = metered.exchange(m ? 1 : 0);
                if (was != (m ? 1 : 0)) fprintf(stderr, "[net] the network %s\n", m ? "is metered (mobile data, a hotspot or Low Data)" : "isn't metered");
            });
            nw_path_monitor_start(monitor);  // kept for the app's life
        });
    }
}

namespace blitz {
    bool network_metered() {
        start_monitor();
        // The first answer comes on the monitor's queue a moment after it starts.
        for (int i = 0; i < 20 && metered.load() < 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return metered.load() == 1;
    }

    bool http_get_progress(const std::string& url, std::string& body, int& status, const std::function<void(uint64_t, uint64_t)>& progress) {
        @autoreleasepool {
            status = 0;
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u == nil) return false;
            // 30 s with nothing arriving, not 30 s in all: a slow phone line still finishes.
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                           timeoutInterval:30];
            // BRANDING: HTTP User-Agent
            [req setValue:@"BlitzRecomp" forHTTPHeaderField:@"User-Agent"];
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
            // The task's own counters, every tenth of a second, until it's done.
            while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
                int64_t have = task.countOfBytesReceived, total = task.countOfBytesExpectedToReceive;
                if (progress) progress((uint64_t)(have > 0 ? have : 0), (uint64_t)(total > 0 ? total : 0));
            }
            dispatch_release(done);
            status = (int)code;
            if (code != 200) return false;
            if (progress) progress(got.size(), got.size());
            body = std::move(got);
            return true;
        }
    }
}
