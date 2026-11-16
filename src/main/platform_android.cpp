// Android host services (docs/ANDROID.md): the functions blitz_host.h declares, through the Java activity
// (platform/android/java/.../BlitzActivity.java) where Android only offers them in Java: HTTPS with the system's
// certificates, and the system file picker.
#include <atomic>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <jni.h>
#include <android/log.h>

#include "blitz_host.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_system.h"
#include "SDL2/SDL_syswm.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <unistd.h>
#include "ultramodern/ultramodern.hpp"

namespace {
    // The activity's class (BlitzActivity): FindClass from a thread SDL started finds only system classes, so its
    // static methods are reached through the activity object instead.
    struct Activity {
        JNIEnv* env = nullptr;
        jobject activity = nullptr;
        jclass cls = nullptr;
        Activity() {
            env = (JNIEnv*)SDL_AndroidGetJNIEnv();
            activity = env ? (jobject)SDL_AndroidGetActivity() : nullptr;
            cls = activity ? env->GetObjectClass(activity) : nullptr;
        }
        ~Activity() {
            if (cls) env->DeleteLocalRef(cls);
            if (activity) env->DeleteLocalRef(activity);
        }
        explicit operator bool() const { return cls != nullptr; }
        jmethodID method(const char* name, const char* sig) {
            jmethodID m = env->GetStaticMethodID(cls, name, sig);
            if (!m) env->ExceptionClear();
            return m;
        }
        // A pending Java exception is reported and cleared (else the next JNI call aborts the app).
        bool failed(const char* what) {
            if (!env->ExceptionCheck()) return false;
            env->ExceptionDescribe();
            env->ExceptionClear();
            fprintf(stderr, "[android] %s threw\n", what);
            return true;
        }
    };

    jstring jstr(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

    // The file picker's answer (nativeFilesPicked, on Android's main thread) for the game thread waiting on it.
    std::mutex pick_mutex;
    std::condition_variable pick_cv;
    bool pick_done = false;
    std::vector<std::filesystem::path> picked;
}

namespace blitz {
    void disable_press_and_hold() {}
    void install_app_menu(const std::string&) {}
    int window_number(void*) { return 0; }

    // BlitzActivity.httpGet: HttpURLConnection, the system's TLS and certificate store.
    bool http_get(const std::string& url, const std::string& bearer, const std::string& accept, std::string& body, int& status) {
        body.clear();
        status = 0;
        Activity a;
        if (!a) return false;
        jmethodID m = a.method("httpGet", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[I)[B");
        if (!m) return false;
        JNIEnv* env = a.env;
        jstring ju = jstr(env, url), jb = jstr(env, bearer), ja = jstr(env, accept);
        jintArray js = env->NewIntArray(1);
        jbyteArray data = (jbyteArray)env->CallStaticObjectMethod(a.cls, m, ju, jb, ja, js);
        bool threw = a.failed("httpGet");
        jint code = 0;
        env->GetIntArrayRegion(js, 0, 1, &code);
        status = code;
        if (data && !threw) {
            jsize n = env->GetArrayLength(data);
            body.resize((size_t)n);
            if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(body.data()));
            env->DeleteLocalRef(data);
        }
        for (jobject o : {(jobject)ju, (jobject)jb, (jobject)ja, (jobject)js}) env->DeleteLocalRef(o);
        if (status != 200) body.clear();
        return status == 200;
    }

    // BlitzActivity.httpPost: the same HttpURLConnection. Crash reports (crash_report.cpp).
    bool http_post(const std::string& url, const std::string& content_type, const std::string& body, int& status) {
        status = 0;
        Activity a;
        if (!a) return false;
        jmethodID m = a.method("httpPost", "(Ljava/lang/String;Ljava/lang/String;[B)I");
        if (!m) return false;
        JNIEnv* env = a.env;
        jstring ju = jstr(env, url), jt = jstr(env, content_type);
        jbyteArray jb = env->NewByteArray((jsize)body.size());
        if (!body.empty()) env->SetByteArrayRegion(jb, 0, (jsize)body.size(), reinterpret_cast<const jbyte*>(body.data()));
        jint code = env->CallStaticIntMethod(a.cls, m, ju, jt, jb);
        bool threw = a.failed("httpPost");
        for (jobject o : {(jobject)ju, (jobject)jt, (jobject)jb}) env->DeleteLocalRef(o);
        status = threw ? 0 : code;
        return status >= 200 && status < 300;
    }

    // The app's external files folder (Android/data/<package>/files): adb push and file managers reach it.
    std::filesystem::path rom_drop_folder() {
        const char* p = SDL_AndroidGetExternalStoragePath();
        return p ? std::filesystem::path(p) : std::filesystem::path{};
    }
}

extern "C" void plume_android_set_window(ANativeWindow* window);  // lib/rt64 Plume (rt64 patch 0007)

namespace {
    ANativeWindow* sdl_native_window(SDL_Window* window) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        return window && SDL_GetWindowWMInfo(window, &info) ? info.info.android.window : nullptr;
    }

