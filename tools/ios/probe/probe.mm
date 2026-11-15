// iOS feasibility probe (docs/IOS_FEASIBILITY.md): the go/no-go checks for porting Blitz to a real iPhone, run once at
// launch, shown on screen and written to Documents/probe.txt (Files app, or `xcrun devicectl device copy from`).
//   1. The runtime's guest memory: reserve 4 GiB of address space, make 512 MiB of it writable, touch some of it
//      (librecomp's recomp.cpp: mmap(PROT_NONE) then mprotect(RW)), and how much memory iOS grants the app.
//   2. RT64's Metal needs (Plume's plume_metal.cpp checks): GPU families, argument buffers tier 2, Metal 3.
//   3. No runtime code generation: writable+executable memory should be refused; Blitz's game code is compiled ahead.
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <cerrno>
#include <cstring>
#include <string>

static std::string results;

static void line(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    results += buf;
    results += "\n";
    NSLog(@"[probe] %s", buf);
}

static double mb(uint64_t bytes) { return bytes / (1024.0 * 1024.0); }

static uint64_t resident_bytes() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}

static void run_probe() {
    char model[64] = {};
    size_t size = sizeof(model);
    sysctlbyname("hw.machine", model, &size, nullptr, 0);
    line("Blitz iOS probe | %s | iOS %s", model, UIDevice.currentDevice.systemVersion.UTF8String);
    line("Physical memory: %.0f MB | available to this app now: %.0f MB",
         mb(NSProcessInfo.processInfo.physicalMemory), mb(os_proc_available_memory()));

    // 1. Guest memory, exactly as librecomp allocates it.
    const size_t allocation_size = 4096ull * 1024 * 1024, mem_size = 512ull * 1024 * 1024;
    void* base = mmap(nullptr, allocation_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) {
        line("FAIL reserve 4 GiB address space: %s", strerror(errno));
        // How big a reservation does work? (tells us how much the runtime's layout would have to shrink)
        for (size_t gib = 3; gib >= 1; gib--) {
            void* smaller = mmap(nullptr, gib << 30, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (smaller != MAP_FAILED) {
                line("     largest that worked: %zu GiB", gib);
                munmap(smaller, gib << 30);
                break;
            }
        }
    } else {
        line("PASS reserve 4 GiB address space at %p", base);
        if (mprotect(base, mem_size, PROT_READ | PROT_WRITE) != 0) {
            line("FAIL make 512 MiB writable: %s", strerror(errno));
        } else {
            line("PASS make 512 MiB writable");
            uint64_t before = resident_bytes();
            const size_t touch = 64ull * 1024 * 1024;  // N64 RDRAM is 8 MiB; game heaps and overlays live above it
            memset(base, 0x5A, touch);
            line("PASS write 64 MiB of it (footprint %.0f -> %.0f MB)", mb(before), mb(resident_bytes()));
        }
        munmap(base, allocation_size);
    }
    line("Available to this app after: %.0f MB", mb(os_proc_available_memory()));

    // 2. Metal, as Plume decides.
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) {
        line("FAIL no Metal device");
    } else {
        line("GPU: %s | recommended working set %.0f MB", dev.name.UTF8String, mb(dev.recommendedMaxWorkingSetSize));
        line("%s GPU family Apple3 (descriptor indexing, 16K textures)", [dev supportsFamily:MTLGPUFamilyApple3] ? "PASS" : "FAIL");
        line("%s GPU family Apple6 (residency sets on iOS 18+)", [dev supportsFamily:MTLGPUFamilyApple6] ? "PASS" : "info");
        line("%s GPU family Apple7", [dev supportsFamily:MTLGPUFamilyApple7] ? "info yes" : "info no");
        line("%s GPU family Apple8", [dev supportsFamily:MTLGPUFamilyApple8] ? "info yes" : "info no");
        line("%s GPU family Apple9", [dev supportsFamily:MTLGPUFamilyApple9] ? "info yes" : "info no");
        line("%s Metal 3 (buffer device addresses)", [dev supportsFamily:MTLGPUFamilyMetal3] ? "PASS" : "FAIL");
        line("%s argument buffers tier 2", dev.argumentBuffersSupport == MTLArgumentBuffersTier2 ? "PASS" : "FAIL");
        line("info max buffer length %.0f MB", mb(dev.maxBufferLength));
    }

    // 3. Runtime code generation (expected refused: the port must not need it).
    void* page = mmap(nullptr, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
    line("%s writable+executable memory %s", page == MAP_FAILED ? "PASS" : "info",
         page == MAP_FAILED ? "refused, as expected (no JIT on iOS)" : "ALLOWED (unexpected)");
    if (page != MAP_FAILED) munmap(page, 16384);

    NSURL* docs = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSString* text = [NSString stringWithUTF8String:results.c_str()];
    [text writeToURL:[docs URLByAppendingPathComponent:@"probe.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

@interface ProbeController : UIViewController
@end

@implementation ProbeController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.02 green:0.04 blue:0.10 alpha:1];
    UITextView* text = [[UITextView alloc] initWithFrame:self.view.bounds];
    text.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    text.backgroundColor = UIColor.clearColor;
    text.textColor = [UIColor colorWithRed:0.56 green:0.94 blue:0.96 alpha:1];
    text.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    text.editable = NO;
    text.textContainerInset = UIEdgeInsetsMake(48, 20, 20, 20);
    text.text = [NSString stringWithUTF8String:results.c_str()];
    [self.view addSubview:text];
}
@end

// iOS 27 requires the scene lifecycle (an app without UIApplicationSceneManifest traps at launch): the window
// belongs to the scene.
@interface ProbeScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation ProbeScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [ProbeController new];
    [self.window makeKeyAndVisible];
}
@end

@interface ProbeApp : UIResponder <UIApplicationDelegate>
@end

@implementation ProbeApp
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    run_probe();
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([ProbeApp class]));
    }
}