    // The game's surface as Android has it now, held (ANativeWindow_fromSurface acquires it): nullptr while there's none, or
    // one on its way out. SDL's own pointer (sdl_native_window) is SDL's to release when the surface goes, on Java's thread,
    // so a copy of it can be freed memory by the time it's used (the team's RG DS, 2026-11-15: the renderer made its swap chain on a
    // window minimized a moment before, and crashed in RefBase::incStrong).
    ANativeWindow* acquire_current_window() {
        Activity a;
        if (!a) return nullptr;
        jmethodID m = a.method("getNativeSurface", "()Landroid/view/Surface;");  // SDLActivity's
        if (!m) return nullptr;
        jobject surface = a.env->CallStaticObjectMethod(a.cls, m);
        if (a.failed("getNativeSurface") || surface == nullptr) return nullptr;
        ANativeWindow* w = ANativeWindow_fromSurface(a.env, surface);
        a.env->DeleteLocalRef(surface);
        if (w != nullptr && (ANativeWindow_getWidth(w) <= 0 || ANativeWindow_getHeight(w) <= 0)) {  // abandoned
            ANativeWindow_release(w);
            w = nullptr;
        }
        return w;
    }

    // The window Plume follows (plume_android_set_window keeps only the pointer), held here until the next one replaces it, so
    // it can't be freed under Plume before Plume takes its own reference.
    std::mutex plume_window_mutex;
    ANativeWindow* plume_window = nullptr;
    void set_plume_window(ANativeWindow* held) {
        std::lock_guard lock{plume_window_mutex};
        plume_android_set_window(held);
        if (plume_window != nullptr) ANativeWindow_release(plume_window);
        plume_window = held;
    }

    // Android takes the app's window away in the background and gives it a new one back: the renderer follows. A low-memory
    // warning goes in the log.
    int on_app_event(void* window, SDL_Event* e) {
        switch (e->type) {
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_DIDENTERBACKGROUND:
                set_plume_window(nullptr);
                fprintf(stderr, "[android] background\n");
                break;
            case SDL_APP_DIDENTERFOREGROUND: {
                ANativeWindow* w = acquire_current_window();
                if (w == nullptr) {  // no surface yet: SDL's, as before (Plume takes its own reference when it uses it)
                    w = sdl_native_window(static_cast<SDL_Window*>(window));
                    if (w != nullptr) ANativeWindow_acquire(w);
                }
                set_plume_window(w);
                fprintf(stderr, "[android] foreground%s\n", w == nullptr ? ", no surface yet" : "");
                break;
            }
            case SDL_APP_LOWMEMORY:  // SDL's onLowMemory; BlitzActivity logs onTrimMemory's levels
                fprintf(stderr, "[pressure] SDL low memory\n");
                break;
            default:
                break;
        }
        return 1;
    }
}

namespace blitz {
    void window_created(SDL_Window* window) {
        ANativeWindow* w = sdl_native_window(window);
        if (w != nullptr) ANativeWindow_acquire(w);
        set_plume_window(w);
        SDL_AddEventWatch(on_app_event, window);
    }

    // The window the renderer is made on (main.cpp's create_render_context), on the graphics thread: the game's surface as it
    // is now, not the one create_window saw, which Android may have taken away since (backgrounded, minimized). Waits for one
    // while there's none. Held for good: the renderer keeps the pointer.
    ANativeWindow* render_window(ANativeWindow* from_create_window) {
        static ANativeWindow* held = nullptr;
        Uint32 began = SDL_GetTicks();
        bool said = false;
        ANativeWindow* w;
        while ((w = acquire_current_window()) == nullptr) {
            if (!said) {
                fprintf(stderr, "[android] no surface for the renderer yet (the app is in the background): waiting for one\n");
                said = true;
            }
            SDL_Delay(50);
        }
        if (said) fprintf(stderr, "[android] the renderer's surface came back after %u ms\n", (unsigned)(SDL_GetTicks() - began));
        if (w != from_create_window) fprintf(stderr, "[android] the renderer starts on the current surface, not create_window's\n");
        if (held != nullptr) ANativeWindow_release(held);
        held = w;
        return w;
    }
}

// The frontend's file dialog on Android (lib/RecompFrontend file.cpp, frontend patch): the system picker
// (ACTION_OPEN_DOCUMENT), whose picks BlitzActivity copies into the app's cache. Blocks, like a desktop dialog.
std::vector<std::filesystem::path> blitz_android_pick_files(bool multiple) {
    {
        std::lock_guard lock{pick_mutex};
        pick_done = false;
        picked.clear();
    }
    {
        Activity a;
        if (!a) return {};
        jmethodID m = a.method("pickFiles", "(Z)V");
        if (!m) return {};
        a.env->CallStaticVoidMethod(a.cls, m, (jboolean)multiple);
        if (a.failed("pickFiles")) return {};
    }
    std::unique_lock lock{pick_mutex};
    pick_cv.wait(lock, [] { return pick_done; });
    return picked;
}

// ---- BLITZ DS (docs/BLITZ_DS.md): the second screen's surface, from BlitzActivity's Presentation ----------------------

extern "C" void plume_android_second_window(ANativeWindow* window);  // lib/rt64 Plume

namespace blitz {
    void ds_second_screen(void* a, void* b, int w, int h);  // ds_view.cpp
    void ds_set_swapped(bool on);
    void ds_tap(int screen, float x, float y);
    bool log_is_up();  // app_log.cpp

    void host_first_frame() {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("firstFrame", "()V");
        if (!m) return;
        a.env->CallStaticVoidMethod(a.cls, m);
        a.failed("firstFrame");
    }

    void host_hide_splash() {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("hideSplash", "()V");
        if (!m) return;
        a.env->CallStaticVoidMethod(a.cls, m);
        a.failed("hideSplash");
    }

    void host_set_potato_two_screens(bool on) {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("setPotatoTwoScreens", "(Z)V");
        if (!m) return;
        a.env->CallStaticVoidMethod(a.cls, m, (jboolean)on);
        a.failed("setPotatoTwoScreens");
    }

    void host_set_potato(bool on) {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("setPotato", "(Z)V");
        if (!m) return;
        a.env->CallStaticVoidMethod(a.cls, m, (jboolean)on);
        a.failed("setPotato");
    }

    void host_set_swap_screens(bool swap) {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("setSwapScreens", "(Z)V");
        if (!m) return;
        a.env->CallStaticVoidMethod(a.cls, m, (jboolean)swap);
        a.failed("setSwapScreens");
    }

    // The hotkey + Select (capture.cpp): both screens' pictures, saved to the gallery by the activity.
    void host_screenshot() {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("takeScreenshot", "()V");
        if (!m) return;
        a.env->CallStaticVoidMethod(a.cls, m);
        a.failed("takeScreenshot");
    }

    // A file this app made (a replay), copied into the gallery: Pictures/New Blitz, or Movies/New Blitz for a video.
    void host_add_to_gallery(const std::filesystem::path& file, const char* mime) {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("addToGallery", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (!m) return;
        jstring p = a.env->NewStringUTF(file.string().c_str()), t = a.env->NewStringUTF(mime);
        a.env->CallStaticVoidMethod(a.cls, m, p, t);
        a.failed("addToGallery");
        a.env->DeleteLocalRef(p);
        a.env->DeleteLocalRef(t);
    }

    void host_send_logs(const std::filesystem::path& zip) {
        Activity a;
        if (!a) return;
        jmethodID m = a.method("sendLogs", "(Ljava/lang/String;)V");
        if (!m) return;
        jstring s = a.env->NewStringUTF(zip.string().c_str());
        a.env->CallStaticVoidMethod(a.cls, m, s);
        a.failed("sendLogs");
        a.env->DeleteLocalRef(s);
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeDsSurface(JNIEnv* env, jclass, jobject surface, jint w, jint h) {
    static ANativeWindow* current = nullptr;
    ANativeWindow* next = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (next == current) {
        if (next != nullptr) {
            blitz::ds_second_screen(next, nullptr, w, h);  // the same surface at a new size: the picture's shape follows
            ANativeWindow_release(next);
        }
        return;
    }
    // The old one goes first: this returns once the renderer has let go, so Android may destroy it.
    if (current != nullptr) blitz::ds_second_screen(nullptr, nullptr, 0, 0);
    plume_android_second_window(next);
    if (current != nullptr) ANativeWindow_release(current);
    current = next;
    if (next != nullptr) blitz::ds_second_screen(next, nullptr, w, h);
}

namespace blitz { extern std::atomic<bool> digital_trigger[2]; }  // controllers.cpp
extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeDigitalTrigger(JNIEnv*, jclass, jint side, jboolean down) {
    blitz::digital_trigger[side != 0 ? 1 : 0] = down != JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeDsTap(JNIEnv*, jclass, jfloat x, jfloat y) {
    blitz::ds_tap(1, x, y);
}

extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeDsFirstIsUpper(JNIEnv*, jclass, jboolean upper) {
    blitz::ds_set_swapped(upper != JNI_FALSE);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeLogUp(JNIEnv*, jclass) {
    return blitz::log_is_up() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeLog(JNIEnv* env, jclass, jstring line) {
    const char* c = env->GetStringUTFChars(line, nullptr);
    fprintf(stderr, "%s\n", c);
    env->ReleaseStringUTFChars(line, c);
}

namespace blitz { void post_status(const std::string& msg); }  // notices.cpp: a toast
extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeStatus(JNIEnv* env, jclass, jstring message) {
    const char* c = env->GetStringUTFChars(message, nullptr);
    blitz::post_status(c);
    env->ReleaseStringUTFChars(message, c);
}

extern "C" void blitz_app_background(void);  // savestate.cpp: paused, the Auto slot, then the game holds
extern "C" void blitz_app_foreground(void);
// The activity's onPause / onResume, straight away: SDL's own background event reaches the game ~3 s later than this.
extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeAppBackground(JNIEnv*, jclass) { blitz_app_background(); }
extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeAppForeground(JNIEnv*, jclass) { blitz_app_foreground(); }

namespace blitz { void toggle_menu(); }  // controllers.cpp
// Back pressed (BlitzActivity.deviceBack): Settings opens, or closes.
extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeToggleMenu(JNIEnv*, jclass) {
    blitz::toggle_menu();
}

// Back held 5 s (BlitzActivity.deviceBack): quit as the quit prompt's confirm does; before the game's up, at once.
extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeBlitzQuit(JNIEnv*, jclass) {
    if (ultramodern::is_game_started()) ultramodern::quit();
    else _exit(0);
}

extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_newblitz_BlitzActivity_nativeFilesPicked(JNIEnv* env, jclass, jobjectArray paths) {
    std::vector<std::filesystem::path> got;
    jsize n = paths ? env->GetArrayLength(paths) : 0;
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)env->GetObjectArrayElement(paths, i);
        if (!s) continue;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) got.emplace_back(c);
        env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
    }
    std::lock_guard lock{pick_mutex};
    picked = std::move(got);
    pick_done = true;
    pick_cv.notify_all();
}

namespace blitz {
    void early_init() {}
}
