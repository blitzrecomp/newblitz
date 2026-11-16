package com.blitzrecomp.newblitz;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.OpenableColumns;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.GZIPOutputStream;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;
import org.libsdl.app.SDLSurface;

/**
 * The game's activity (docs/ANDROID.md): SDL's activity, plus what the native game needs from Java.
 *  - Its resources (the APK's assets/res: the UI, fonts, packs, Modern's art) unpacked to files/res on the first
 *    launch and after each update, since the game reads ordinary files; BLITZ_RESOURCES names the folder.
 *  - The app folder (settings, saves, the stored ROM, add-ons) in the app's external files folder, which adb and
 *    file managers reach: APP_FOLDER_PATH. A ROM put there is imported at startup (main.cpp).
 *  - HTTPS with the system's certificates (httpGet) and the system file picker (pickFiles), called from native code
 *    (src/main/platform_android.cpp).
 */
public class BlitzActivity extends SDLActivity {
    private static final String TAG = "BlitzRecomp";
    private static final int PICK_FILES = 0xB17;
    private static BlitzActivity current;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "BlitzRecompiled" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        current = this;
        readStamp();
        keepLogOfDeadRun();
        startEarlyLog();
        safeStartCheck();
        try {
            File res = new File(getFilesDir(), "res");
            unpackResources(res);
            File app = getExternalFilesDir(null);
            if (app == null) app = new File(getFilesDir(), "app");
            app.mkdirs();
            Os.setenv("BLITZ_RESOURCES", res.getAbsolutePath(), true);
            Os.setenv("APP_FOLDER_PATH", app.getAbsolutePath(), true);
            Os.setenv("HOME", getFilesDir().getAbsolutePath(), true);
            // Tests: adb shell am start ... --es blitz_env "NAME=value;NAME=value" (docs/BLITZ_DS.md).
            // This activity is exported, so only these, and a file only in the app's own folder.
            String env = getIntent() != null ? getIntent().getStringExtra("blitz_env") : null;
            if (env != null) {
                java.util.Set<String> allowed = new java.util.HashSet<>(java.util.Arrays.asList(
                        "BLITZ_INPUT_LIVE", "BLITZ_VIRTUAL_PADS", "BLITZ_QUIET_NOTICES", "BLITZ_ADVANCED", "BLITZ_DS_PROFILE", "BLITZ_DS_FPS_FLOOR", "BLITZ_DS_ACTIVITY"));
                for (String pair : env.split(";")) {
                    int eq = pair.indexOf('=');
                    if (eq <= 0 || !allowed.contains(pair.substring(0, eq))) continue;
                    String value = pair.substring(eq + 1);
                    if (pair.startsWith("BLITZ_INPUT_LIVE") && !new File(value).getCanonicalPath().startsWith(app.getCanonicalPath() + "/")) continue;
                    Os.setenv(pair.substring(0, eq), value, true);
                }
            }
            // Tests over adb: <app folder>/test_env.txt, one BLITZ_NAME=value a line, for this and every later launch until it's
            // deleted. Only the app and adb can write there.
            File testEnv = new File(app, "test_env.txt");
            if (testEnv.isFile()) {
                try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(testEnv))) {
                    for (String line; (line = r.readLine()) != null; ) {
                        line = line.trim();
                        int eq = line.indexOf('=');
                        if (!line.startsWith("BLITZ_") || eq <= 0) continue;
                        Os.setenv(line.substring(0, eq), line.substring(eq + 1), true);
                        report("[test] " + line);
                    }
                }
            }
            // In its own try: Potato Mode, safe start and the recipe below can't be lost to testing that didn't come up.
            try { startRemote(app); }
            catch (Exception e) { Log.e(TAG, "remote testing didn't start", e); }
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            ((ActivityManager) getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(memory);
            if ((memory.totalMem >> 20) < 2500) Os.setenv("BLITZ_UI_NO_MSAA", "1", true);
            // GammaOS on an RG DS, by the image's generic name: any handheld on this image gets it. BlitzLauncher.gammaOs() is the
            // closer key, but a wrong pad has no fallback, so this stays until a run's own log shows that name on this unit.
            if ("tdgsi_arm64_ab".equals(Build.DEVICE)) {
                Os.setenv("BLITZ_NINTENDO_PAD", "1", true);
                Os.setenv("BLITZ_SHOULDERS_ARE_TRIGGERS", "1", true);
                Os.setenv("BLITZ_ONE_STICK", "1", true);  // L3 is a hotkey too there (controllers.cpp hotkey_down)
            }
            if (BlitzLauncher.lean(this)) {
                Os.setenv("BLITZ_LEAN", "1", true);
                if (safeStart) Os.setenv("BLITZ_SAFE_START", "1", true);  // ds_view.cpp's 1x, config.cpp's Full; saved settings stay
                // A strong device in Potato Mode: its first four cores, the slow ones on a Snapdragon, stand in for a weak chip.
                if ((memory.totalMem >> 20) >= 1500) Os.setenv("BLITZ_POTATO_CORES", "1", true);
            }
            BlitzRecipe.forThisRun(this, safeStart, getIntent() != null && getIntent().getBooleanExtra(BlitzRecipe.LAUNCHER_ASKED, false));
            // An upper screen with no touch of its own (an RG DS's): its Settings and Replay come to the lower one (ds_touch.cpp).
            // A mapper's virtual panel could count as a second, so Potato Mode doesn't ask. The stacked test's two screens are one
            // panel, both of it touchable.
            int panels = touchPanels();
            if (BlitzLauncher.lean(this) || (panels < 2 && !stacked())) Os.setenv("BLITZ_ONE_TOUCH", String.valueOf(panels), true);
        } catch (Exception e) {
            Log.e(TAG, "setup failed", e);
        }
        hwui(this, getWindow());  // before SDL sets the content view
        super.onCreate(savedInstanceState);
        if (mLayout != null) {
            mLayout.addView(makeSplash(this), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        ui.postDelayed(this::removeSplashes, SPLASH_MOST_MS);  // the game's own ticks may never come
        if (safeStart) showSafeStartNotice();
        watchLaunch();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();
        setupTwoScreens();
        startReporting();
    }

    @Override
    protected void onStart() {
        super.onStart();
        started = true;
        showSecondScreen();
    }

    @Override
    protected void onStop() {
        started = false;
        hideSecondScreen();
        super.onStop();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
        if (hasFocus && !gotFocus) report("[input] window focus: input reaches the game");
        if (hasFocus) gotFocus = true;
    }

    // GammaOS's quick boot (ro.gammaos.lean_boot) leaves Android's boot unfinished, and with it input dispatch: launched by nano
    // with the Control Center off, no key or touch reached any app (DispatchEnabled false, mBooted false) until a sleep and wake
    // or a lid close and open (2026-11-03; the mechanism, from GammaOS's services.jar, in docs/BLITZ_DS.md). The boot finishes
    // when every resumed activity has reported idle: a see-through activity of ours over the game for a moment (BlitzNudge),
    // and the game resuming after it, does that, as Magisk's prompt over it did. Once a process, 1 s after the first resume,
    // GammaOS only, while nothing's in a game yet. BLITZ_NUDGE=0: not. BLITZ_ROOT_INPUT=1: also the root way, through su:
    // IWindowManager.setEventDispatching(true), transaction 15.
    private boolean gotFocus, inputFixAsked;
    private void rootInputFix() {
        if (inputFixAsked || !BlitzLauncher.gammaOs()) return;
        inputFixAsked = true;
        if (!"0".equals(Os.getenv("BLITZ_NUDGE"))) {
            ui.postDelayed(() -> {
                report("[input] GammaOS: a moment's see-through activity, so Android's boot finishes and input comes on");
                try {
                    startActivity(new Intent(this, BlitzNudge.class).addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION));
                } catch (Exception e) { report("[input] nudge: " + e); }
            }, 1000);
        }
        if (!"1".equals(Os.getenv("BLITZ_ROOT_INPUT"))) return;
        Thread t = new Thread(() -> {
            try {
                java.lang.Process p = Runtime.getRuntime().exec(new String[] {"su", "-c", "service call window 15 i32 1"});
                boolean done = p.waitFor(15, java.util.concurrent.TimeUnit.SECONDS);
                report("[input] GammaOS: event dispatching turned on through su: " + (done ? "exit " + p.exitValue() : "no answer in 15 s"));
                if (!done) p.destroy();
            } catch (Exception e) {
                report("[input] GammaOS: no su for the input fix (" + e.getMessage() + ")");
            }
        }, "BlitzInputFix");
        t.setDaemon(true);
        t.start();
    }

    /**
     * HWUI (Android's GPU drawing of views) for one of the game's windows, before it's on screen. The manifest has it off, so this
     * turns it back on, except in Potato Mode: there the splash and notices are drawn by the CPU, and the process has no second GPU
     * context with its caches next to the game's Vulkan. The game's own picture is a SurfaceView and never used it. BLITZ_HWUI=1:
     * on in Potato Mode too.
     */
    static void hwui(Context context, android.view.Window window) {
        if (!BlitzLauncher.lean(context) || "1".equals(Os.getenv("BLITZ_HWUI"))) {
            window.setFlags(WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED, WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED);
        }
    }

    // What the window really got: Android may still decide.
    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        report("[device] hwui " + (getWindow().getDecorView().isHardwareAccelerated() ? "on" : "off") + " for the game's window");
    }

    // Edge to edge, the status and navigation bars hidden until swiped in.
    private void hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    // --- A surface taller than wide ----------------------------------------------------------------------------------

    // SDL won't start on a tall surface while the activity asks for landscape (SDLSurface.surfaceChanged: "Surface is not ready"),
    // and it waits for good. Normally that's a moment before Android turns the window, as on a phone opened from a portrait home
    // screen. GammaOS's DualStack makes one tall surface that never turns: the game would never start. So if it's still tall after
    // a moment, the lock goes, for this launch, and the game starts on what it has (stretched, for now). Only for a launch whose
    // surface has never been ready: waking a running game, on a device whose panel is tall by nature (a Retroid), has a tall
    // surface for a while too, and that lock has to stay. And the moment only counts awake, unlocked and with the activity up.
    private static final long TALL_WAIT_MS = 2000, TALL_RECHECK_MS = 500;
    private boolean orientationFreed, surfaceWasReady;
    private long tallWaitFrom;  // 0: the moment hasn't begun
    private final Runnable tallCheck = this::freeOrientationIfStillTall;

    private static boolean isTall(int w, int h) { return w > 0 && h > w && (double) h / w >= 1.2; }  // SDL's own square-ish exception

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface s = makeSDLSurface(context);
        // BLITZ_DS_FIRST_PX=WxH: the game's own surface at that size, scaled up by the display, as BLITZ_DS_SECOND_PX does for the
        // second screen's. Its swap chain is 7 images on an RG DS: 21 MB at 1024x768, 13 at 800x600.
        String px = Os.getenv("BLITZ_DS_FIRST_PX");
        if (px != null && px.matches("\\d{2,4}x\\d{2,4}")) {
            String[] wh = px.split("x");
            s.getHolder().setFixedSize(Integer.parseInt(wh[0]), Integer.parseInt(wh[1]));
            report("[ds] first surface fixed at " + px);
        }
        return s;
    }

    private SDLSurface makeSDLSurface(Context context) {
        return new SDLSurface(context) {
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                super.surfaceChanged(holder, format, width, height);
                ui.removeCallbacks(tallCheck);
                if (mIsSurfaceReady) {
                    surfaceWasReady = true;
                } else if (!surfaceWasReady && isTall(width, height)) {
                    tallWaitFrom = awakeAndUp() ? android.os.SystemClock.elapsedRealtime() : 0;
                    ui.postDelayed(tallCheck, TALL_WAIT_MS);
                }
            }
        };
    }

    // No focus test: DualStack may never give this window focus.
    private boolean awakeAndUp() {
        return mIsResumedCalled && getWindowManager().getDefaultDisplay().getState() != Display.STATE_OFF
                && !((android.app.KeyguardManager) getSystemService(Context.KEYGUARD_SERVICE)).isKeyguardLocked();
    }

    private void freeOrientationIfStillTall() {
        final SDLSurface s = mSurface;
        if (orientationFreed || surfaceWasReady || current != this || s == null || s.mIsSurfaceReady || !s.getHolder().getSurface().isValid()) return;
        android.graphics.Rect r = s.getHolder().getSurfaceFrame();
        int asked = getRequestedOrientation();
        boolean landscape = asked == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE || asked == android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if (!landscape || !isTall(r.width(), r.height())) return;
        if (!awakeAndUp()) {
            tallWaitFrom = 0;
            ui.postDelayed(tallCheck, TALL_RECHECK_MS);
            return;
        }
        final long now = android.os.SystemClock.elapsedRealtime();
        if (tallWaitFrom == 0) tallWaitFrom = now;
        if (now - tallWaitFrom < TALL_WAIT_MS) {
            ui.postDelayed(tallCheck, TALL_WAIT_MS - (now - tallWaitFrom));
            return;
        }
        report("[launch] the surface is still tall (" + r.width() + "x" + r.height() + ") after " + TALL_WAIT_MS + " ms: orientation lock cleared, so the game can start on it");
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        orientationFreed = true;
        s.surfaceChanged(s.getHolder(), android.graphics.PixelFormat.RGBA_8888, r.width(), r.height());  // Android has no reason to say so again
    }

    // SDL reads this back right away and asks for landscape again when its window opens (main.cpp's orientation hint). Once freed,
    // it stays free, and SDL's check sees that whatever order Android handles the request in.
    @Override
    public int getRequestedOrientation() {
        return orientationFreed ? android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED : super.getRequestedOrientation();
    }

    @Override
    public void setRequestedOrientation(int orientation) {
        if (!orientationFreed) super.setRequestedOrientation(orientation);
    }

    // --- Resources ---------------------------------------------------------------------------------------------------

    // assets/res -> files/res, once per installed version (versionCode and install time): an update replaces them all.
    private void unpackResources(File dest) throws Exception {
        PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
        String stamp = info.getLongVersionCode() + ":" + info.lastUpdateTime;
        File stampFile = new File(dest, ".stamp");
        if (stampFile.exists() && stamp.equals(readText(stampFile))) return;
        long t0 = System.currentTimeMillis();
        deleteTree(dest);
        dest.mkdirs();
        int n = copyAssetTree(getAssets(), "res", dest);
        try (OutputStream o = new FileOutputStream(stampFile)) { o.write(stamp.getBytes("UTF-8")); }
        Log.i(TAG, "unpacked " + n + " resource files in " + (System.currentTimeMillis() - t0) + " ms");
    }

    private static int copyAssetTree(AssetManager am, String path, File dest) throws Exception {
        String[] children = am.list(path);
        if (children == null || children.length == 0) {  // a file (or an empty folder, which assets can't hold)
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(dest)) { copy(in, out); }
            return 1;
        }
        dest.mkdirs();
        int n = 0;
        for (String c : children) n += copyAssetTree(am, path + "/" + c, new File(dest, c));
        return n;
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private static String readText(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            copy(in, b);
            return b.toString("UTF-8");
        } catch (Exception e) {
            return "";
        }
    }

    private static long copy(InputStream in, OutputStream out) throws java.io.IOException {
        byte[] buf = new byte[1 << 16];
        long total = 0;
        for (int n; (n = in.read(buf)) > 0; ) { out.write(buf, 0, n); total += n; }
        return total;
    }

    // --- Called from native code (platform_android.cpp) -------------------------------------------------------------

    /** A blocking HTTP GET (a native thread): the body, with status[0] the HTTP status (0: no answer). */
    public static byte[] httpGet(String url, String bearer, String accept, int[] status) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(15000);
            // BRANDING: HTTP User-Agent
            c.setRequestProperty("User-Agent", "BlitzRecomp");
            if (accept != null && !accept.isEmpty()) c.setRequestProperty("Accept", accept);
            if (bearer != null && !bearer.isEmpty()) c.setRequestProperty("Authorization", "Bearer " + bearer);
            status[0] = c.getResponseCode();
            InputStream in = status[0] >= 400 ? c.getErrorStream() : c.getInputStream();
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            if (in != null) try (InputStream i = in) { copy(i, b); }
            return b.toByteArray();
        } catch (Exception e) {
            Log.w(TAG, "httpGet " + url + ": " + e);
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** A POST of a body (crash reports, src/main/crash_report.cpp): the HTTP status, or 0 when it couldn't connect. */
    public static int httpPost(String url, String contentType, byte[] body) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(20000);
            c.setReadTimeout(20000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            // BRANDING: HTTP User-Agent
            c.setRequestProperty("User-Agent", "BlitzRecomp");
            c.setRequestProperty("Content-Type", contentType);
            c.setFixedLengthStreamingMode(body.length);
            try (java.io.OutputStream o = c.getOutputStream()) { o.write(body); }
            return c.getResponseCode();
        } catch (Exception e) {
            Log.w(TAG, "httpPost: " + e);
            return 0;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** The system file picker; the picks are copied into the cache and handed to nativeFilesPicked. */
    public static void pickFiles(final boolean multiple) {
        final BlitzActivity a = current;
        if (a == null) { nativeFilesPicked(new String[0]); return; }
        a.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
            try {
                a.startActivityForResult(i, PICK_FILES);
            } catch (Exception e) {
                Log.w(TAG, "no file picker: " + e);
                nativeFilesPicked(new String[0]);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_FILES) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final ArrayList<Uri> uris = new ArrayList<>();
        if (resultCode == Activity.RESULT_OK && data != null) {
            if (data.getClipData() != null) {
                for (int k = 0; k < data.getClipData().getItemCount(); k++) uris.add(data.getClipData().getItemAt(k).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
        }
        // Copying a ROM takes a moment: off the main thread.
        new Thread(() -> {
            ArrayList<String> paths = new ArrayList<>();
            File dir = new File(getCacheDir(), "picked");
            deleteTree(dir);
            dir.mkdirs();
            for (Uri u : uris) {
                String name = displayName(u);
                File out = new File(dir, name);
                try (InputStream in = getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(out)) {
                    if (in == null) continue;
                    copy(in, o);
                    paths.add(out.getAbsolutePath());
                } catch (Exception e) {
                    Log.w(TAG, "couldn't copy " + u + ": " + e);
                }
            }
            nativeFilesPicked(paths.toArray(new String[0]));
        }, "BlitzPick").start();
    }

    private String displayName(Uri u) {
        String name = null;
        try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception e) {
            // the URI's last segment below
        }
        if (name == null || name.isEmpty()) name = u.getLastPathSegment();
        if (name == null || name.isEmpty()) name = "picked";
        return name.replace('/', '_').replace("..", "_");
    }

    static native void nativeFilesPicked(String[] paths);

    // --- BLITZ DS: a second screen, when the device has one (docs/BLITZ_DS.md) --------------------------------------

    private Presentation secondScreen;
    static Activity secondActivity;           // BlitzSecondScreen, when it's up
    static boolean secondActivityOpening;
    private boolean started;
    private DisplayManager displays;
    private final Handler ui = new Handler(Looper.getMainLooper());

    static native void nativeDsSurface(android.view.Surface surface, int width, int height);
    static native void nativeDsTap(float x, float y);
    static native void nativeDsFirstIsUpper(boolean upper);
    static native void nativeDigitalTrigger(int side, boolean down);
    static native void nativeLog(String line);
    static native void nativeStatus(String message);
    static native void nativeBlitzQuit();
    static native void nativeToggleMenu();
    static native void nativeAppBackground();
    static native void nativeAppForeground();
    static native boolean nativeLogUp();

    // Lines from before the game opens its log. The library's loaded early in onCreate, so nativeLog doesn't fail then: it prints to
    // a stderr that leads nowhere until log_init (app_log.cpp), on the SDL thread, once there's a surface. Which may be never, and
    // then blitz.log stays the last run's. So they're held here and written to <app folder>/blitz.early.log, for any launch that
    // doesn't get that far, and go into blitz.log once it's really this launch's.
    private static final java.util.ArrayList<String> earlyReports = new java.util.ArrayList<>();
    private static final int EARLY_MOST_LINES = 2000;
    private static final long EARLY_MOST_BYTES = 256 * 1024;
    private static volatile boolean nativeUp;
    private static File earlyFile;
    private static long launchAt;

    /** True once blitz.log is this launch's and holds everything said so far. */
    private static boolean flushEarly() {
        if (nativeUp) return true;
        synchronized (earlyReports) {
            if (nativeUp) return true;
            try {
                if (!nativeLogUp()) return false;
            } catch (UnsatisfiedLinkError e) {
                return false;  // the library isn't loaded yet
            }
            for (String early : earlyReports) nativeLog(early);
            earlyReports.clear();
            nativeUp = true;
            return true;
        }
    }

    private static void report(String line) {
        if (flushEarly()) {
            nativeLog(line);  // blitz.log and logcat
            return;
        }
        Log.i(TAG, line);
        String timed = String.format(Locale.US, "[early +%.1f s] %s", (android.os.SystemClock.elapsedRealtime() - launchAt) / 1000.0, line);
        synchronized (earlyReports) {
            if (earlyReports.size() < EARLY_MOST_LINES) earlyReports.add(timed);
            if (earlyFile != null && earlyFile.length() < EARLY_MOST_BYTES) {
                try (OutputStream o = new FileOutputStream(earlyFile, true)) { o.write((timed + "\n").getBytes("UTF-8")); }
                catch (Exception e) { Log.w(TAG, "early log: " + e); }
            }
        }
    }

    private void setupTwoScreens() {
        displays = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Display here = getWindowManager().getDefaultDisplay();
        report("[ds] this activity is on display " + here.getDisplayId());
        report("[ds] displays: " + displays.getDisplays().length + " listed, " + displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION).length + " for a presentation");
        for (Display d : BlitzLauncher.allDisplays(this)) report("[ds] " + describe(d));
        displays.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { report("[ds] display added: " + id); showSecondScreen(); }
            @Override public void onDisplayRemoved(int id) {
                report("[ds] display removed: " + id);
                if (secondScreen != null && secondScreen.getDisplay().getDisplayId() == id) hideSecondScreen();
            }
            @Override public void onDisplayChanged(int id) {
                Display d = displays.getDisplay(id);
                report("[ds] display changed: " + (d != null ? describe(d) : String.valueOf(id)));
                boolean ours = secondScreen != null && secondScreen.getDisplay().getDisplayId() == id;
                if (ours && d != null && d.getState() == Display.STATE_OFF) {
                    report("[ds] the second screen went off: one screen for now");
                    hideSecondScreen();
                }
                else if (secondScreen == null) showSecondScreen();  // off at launch, on now (a lid opening)
            }
        }, ui);
    }

    private static String describe(Display d) {
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        d.getRealMetrics(m);
        return "display " + d.getDisplayId() + " \"" + d.getName() + "\" " + m.widthPixels + "x" + m.heightPixels + " dpi " + m.densityDpi + " rotation " + d.getRotation()
                + " refresh " + d.getRefreshRate() + " state " + d.getState() + " flags 0x" + Integer.toHexString(d.getFlags());
    }

    // A test build's stand-in for two screens: <app folder>/ds_stack holds any text. Held upright, the panel becomes two nearly
    // 4:3 screens with a hinge's gap, so the real GPU draws what a dual-screen handheld would. tools/android/remote.sh switches it.
    private SurfaceView stackedView;
    private boolean stacked() {
        File app = getExternalFilesDir(null);
        return !BlitzLauncher.lean(this) && !BlitzRemote.base(this).isEmpty() && app != null && new File(app, "ds_stack_test").isFile();
    }
    private void showStacked() {
        if (stackedView != null || mLayout == null || mSurface == null) return;
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        int w = Math.min(m.widthPixels, m.heightPixels), h = Math.max(m.widthPixels, m.heightPixels);
        // Under 1.2:1, not 4:3: in a portrait activity SDL won't start on a surface wider than that ("Surface is not ready").
        int screenHeight = Math.min(w, h * 47 / 100);
        int screenWidth = Math.min(w, screenHeight * 119 / 100);
        android.widget.RelativeLayout.LayoutParams lower = new android.widget.RelativeLayout.LayoutParams(screenWidth, screenHeight);
        lower.addRule(android.widget.RelativeLayout.ALIGN_PARENT_BOTTOM);
        lower.addRule(android.widget.RelativeLayout.CENTER_HORIZONTAL);
        mSurface.setLayoutParams(lower);
        stackedView = makeSecondView(this);
        // ds_stack holding "wide": a 16:9 top screen, as the Retroid Pocket Duo and the Thor have.
        boolean wide = false;
        try { wide = new String(java.nio.file.Files.readAllBytes(new File(getExternalFilesDir(null), "ds_stack_test").toPath())).trim().equals("wide"); }
        catch (Exception e) { report("[ds] stacked: " + e); }
        android.widget.RelativeLayout.LayoutParams upper = wide ? new android.widget.RelativeLayout.LayoutParams(w, w * 9 / 16)
                : new android.widget.RelativeLayout.LayoutParams(screenWidth, screenHeight);
        upper.addRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
        upper.addRule(android.widget.RelativeLayout.CENTER_HORIZONTAL);
        mLayout.addView(stackedView, upper);
        report("[ds] stacked: two " + screenWidth + "x" + screenHeight + " screens on one " + w + "x" + h + " panel");
    }

    // GammaOS's DualStack (the app listed in persist.gammaos.dualstack.pkgs): both panels as one display, twice as tall as wide,
    // the upper panel its upper half. On an RG DS that's 1024x1536 and no second display. Two screens: the game's window (the
    // lower screen's UI in Stylus) in the lower half and the picture in the upper; one screen: the game in the upper half and the
    // lower left black. Each half is landscape, so SDL starts on it with the lock it asks for (no tall surface to wait out).
    private boolean dualStackShown;
    private boolean dualStack() {
        if (!BlitzLauncher.gammaOs()) return false;
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        return BlitzLauncher.allDisplays(this).size() == 1 && m.heightPixels >= m.widthPixels * 7 / 5;
    }
    private void showDualStack() {
        if (dualStackShown || mLayout == null || mSurface == null) return;
        dualStackShown = true;
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        int w = m.widthPixels, half = m.heightPixels / 2;
        mLayout.setBackgroundColor(android.graphics.Color.BLACK);
        boolean two = !safeStart && BlitzLauncher.twoScreens(this);
        android.widget.RelativeLayout.LayoutParams game = new android.widget.RelativeLayout.LayoutParams(w, half);
        game.addRule(two ? android.widget.RelativeLayout.ALIGN_PARENT_BOTTOM : android.widget.RelativeLayout.ALIGN_PARENT_TOP);
        mSurface.setLayoutParams(game);
        if (two) {
            stackedView = makeSecondView(this);
            android.widget.RelativeLayout.LayoutParams upper = new android.widget.RelativeLayout.LayoutParams(w, half);
            upper.addRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
            mLayout.addView(stackedView, upper);
        }
        report("[ds] DualStack " + w + "x" + m.heightPixels + ": " + (two ? "the game's window below, the picture above" : "one screen, on the upper half"));
    }

    /** Touch panels on this device. Touch can't be tied to a display from here, so a count is what there is. */
    static int touchPanels() {
        int panels = 0;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && !d.isVirtual() && (d.getSources() & InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN) panels++;
        }
        return panels;
    }

    private Display findSecond() {
        if (safeStart || !BlitzLauncher.twoScreens(this)) return null;
        Display here = getWindowManager().getDefaultDisplay();
        for (Display d : BlitzLauncher.allDisplays(this)) {
            if (d.getDisplayId() != here.getDisplayId() && d.getState() != Display.STATE_OFF) return d;
        }
        return null;
    }

    /** The second screen's picture: a surface whose life the renderer follows, and taps. A Presentation or BlitzSecondScreen holds it. */
    static SurfaceView makeSecondView(Context context) {
        SurfaceView view = new SurfaceView(context);
        secondView = view;
        // BLITZ_DS_SECOND_PX=WxH: the second screen's surface at that size, scaled up by the display. Its swap chain is one of the
        // biggest things two screens add (7 images at 1024x768 on an RG DS: 22 MB); 768x576 is 12.
        String px = Os.getenv("BLITZ_DS_SECOND_PX");
        if (px != null && px.matches("\\d{2,4}x\\d{2,4}")) {
            String[] wh = px.split("x");
            view.getHolder().setFixedSize(Integer.parseInt(wh[0]), Integer.parseInt(wh[1]));
            report("[ds] second surface fixed at " + px);
        }
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { report("[ds] second surface created"); }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int height) {
                report("[ds] second surface " + w + "x" + height + " format " + format);
                nativeDsSurface(h.getSurface(), w, height);
            }
            @Override public void surfaceDestroyed(SurfaceHolder h) {
                report("[ds] second surface destroyed");
                nativeDsSurface(null, 0, 0);
            }
        });
        view.setOnTouchListener((v, e) -> {
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN && v.getWidth() > 0 && v.getHeight() > 0) {
                nativeDsTap(e.getX() / v.getWidth(), e.getY() / v.getHeight());
            }
            return true;
        });
        return view;
    }

    static void setupSecondWindow(android.view.Window window) {
        // BlitzSecondScreen's comes here with its content already set: still before the window's added, which is when it counts.
        hwui(window.getContext(), window);
        final View decor = window.getDecorView();
        decor.post(() -> report("[ds] hwui " + (decor.isHardwareAccelerated() ? "on" : "off") + " for the second screen"));
        // Keeps the controller's keys going to the game: a window that can take focus would take them.
        window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = window.getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    private void showSecondScreen() {
        if (secondScreen != null || secondActivity != null || secondActivityOpening || displays == null || !started) return;
        if (dualStack()) { showDualStack(); return; }
        if (stacked()) { showStacked(); return; }
        final Display d = findSecond();
        if (d == null) { report("[ds] one screen"); return; }
        tellWhichIsUpper(d);
        // Android allows a Presentation only on a display flagged for one, which an internal panel may not be, and the
        // default display never is: then a plain activity on that display does the same job.
        boolean flagged = (d.getFlags() & Display.FLAG_PRESENTATION) != 0;
        boolean forceActivity = "1".equals(System.getenv("BLITZ_DS_ACTIVITY"));
        if (flagged && !forceActivity) {
            try {
                Presentation p = new Presentation(this, d) {
                    @Override protected void onCreate(Bundle state) {
                        super.onCreate(state);
                        hwui(getContext(), getWindow());
                        android.widget.FrameLayout holder = new android.widget.FrameLayout(getContext());
                        holder.addView(makeSecondView(getContext()));
                        if (!splashHidden) holder.addView(makeSplash(getContext()));
                        setContentView(holder);
                        setupSecondWindow(getWindow());
                        // On GammaOS it takes keys from the start, so they don't stay with the launcher on this screen (its own, on
                        // an RG DS's top one) when the game starts on the other. They all come to the game. Elsewhere only if the
                        // game's left without focus: below.
                        if (BlitzLauncher.gammaOs()) getWindow().clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
                    }
                    @Override public boolean dispatchKeyEvent(KeyEvent e) {
                        return forwardKey(e);
                    }
                    @Override public boolean dispatchGenericMotionEvent(android.view.MotionEvent e) {
                        if (SDLControllerManager.handleJoystickMotionEvent(e)) return true;
                        return super.dispatchGenericMotionEvent(e);
                    }
                    @Override public boolean dispatchTouchEvent(MotionEvent e) {
                        rawTouch("presentation", e);
                        return super.dispatchTouchEvent(e);
                    }
                };
                // Android cancels it itself when the display changes shape: then it's made again.
                p.setOnDismissListener(x -> {
                    if (secondScreen == x) {
                        report("[ds] second screen dismissed");
                        secondScreen = null;
                        if (started) ui.post(this::showSecondScreen);
                    }
                });
                p.show();
                secondScreen = p;
                report("[ds] second screen shown on display " + d.getDisplayId() + " (presentation)");
                ui.postDelayed(() -> {
                    comeToFront();
                    // Still no focus a moment later: this window takes the keys after all, and passes them on (forwardKey).
                    ui.postDelayed(() -> {
                        if (BlitzLauncher.gammaOs() || secondScreen != p || !started || hasWindowFocus()) return;
                        report("[ds] still no window focus: the second screen takes the keys");
                        p.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
                    }, 1500);
                }, 1500);
                return;
            } catch (Exception e) {
                report("[ds] presentation FAILED on display " + d.getDisplayId() + ": " + e);
            }
        }
        try {
            Intent i = new Intent(this, BlitzSecondScreen.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
            secondActivityOpening = true;
            startActivity(i, android.app.ActivityOptions.makeBasic().setLaunchDisplayId(d.getDisplayId()).toBundle());
            report("[ds] second screen opening on display " + d.getDisplayId() + " (activity)");
            ui.postDelayed(this::comeToFront, 1500);
        } catch (Exception e) {
            secondActivityOpening = false;
            report("[ds] second screen FAILED on display " + d.getDisplayId() + ": " + e);
        }
    }

    // GammaOS's Control Center can stay over a game just started on the bottom screen until it's tapped.
    private void comeToFront() {
        report("[ds] window focus " + hasWindowFocus() + " on display " + getWindowManager().getDefaultDisplay().getDisplayId());
        try { ((ActivityManager) getSystemService(Context.ACTIVITY_SERVICE)).moveTaskToFront(getTaskId(), 0); }
        catch (Exception e) { report("[ds] couldn't come to the front: " + e); }
    }

    // The game's on one screen and the other is found here, so the picture learns which is on top before it draws on both.
    private void tellWhichIsUpper(Display second) {
        Display here = getWindowManager().getDefaultDisplay();
        boolean upper = BlitzLauncher.firstIsUpper(this, here, second);
        firstUpper = upper;
        report("[ds] upper screen: display " + (upper ? here : second).getDisplayId() + " (model " + Build.MODEL + ")");
        try { nativeDsFirstIsUpper(upper); }
        catch (UnsatisfiedLinkError e) { report("[ds] which screen is on top: " + e); }
    }

    private void hideSecondScreen() {
        if (secondScreen != null) {
            Presentation p = secondScreen;
            secondScreen = null;  // the dismiss listener: this one's on purpose
            try { p.dismiss(); } catch (Exception e) { report("[ds] dismiss: " + e); }
        }
        if (secondActivity != null) {
            secondActivity.finish();
            secondActivity = null;
        }
    }

    // --- Reports: what the device is doing, and getting the logs to the developer -----------------------------------

    private static final long REPORT_EVERY_MS = 10000, UPLOAD_EVERY_MS = 60 * 1000;
    /** This launch, in UTC, the way the Worker names files (so the list sorts it with the rest). */
    private static final String LAUNCH_STAMP;
    static {
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH-mm-ss", Locale.US);
        f.setTimeZone(java.util.TimeZone.getTimeZone("UTC"));
        LAUNCH_STAMP = f.format(new java.util.Date()) + "Z-" + android.os.Process.myPid();
    }

    private File appFolder() {
        File app = getExternalFilesDir(null);
        return app != null ? app : new File(getFilesDir(), "app");
    }

    private File pendingFolder() { return new File(getFilesDir(), "pending_logs"); }

    // On every log that goes up: what built it, which launch, and whether the game has started in that launch.
    private static String buildStamp = "build ? commit ?", launchId = "?";

    private void readStamp() {
        launchAt = android.os.SystemClock.elapsedRealtime();
        launchId = new java.text.SimpleDateFormat("MMdd-HHmmss", Locale.US).format(new java.util.Date()) + "-" + android.os.Process.myPid();
        String commit = "unknown";
        long build = 0;
        try (InputStream in = getAssets().open("res/build_commit")) {  // tools/android/build_apk.sh
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            copy(in, b);
            commit = b.toString("UTF-8").trim();
        } catch (Exception e) { /* then unknown */ }
        try { build = getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { /* then 0 */ }
        buildStamp = "build " + build + " commit " + commit;
    }

    private static String stamp() {
        return "[stamp] " + buildStamp + " launch " + launchId + ", game " + (nativeUp ? "started" : "not started");
    }

    // This launch's early log, and the marker saying it's under way: what keepLogOfDeadRun finds if it never closes normally.
    private void startEarlyLog() {
        try {
            File app = appFolder();
            app.mkdirs();
            File early = new File(app, "blitz.early.log");
            early.renameTo(new File(app, "blitz.early.prev.log"));
            earlyFile = early;
            try (OutputStream o = new FileOutputStream(new File(getFilesDir(), "running"))) {
                o.write((launchId + "\n" + System.currentTimeMillis() + "\n" + buildStamp + "\n").getBytes("UTF-8"));
            }
        } catch (Exception e) {
            Log.w(TAG, "couldn't start the early log: " + e);  // only costs the crash report
        }
        report(stamp());
    }

    // Until the game's log is up: what's in the way, since nothing else can say. SDL starts the game only with a ready surface,
    // the window's focus (another window on a dual-screen device can hold it) and a resumed activity.
    private void watchLaunch() {
        new Runnable() {
            private int seconds;
            @Override public void run() {
                if (current != BlitzActivity.this) return;
                if (flushEarly()) {
                    report("[launch] the game's log is up, about " + seconds + " s in");
                    report(stamp());
                    return;
                }
                seconds++;
                if (seconds == 5 || seconds == 15 || seconds == 30 || seconds % 60 == 0) {
                    final SDLSurface s = mSurface;
                    android.graphics.Rect r = s != null ? s.getHolder().getSurfaceFrame() : new android.graphics.Rect();
                    report("[launch] " + seconds + " s, no game yet: surface ready " + (s != null && s.mIsSurfaceReady) + " " + r.width() + "x" + r.height()
                            + ", focus " + mHasFocus + ", resumed " + mIsResumedCalled + ", orientation " + getRequestedOrientation()
                            + ", display state " + getWindowManager().getDefaultDisplay().getState()
                            + ", keyguard " + ((android.app.KeyguardManager) getSystemService(Context.KEYGUARD_SERVICE)).isKeyguardLocked());
                }
                if (seconds < 600) ui.postDelayed(this, 1000);
            }
        }.run();
    }

    private static final String[] EXIT_REASONS = {"unknown", "exit self", "signaled", "low memory", "crash", "native crash", "not responding", "initialization failure",
        "permission change", "excessive resources", "user requested", "user stopped", "dependency died", "other", "freezer", "package state change", "package updated"};

    // Which process: the newest record may be :remote's, and more than one process's can look like the game's.
    private static String describeExit(ApplicationExitInfo x) {
        int r = x.getReason();
        return "reason " + r + " (" + (r >= 0 && r < EXIT_REASONS.length ? EXIT_REASONS[r] : "?") + ") process " + x.getProcessName() + " pid " + x.getPid()
                + String.format(Locale.US, " at %tF %<tT", x.getTimestamp()) + " status " + x.getStatus() + " importance " + x.getImportance()
                + " pss " + (x.getPss() >> 10) + " MB rss " + (x.getRss() >> 10) + " MB: " + x.getDescription();
    }

    // The end of a log, into something being sent. Stale: not from the launch being reported, and it says so.
    private static void writeLogTail(OutputStream o, File log, boolean stale) throws java.io.IOException {
        if (!log.isFile()) return;
        if (stale) o.write("[stamp] STALE: that launch's game never started. The blitz.log below is from an earlier launch\n".getBytes("UTF-8"));
        try (java.io.RandomAccessFile in = new java.io.RandomAccessFile(log, "r")) {
            long most = stale ? STALE_TAIL_BYTES : LOG_TAIL_BYTES;
            if (in.length() > most) in.seek(in.length() - most);
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
        }
    }

    // Before the game starts and replaces its log: a run that never closed normally keeps its log to send, however early it
    // died. Most likely of all on a new device, where a crash at startup would otherwise repeat with nothing sent.
    private void keepLogOfDeadRun() {
        File marker = new File(getFilesDir(), "running");
        if (!marker.exists()) return;
        try {
            File log = new File(appFolder(), "blitz.log"), early = new File(appFolder(), "blitz.early.log");
            if (!log.isFile() && !early.isFile()) return;
            File folder = pendingFolder();
            folder.mkdirs();
            File[] waiting = folder.listFiles();
            if (waiting != null && waiting.length >= 5) return;  // enough of the same story
            String[] dead = readText(marker).split("\n");  // startEarlyLog; an older build's is empty
            long deadAt = 0;
            try { deadAt = Long.parseLong(dead[1]); } catch (Exception e) { /* then it can't tell */ }
            boolean stale = deadAt > 0 && log.lastModified() + 1000 < deadAt;  // nothing written since that launch began
            File out = new File(folder, "dead-" + System.currentTimeMillis() + ".log");
            try (OutputStream o = new FileOutputStream(out)) {
                o.write(("[stamp] the launch that died: " + (dead.length >= 3 ? dead[2] + " launch " + dead[0] : "unknown") + "\n").getBytes("UTF-8"));
                if (early.isFile()) o.write(java.nio.file.Files.readAllBytes(early.toPath()));
                writeLogTail(o, log, stale);
                if (Build.VERSION.SDK_INT >= 30) {
                    ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
                    for (ApplicationExitInfo x : am.getHistoricalProcessExitReasons(null, 0, 3)) {
                        o.write(("\n[device] how it ended: " + describeExit(x) + "\n").getBytes("UTF-8"));
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "couldn't keep the last log: " + e);
        }
    }

    // What's waiting goes first, and stays until it's really gone (Wi-Fi may not be up yet).
    private void sendPending() {
        File[] waiting = pendingFolder().listFiles();
        if (waiting == null) return;
        for (File f : waiting) {
            if (upload(f, "crash", true)) f.delete();
            else return;
        }
    }

    private void startReporting() {
        try { report("[device] build " + getPackageManager().getPackageInfo(getPackageName(), 0).versionCode); } catch (Exception e) { /* only a log line */ }
        report("[device] " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + "), android " + Build.VERSION.RELEASE + " sdk " + Build.VERSION.SDK_INT + ", " + Build.DISPLAY);
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null) report("[device] input " + id + " \"" + d.getName() + "\" sources 0x" + Integer.toHexString(d.getSources()));
            if (d != null) for (InputDevice.MotionRange r : d.getMotionRanges()) {
                if ((r.getSource() & InputDevice.SOURCE_JOYSTICK) != 0) report("[device]   axis " + android.view.MotionEvent.axisToString(r.getAxis()) + " " + r.getMin() + " to " + r.getMax() + " flat " + r.getFlat());
            }
        }
        final ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                List<ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(null, 0, 3);
                for (ApplicationExitInfo x : exits) {
                    report("[device] earlier exit: " + describeExit(x));
                }
            } catch (Exception e) { report("[device] exit reasons: " + e); }
        }
        final PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
        logTouches = BlitzLauncher.lean(this);
        if (logTouches) {
            // Its own thread: a getprop that hung shouldn't cost the reports.
            Thread os = new Thread(() -> {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
                reportOs(power);
            }, "BlitzOsReport");
            os.setDaemon(true);
            os.start();
        }
        Thread t = new Thread(() -> {
            Thread.currentThread().setName("BlitzReports");
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
            long nextUpload = System.currentTimeMillis() + 60000;
            sendPending();
            uploadPaks(new File(appFolder(), "saves"));
            for (;;) {
                try { Thread.sleep(REPORT_EVERY_MS); } catch (InterruptedException e) { return; }
                ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
                am.getMemoryInfo(mi);
                int thermal = Build.VERSION.SDK_INT >= 29 ? power.getCurrentThermalStatus() : -1;
                report(ownMemory());
                final boolean lean = BlitzLauncher.lean(this);
                if (lean) {
                    report(threadUse());
                    String swaps = swapUse(), clocks = clockUse(), touches = touchUse();
                    if (swaps != null) report(swaps);
                    if (clocks != null) report(clocks);
                    if (touches != null) report(touches);
                }
                // BLITZ_PRESSURE=0: no [pressure] line in Potato Mode; =1: one out of it too.
                String pressure = Os.getenv("BLITZ_PRESSURE");
                if (lean ? !"0".equals(pressure) : "1".equals(pressure)) report(pressureUse());
                report(String.format(Locale.US, "[device] ram %d of %d MB free%s, thermal %d", mi.availMem >> 20, mi.totalMem >> 20, mi.lowMemory ? " LOW" : "", thermal));
                sendPending();
                if (System.currentTimeMillis() >= nextUpload) {
                    nextUpload = System.currentTimeMillis() + UPLOAD_EVERY_MS;
                    upload(new File(appFolder(), "blitz.log"), "run", true);
                }
            }
        }, "BlitzReport");
        t.setDaemon(true);
        t.start();
    }

    // Android asking for memory back, in ComponentCallbacks2's levels. From Android 14 an app may get only 20 and 40.
    @Override
    public void onTrimMemory(int level) {
        String name = level == TRIM_MEMORY_RUNNING_MODERATE ? "running moderate" : level == TRIM_MEMORY_RUNNING_LOW ? "running low"
                : level == TRIM_MEMORY_RUNNING_CRITICAL ? "running critical" : level == TRIM_MEMORY_UI_HIDDEN ? "ui hidden"
                : level == TRIM_MEMORY_BACKGROUND ? "background" : level == TRIM_MEMORY_MODERATE ? "moderate" : level == TRIM_MEMORY_COMPLETE ? "complete" : "?";
        report("[pressure] trim " + level + " (" + name + ")");
        super.onTrimMemory(level);
    }

    // SDL's onDestroy sends a quit and waits for the game's thread with no time limit. With a game up, native code answers that
    // quit by opening a prompt nobody can see (input_events.cpp), so this thread would never come back, and the next launch needs
    // this same thread. After a Quit the thread has ended, but main() can't run a second time in one process (config::finalize
    // throws), and the remote service would keep this process for it. So Potato Mode's process ends here, in both cases, and
    // the next launch starts clean. Out of Potato Mode, SDL's way, unchanged: the Retroid has the same wait, a decision of its own.
    @Override
    protected void onDestroy() {
        final boolean endProcess = BlitzLauncher.lean(this);
        hideSecondScreen();
        current = null;
        new File(getFilesDir(), "running").delete();
        if (endProcess) {
            report("[launch] the game's activity is closing: its process ends with it");
            try { stopService(new android.content.Intent(this, BlitzRemoteLocal.class)); }
            catch (Exception e) { Log.w(TAG, "remote testing didn't stop: " + e); }
            android.os.Process.killProcess(android.os.Process.myPid());
        }
        super.onDestroy();
    }

    private String uploadAddress() {
        try (InputStream in = getAssets().open("res/logs_hub")) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            copy(in, b);
            return b.toString("UTF-8").trim();
        } catch (Exception e) {
            return "";  // a build without one
        }
    }

    private static final long LOG_TAIL_BYTES = 2 * 1024 * 1024, STALE_TAIL_BYTES = 256 * 1024;

    // Test builds: BlitzRemote takes commands from the Mac. Its presses reach the game through the live input file, and it
    // takes screenshots through here, since only this process has the window.
    private void startRemote(File app) throws Exception {
        if (BlitzRemote.base(this).isEmpty()) return;
        File input = new File(app, "remote_input.txt");
        try (OutputStream o = new FileOutputStream(input)) { /* only what's new */ }
        Os.setenv("BLITZ_INPUT_LIVE", input.getAbsolutePath(), true);
        if (new File(app, "ds_stack_test").isFile()) {
            Os.setenv("BLITZ_DS_STACK", "1", true);
            setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        }
        // One poller only: the mailbox gives each batch of commands to whichever asks first, and Potato Mode can have changed.
        final boolean lean = BlitzLauncher.lean(this);
        stopService(new android.content.Intent(this, lean ? BlitzRemote.class : BlitzRemoteLocal.class));
        startForegroundService(new android.content.Intent(this, lean ? BlitzRemoteLocal.class : BlitzRemote.class));
        setShowWhenLocked(true);  // a restart from the service comes to the front with the screen off
        setTurnScreenOn(true);
        android.content.BroadcastReceiver shots = new android.content.BroadcastReceiver() {
            @Override public void onReceive(Context c, android.content.Intent i) { screenshot(i.getStringExtra("name")); }
        };
        android.content.IntentFilter filter = new android.content.IntentFilter(getPackageName() + BlitzRemote.SHOT);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(shots, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(shots, filter);
        // Starting the game again after a crash, from the background, needs this one permission.
        if (!android.provider.Settings.canDrawOverlays(this)) {
            android.content.SharedPreferences prefs = getSharedPreferences("remote", MODE_PRIVATE);
            if (!prefs.getBoolean("asked", false)) {
                prefs.edit().putBoolean("asked", true).apply();
                startActivity(new android.content.Intent(android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + getPackageName())));
            }
        }
    }

    private void screenshot(String name) {
        final boolean upper = name.startsWith("top-") && stackedView != null;
        final android.view.SurfaceView view = upper ? stackedView : mSurface;
        if (view == null || view.getWidth() <= 0 || !view.getHolder().getSurface().isValid()) {  // not on screen: PixelCopy would throw
            new Thread(() -> {
                try {
                    BlitzRemote.post(BlitzRemote.base(this) + "/res/" + name, "not on screen".getBytes());
                } catch (Exception e) {
                    report("[remote] shot: " + e);
                }
            }, "BlitzShotSend").start();
            return;
        }
        final android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(view.getWidth(), view.getHeight(), android.graphics.Bitmap.Config.ARGB_8888);
        android.os.HandlerThread thread = new android.os.HandlerThread("BlitzShot");
        thread.start();
        android.view.PixelCopy.request(view, bitmap, result -> {
            new Thread(() -> {
                try {
                    ByteArrayOutputStream jpeg = new ByteArrayOutputStream();
                    bitmap.compress(android.graphics.Bitmap.CompressFormat.JPEG, 85, jpeg);
                    BlitzRemote.post(BlitzRemote.base(this) + "/res/" + name, result == android.view.PixelCopy.SUCCESS ? jpeg.toByteArray() : ("copy failed: " + result).getBytes());
                } catch (Exception e) {
                    report("[remote] shot: " + e);
                }
                thread.quitSafely();
            }, "BlitzShotSend").start();
        }, new android.os.Handler(thread.getLooper()));
    }

    // gzip: a log, of which only the last 2 MB go: a device may have little memory to spare, and this runs during play. With the
    // stamp first. Until the game opens its own blitz.log, that file's the last run's: this launch's early lines go first, and
    // what follows is labelled, in the file and in its kind.
    private boolean upload(File file, String kind, boolean gzip) {
        String address = uploadAddress();
        final boolean stale = gzip && file.getName().equals("blitz.log") && !nativeUp;
        if (address.isEmpty() || (!file.isFile() && !stale)) return false;
        HttpURLConnection c = null;
        try {
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (OutputStream o = gzip ? new GZIPOutputStream(body) : body) {
                if (gzip) {
                    o.write((stamp() + ", sending " + file.getName() + "\n").getBytes("UTF-8"));
                    if (stale && earlyFile != null && earlyFile.isFile()) o.write(java.nio.file.Files.readAllBytes(earlyFile.toPath()));
                    writeLogTail(o, file, stale);
                }
                else {
                    try (InputStream in = new FileInputStream(file)) { copy(in, o); }
                }
            }
            // A running game's log keeps one name for the launch, so each upload replaces the last (the Worker's free storage
            // takes a limited number of new files a day).
            String launch = kind.equals("run") ? "&launch=" + LAUNCH_STAMP : "";
            c = (HttpURLConnection) new URL(address + "?device=" + Uri.encode(Build.DEVICE) + "&kind=" + kind + (stale ? "-stale" : "") + (gzip ? "-gz" : "") + launch).openConnection();
            c.setDoOutput(true);
            c.setRequestMethod("POST");
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setFixedLengthStreamingMode(body.size());
            try (OutputStream o = c.getOutputStream()) { body.writeTo(o); }
            int code = c.getResponseCode();
            report("[logs] sent " + file.getName() + ": HTTP " + code);
            return code == 200;
        } catch (Exception e) {
            report("[logs] couldn't send " + file.getName() + ": " + e);
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** What the game itself holds: resident and swapped out, from the kernel, so it's cheap enough for every report. */
    private static String ownMemory() {
        long rss = 0, swap = 0;
        try {
            for (String l : java.nio.file.Files.readAllLines(new File("/proc/self/status").toPath())) {
                String[] w = l.trim().split("\\s+");
                if (l.startsWith("VmRSS:")) rss = Long.parseLong(w[1]);
                else if (l.startsWith("VmSwap:")) swap = Long.parseLong(w[1]);
            }
        } catch (Exception e) { /* then zeros */ }
        return String.format(Locale.US, "[device] game holds %d MB, %d MB more swapped out", rss >> 10, swap >> 10);
    }

    // Something on screen from the first moment, until the game's own first frames.
    private final java.util.ArrayList<View> splashes = new java.util.ArrayList<>();

    private static final long SPLASH_MOST_MS = 20000;

    private View makeSplash(Context context) {
        android.widget.LinearLayout box = new android.widget.LinearLayout(context);
        box.setOrientation(android.widget.LinearLayout.VERTICAL);
        box.setGravity(android.view.Gravity.CENTER);
        box.setBackgroundColor(0xFF0A1450);
        android.widget.TextView t = new android.widget.TextView(context);
        // BRANDING: Android start screen text
        t.setText("NEW BLITZ");
        t.setTextColor(0xFFFFE600);
        t.setTextSize(44);
        t.setTypeface(android.graphics.Typeface.create(android.graphics.Typeface.DEFAULT_BOLD, android.graphics.Typeface.BOLD_ITALIC));
        t.setGravity(android.view.Gravity.CENTER);
        box.addView(t);
        // A slow handheld takes a while to the title (about 15 s on an RG DS): say so, so a black screen isn't a hang (team).
        if (BlitzLauncher.lean(context)) {
            android.widget.TextView sub = new android.widget.TextView(context);
            sub.setText("Starting up. This can take a little while.");
            sub.setTextColor(0xFF8FEFF5);
            sub.setTextSize(18);
            sub.setGravity(android.view.Gravity.CENTER);
            sub.setPadding(0, 24, 0, 0);
            box.addView(sub);
            leaveInputAlone(sub);
        }
        leaveInputAlone(t);
        leaveInputAlone(box);
        synchronized (splashes) { splashes.add(box); }
        return box;
    }

    // Only something to look at: touches and keys go on to the game beneath it.
    private static void leaveInputAlone(View v) {
        v.setClickable(false);
        v.setLongClickable(false);
        v.setFocusable(false);
        v.setFocusableInTouchMode(false);
    }

    /** From native code, the UI's first ticks (ds_view.cpp). */
    public static void hideSplash() {
        final BlitzActivity a = current;
        if (a == null) return;
        a.runOnUiThread(a::removeSplashes);
    }

    private void removeSplashes() {
        synchronized (splashes) {
            for (View v : splashes) {
                if (v.getParent() instanceof ViewGroup) ((ViewGroup) v.getParent()).removeView(v);
            }
            splashes.clear();
        }
        splashHidden = true;
    }
    private boolean splashHidden;

    /** From native code, once the game has held a frame rate for one of its 5 s windows (ds_view.cpp): this launch drew. */
    public static void firstFrame() {
        final BlitzActivity a = current;
        if (a == null) return;
        drewThisProcess = true;
        if (BlitzLauncher.lean(a)) a.getSharedPreferences(BlitzLauncher.PREFS, Context.MODE_PRIVATE).edit().putInt(BlitzLauncher.NO_FRAME, 0).commit();
        report("[launch] the game is drawing");
        hideSplash();
    }
    private static boolean drewThisProcess;
    private boolean safeStart;

    /**
     * Potato Mode: after two launches that never drew, however they ended (a crash, a kill, a force stop, all alike here), this one
     * runs one screen at original resolution with Full barriers, without the limits and pipelines saved by earlier runs (moved
     * aside). For this launch and each one after it until one draws; Settings stay as they are, so the next try is the usual one.
     */
    private void safeStartCheck() {
        if (drewThisProcess || !BlitzLauncher.lean(this)) return;
        safeStart = BlitzLauncher.safeStartDue(this);
        android.content.SharedPreferences prefs = getSharedPreferences(BlitzLauncher.PREFS, Context.MODE_PRIVATE);
        int failed = prefs.getInt(BlitzLauncher.NO_FRAME, 0);
        prefs.edit().putInt(BlitzLauncher.NO_FRAME, failed + 1).commit();
        report("[launch] launches since one drew: " + failed);
        if (!safeStart) return;
        report("[device] safe start: " + failed + " launches never drew. One screen, original resolution, Full GPU waits");
        File[] files = appFolder().listFiles();
        if (files != null) for (File f : files) {
            String n = f.getName();
            if (n.equals("pipeline_cache.bin") || (n.startsWith("gfx_limits") && !n.contains(".safe"))) setAside(f);
        }
    }

    // Moved, not deleted, and an earlier set-aside stays: <name>.safe1, .safe2, up to the last one, which gets reused.
    private static void setAside(File f) {
        for (int n = 1; ; n++) {
            File to = new File(f.getParentFile(), f.getName() + ".safe" + n);
            if (to.exists() && n < 5) continue;
            report("[device] safe start: " + f.getName() + (f.renameTo(to) ? " moved to " + to.getName() : " couldn't be moved"));
            return;
        }
    }

    private void showSafeStartNotice() {
        if (mLayout == null) return;
        final android.widget.TextView t = new android.widget.TextView(this);
        t.setText("SAFE START: the last launches never drew, so this one runs on one screen at original resolution. Saved limits and pipelines were set aside. Your settings are unchanged.");
        t.setTextColor(0xFFFFE600);
        t.setTextSize(16);
        t.setPadding(24, 16, 24, 16);
        t.setBackgroundColor(0xE00A1450);
        leaveInputAlone(t);
        android.widget.RelativeLayout.LayoutParams top = new android.widget.RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        top.addRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
        mLayout.addView(t, top);
        ui.postDelayed(() -> { if (t.getParent() instanceof ViewGroup) ((ViewGroup) t.getParent()).removeView(t); }, SAFE_NOTICE_MS);
    }
    private static final long SAFE_NOTICE_MS = 20000;

    private int keysLogged;

    /** Every touch this window gets, before SDL or the game can turn it down (rawTouch). */
    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        rawTouch("main", e);
        return super.dispatchTouchEvent(e);
    }

    /** The first keys of a run, in the log: a handheld's Start or Select may not arrive as its controller's. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        logKey(e);
        if (deviceBack(e)) return true;
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2) {
            if (e.getAction() != KeyEvent.ACTION_MULTIPLE) nativeDigitalTrigger(code == KeyEvent.KEYCODE_BUTTON_L2 ? 0 : 1, e.getAction() == KeyEvent.ACTION_DOWN);
            return true;  // SDL's controller layer ignores these two on Android
        }
        return super.dispatchKeyEvent(e);
    }

    /** The second screen's picture (the last one made), and whether the game's own screen is the upper one: for screenshots. */
    private static SurfaceView secondView;
    private static boolean firstUpper;

    /** From native code: a file the game made (a replay) copied into the gallery, so it's easy to find and share (team): an image
     *  to Pictures/New Blitz, a video to Movies/New Blitz. On the caller's thread (not the UI's: the copy can take a moment). */
    public static void addToGallery(String path, String mime) {
        final BlitzActivity a = current;
        if (a == null) return;
        File f = new File(path);
        boolean video = mime.startsWith("video/");
        try {
            android.content.ContentValues values = new android.content.ContentValues();
            values.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, f.getName());
            values.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, mime);
            // BRANDING: gallery folder name
            values.put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, (video ? "Movies" : "Pictures") + "/New Blitz");
            Uri uri = a.getContentResolver().insert(video ? android.provider.MediaStore.Video.Media.EXTERNAL_CONTENT_URI
                                                          : android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
            if (uri == null) throw new java.io.IOException("no place for it");
            try (OutputStream o = a.getContentResolver().openOutputStream(uri); InputStream in = new FileInputStream(f)) {
                copy(in, o);
            }
            report("[capture] in the gallery: " + (video ? "Movies" : "Pictures") + "/New Blitz/" + f.getName());
        } catch (Exception e) {
            report("[capture] couldn't add " + f.getName() + " to the gallery: " + e);
        }
    }

    /** From native code: the hotkey + Select (controllers.cpp, capture.cpp). */
    public static void takeScreenshot() {
        final BlitzActivity a = current;
        if (a != null) a.runOnUiThread(a::saveScreenshot);
    }

    /** Both screens' pictures, the upper one first, as one PNG in Pictures/New Blitz (the gallery and file managers find it). */
    void saveScreenshot() {
        final List<SurfaceView> views = new ArrayList<>();
        SurfaceView second = secondView;
        boolean twoScreens = second != null && second.getWidth() > 0 && second.getHolder().getSurface().isValid();
        if (twoScreens && !firstUpper) views.add(second);
        if (mSurface != null && mSurface.getWidth() > 0 && mSurface.getHolder().getSurface().isValid()) views.add(mSurface);
        if (twoScreens && firstUpper) views.add(second);
        if (views.isEmpty()) return;
        final android.graphics.Bitmap[] got = new android.graphics.Bitmap[views.size()];
        final int[] left = {views.size()};
        android.os.HandlerThread thread = new android.os.HandlerThread("BlitzScreenshot");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        for (int i = 0; i < views.size(); i++) {
            final int k = i;
            SurfaceView v = views.get(i);
            final android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(v.getWidth(), v.getHeight(), android.graphics.Bitmap.Config.ARGB_8888);
            try {
                android.view.PixelCopy.request(v, b, result -> {
                    if (result == android.view.PixelCopy.SUCCESS) got[k] = b;
                    if (--left[0] > 0) return;
                    thread.quitSafely();
                    new Thread(() -> writeScreenshot(got), "BlitzScreenshotSave").start();
                }, handler);
            } catch (IllegalArgumentException x) {  // the surface went away since the check
                handler.post(() -> {
                    if (--left[0] > 0) return;
                    thread.quitSafely();
                    new Thread(() -> writeScreenshot(got), "BlitzScreenshotSave").start();
                });
            }
        }
    }

    private void writeScreenshot(android.graphics.Bitmap[] parts) {
        int w = 0, h = 0;
        for (android.graphics.Bitmap b : parts) {
            if (b == null) continue;
            w = Math.max(w, b.getWidth());
            h += b.getHeight();
        }
        if (w == 0) { status("Couldn't take the screenshot"); return; }
        android.graphics.Bitmap all = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas = new android.graphics.Canvas(all);
        canvas.drawColor(android.graphics.Color.BLACK);
        int y = 0;
        for (android.graphics.Bitmap b : parts) {
            if (b == null) continue;
            canvas.drawBitmap(b, (w - b.getWidth()) / 2f, y, null);
            y += b.getHeight();
        }
        String name = "Blitz " + new java.text.SimpleDateFormat("yyyy-MM-dd 'at' HH.mm.ss", Locale.US).format(new java.util.Date()) + ".png";
        try {
            android.content.ContentValues values = new android.content.ContentValues();
            values.put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, name);
            values.put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png");
            // BRANDING: gallery folder name
            values.put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, "Pictures/New Blitz");
            Uri uri = getContentResolver().insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
            if (uri == null) throw new java.io.IOException("no place for it");
            try (OutputStream o = getContentResolver().openOutputStream(uri)) {
                all.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, o);
            }
            report("[capture] screenshot Pictures/New Blitz/" + name);
            status("Screenshot saved to Pictures/New Blitz");
        } catch (Exception e) {
            report("[capture] screenshot failed: " + e);
            status("Couldn't save the screenshot");
        }
    }

    private static void status(String message) {
        try { nativeStatus(message); } catch (UnsatisfiedLinkError e) { /* the game isn't loaded */ }
    }

    private final Handler backHold = new Handler(Looper.getMainLooper());

    @Override
    protected void onResume() {
        super.onResume();
        try { nativeAppForeground(); } catch (UnsatisfiedLinkError e) { /* the game isn't loaded yet */ }
        rootInputFix();
    }

    /** A Back held as the app goes away: its release may never come, so its timers go with it. */
    @Override
    protected void onPause() {
        backHold.removeCallbacksAndMessages(null);
        backDownAt = 0;
        // Straight to the game (paused, the Auto slot, then it holds): SDL's own event comes ~3 s later (an RG DS's lid).
        try { nativeAppBackground(); } catch (UnsatisfiedLinkError e) { /* the game isn't loaded yet */ }
        super.onPause();
    }
    private long backDownAt;
    private static final long BACK_WARN_MS = 2000, BACK_QUIT_MS = 5000;

    /** The device's own Back (an RG DS's side button, a phone's back gesture): pressed, it opens and closes the menu, as Escape
     *  does; held on, 2 s in it says how much longer quits, and at 5 s the app quits (the team; GammaOS quits it itself sooner). A controller's Back (Select) is its own
     *  button. Kept here either way: a Back nobody took gets the app stopped on GammaOS. The menu's asked for straight from
     *  native code: an Escape key reached SDL and went no further when no window had focus (a nano launch). */
    private boolean deviceBack(KeyEvent e) {
        if (e.getKeyCode() != KeyEvent.KEYCODE_BACK) return false;
        // Before the game draws (the launcher, first run) Back is the system's, as it was: there's no Settings to open yet.
        if (!drewThisProcess) return false;
        // Whole sources: a keyboard's (0x101, an RG DS's side button) shares the button class bit with a gamepad's (0x401).
        if (e.isFromSource(android.view.InputDevice.SOURCE_GAMEPAD) || e.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK)) return false;
        if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) {
            // On the press, not the release: GammaOS keeps the release (an RG DS's menu never opened on it).
            try { nativeToggleMenu(); } catch (UnsatisfiedLinkError x) { /* the game isn't loaded */ }
            backDownAt = e.getEventTime();
            backHold.removeCallbacksAndMessages(null);
            // GammaOS quits a held Back itself, sooner, and may keep the release: no timers there, or one would quit
            // 5 s after any press.
            if (BlitzLauncher.gammaOs()) return true;
            backHold.postDelayed(() -> status("Hold for 3 more seconds to exit"), BACK_WARN_MS);
            backHold.postDelayed(() -> {
                report("[keys] Back held 5 s: quitting");
                status("Exiting");
                try { nativeBlitzQuit(); } catch (UnsatisfiedLinkError x) { finishAndRemoveTask(); }
            }, BACK_QUIT_MS);
        }
        else if (e.getAction() == KeyEvent.ACTION_UP) {
            backHold.removeCallbacksAndMessages(null);
            backDownAt = 0;
        }
        return true;
    }

    private void logKey(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0 && keysLogged < 40) {
            keysLogged++;
            android.view.InputDevice d = e.getDevice();
            report(String.format(Locale.US, "[keys] %s, source 0x%x, device %s", KeyEvent.keyCodeToString(e.getKeyCode()), e.getSource(),
                d != null ? d.getName() : "none"));
        }
    }

    /** A key that reached the second screen's window: straight to SDL, as this window's views may not have focus. */
    private boolean forwardKey(KeyEvent e) {
        logKey(e);
        if (deviceBack(e)) return true;
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2) {
            if (e.getAction() != KeyEvent.ACTION_MULTIPLE) nativeDigitalTrigger(code == KeyEvent.KEYCODE_BUTTON_L2 ? 0 : 1, e.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN || code == KeyEvent.KEYCODE_POWER || code == KeyEvent.KEYCODE_HOME) return false;
        return SDLActivity.handleKeyEvent(null, code, e, null);
    }

    private int axesLogged;
    private static final int[] WATCHED_AXES = {android.view.MotionEvent.AXIS_LTRIGGER, android.view.MotionEvent.AXIS_RTRIGGER, android.view.MotionEvent.AXIS_Z,
        android.view.MotionEvent.AXIS_RZ, android.view.MotionEvent.AXIS_BRAKE, android.view.MotionEvent.AXIS_GAS, android.view.MotionEvent.AXIS_RX, android.view.MotionEvent.AXIS_RY};

    /** The first moved axes of a run, in the log: the triggers differ from handheld to handheld. */
    @Override
    public boolean dispatchGenericMotionEvent(android.view.MotionEvent e) {
        if (axesLogged < 24 && (e.getSource() & android.view.InputDevice.SOURCE_JOYSTICK) != 0) {
            for (int a : WATCHED_AXES) {
                if (Math.abs(e.getAxisValue(a)) > 0.5f && axesLogged < 24) {
                    axesLogged++;
                    report("[keys] axis " + android.view.MotionEvent.axisToString(a) + " = " + e.getAxisValue(a));
                }
            }
        }
        return super.dispatchGenericMotionEvent(e);
    }

    // Potato Mode: the raw touches of each window, in each report and never per event, so a finger can't write a line. Counted before
    // anything can reject them, with where the first few came down in that window's own coordinates.
    private static boolean logTouches;
    private static final class RawTouches { int events, downs; final StringBuilder at = new StringBuilder(); }
    private static final java.util.TreeMap<String, RawTouches> rawTouches = new java.util.TreeMap<>();

    static void rawTouch(String window, MotionEvent e) {
        if (!logTouches) return;
        synchronized (rawTouches) {
            RawTouches t = rawTouches.get(window);
            if (t == null) {
                t = new RawTouches();
                rawTouches.put(window, t);
            }
            t.events++;
            int action = e.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                t.downs++;
                if (t.downs <= 6) {
                    int i = e.getActionIndex();
                    t.at.append(' ').append((int) e.getX(i)).append(',').append((int) e.getY(i));
                }
            }
        }
    }

    private static String touchUse() {
        synchronized (rawTouches) {
            if (rawTouches.isEmpty()) return null;
            StringBuilder s = new StringBuilder("[touch]");
            for (java.util.Map.Entry<String, RawTouches> en : rawTouches.entrySet()) {
                RawTouches t = en.getValue();
                s.append(' ').append(en.getKey()).append(' ').append(t.events).append(" events, ").append(t.downs).append(" downs:").append(t.at).append(';');
            }
            rawTouches.clear();
            s.setLength(s.length() - 1);
            return s.toString();
        }
    }

    // A /proc or /sys file of a few lines. Null: not there, or not for an app to read. Not the 64 KB buffers of readText, every 10 s.
    private static String readShort(String path) {
        try (FileInputStream in = new FileInputStream(path)) {
            byte[] bytes = new byte[2048];
            int total = 0;
            for (int n; total < bytes.length && (n = in.read(bytes, total, bytes.length - total)) > 0; ) total += n;
            return new String(bytes, 0, total, "UTF-8").trim();
        } catch (Exception e) {
            return null;
        }
    }

    private static boolean swapsUnreadableSaid;

    /** Potato Mode: which swap devices fill, and in what order. Swap megabytes alone can't tell zram from the card. */
    private static String swapUse() {
        String text = readShort("/proc/swaps");
        if (text == null) {
            if (swapsUnreadableSaid) return null;
            swapsUnreadableSaid = true;
            return "[swaps] unreadable";
        }
        StringBuilder s = new StringBuilder("[swaps]");
        String[] lines = text.split("\n");
        for (int i = 1; i < lines.length; i++) {  // the first is the heading: Filename Type Size Used Priority, sizes in KB
            String[] w = lines[i].trim().split("\\s+");
            if (w.length < 5) continue;
            try {
                s.append(String.format(Locale.US, " %s used %d of %d MB priority %s,", w[0], Long.parseLong(w[3]) >> 10, Long.parseLong(w[2]) >> 10, w[4]));
            } catch (NumberFormatException e) { /* not this line */ }
        }
        if (s.length() == "[swaps]".length()) return "[swaps] none";
        s.setLength(s.length() - 1);
        return s.toString();
    }

    // [pressure]: one buffer for the reports thread, read into again for each file. /proc/vmstat runs past 2 KB before pswpin.
    private static final byte[] procBytes = new byte[8192];
    private static int procLength;
    private static boolean psiUnreadable, vmstatUnreadable;
    private static long pswpinBefore = -1, pswpoutBefore = -1, vmstatAt;

    private static boolean readProc(String path) {
        procLength = 0;
        try (FileInputStream in = new FileInputStream(path)) {
            for (int n; procLength < procBytes.length && (n = in.read(procBytes, procLength, procBytes.length - procLength)) > 0; ) procLength += n;
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /** Where the line starting with key ends its key in what readProc read, -1 if no line does. */
    private static int procKey(String key) {
        int k = key.length();
        for (int i = 0; i + k <= procLength; i++) {
            if (i > 0 && procBytes[i - 1] != '\n') continue;
            int j = 0;
            while (j < k && procBytes[i + j] == key.charAt(j)) j++;
            if (j == k) return i + k;
        }
        return -1;
    }

    /** The whole number after key (kB in most files), -1 if it's not there. */
    private static long procNumber(String key) {
        int p = procKey(key);
        if (p < 0) return -1;
        while (p < procLength && (procBytes[p] == ' ' || procBytes[p] == '\t')) p++;  // status has a tab
        long v = -1;
        for (; p < procLength && procBytes[p] >= '0' && procBytes[p] <= '9'; p++) v = (v < 0 ? 0 : v * 10) + (procBytes[p] - '0');
        return v;
    }

    /** The word after key, up to a space or the line's end: "?" if it's not there. */
    private static String procWord(String key) {
        int p = procKey(key);
        if (p < 0) return "?";
        int end = p;
        while (end < procLength && procBytes[end] != ' ' && procBytes[end] != '\n') end++;
        return new String(procBytes, p, end - p, java.nio.charset.StandardCharsets.US_ASCII);
    }

    /** The fields of a file read, named as the kernel does, in MB. */
    private static void procMb(StringBuilder s, String file, String[] keys) {
        if (!readProc(file)) {
            s.append(' ').append(file).append(" unreadable;");
            return;
        }
        for (String key : keys) {
            long kb = procNumber(key);
            s.append(' ').append(key, 0, key.length() - 1).append(' ');
            if (kb < 0) s.append('?');
            else s.append(kb >> 10);
        }
        s.append(" MB;");
    }

    private static final String[] SMAPS_KEYS = {"Rss:", "Pss_Anon:", "Pss_File:", "Swap:", "SwapPss:"},
        STATUS_KEYS = {"RssAnon:", "RssFile:", "VmSwap:"}, MEMINFO_KEYS = {"MemAvailable:", "SwapFree:", "SwapCached:"};

    /**
     * Potato Mode: how hard memory is pressed, in the kernel's own field names and in MB. SwapPss leaves out the swapped zygote pages
     * every app is forked with, which Swap and VmSwap count. psi avg10: the share of the last 10 s some (or all) tasks waited on
     * memory. pswpin/pswpout: what the whole device swapped in and out since the last report. A file Android won't let an app read is
     * said once, then left alone.
     */
    private static synchronized String pressureUse() {  // one buffer: an activity made again in the same process has a second reports thread
        StringBuilder s = new StringBuilder(320).append("[pressure]");
        procMb(s, "/proc/self/smaps_rollup", SMAPS_KEYS);
        procMb(s, "/proc/self/status", STATUS_KEYS);
        procMb(s, "/proc/meminfo", MEMINFO_KEYS);
        if (!psiUnreadable) {
            if (readProc("/proc/pressure/memory")) s.append(" psi some ").append(procWord("some avg10=")).append(" full ").append(procWord("full avg10=")).append(';');
            else { psiUnreadable = true; s.append(" psi unreadable;"); }
        }
        if (!vmstatUnreadable) {
            if (readProc("/proc/vmstat")) {
                long in = procNumber("pswpin "), out = procNumber("pswpout "), now = android.os.SystemClock.elapsedRealtime();
                if (pswpinBefore >= 0 && in >= 0 && out >= 0) {
                    double mbPerPage = Os.sysconf(android.system.OsConstants._SC_PAGESIZE) / 1048576.0;
                    s.append(String.format(Locale.US, " pswpin +%.1f pswpout +%.1f MB in %.0f s;", (in - pswpinBefore) * mbPerPage, (out - pswpoutBefore) * mbPerPage, (now - vmstatAt) / 1000.0));
                } else if (in < 0 || out < 0) s.append(" pswpin/pswpout not in vmstat;");
                pswpinBefore = in;
                pswpoutBefore = out;
                vmstatAt = now;
            } else { vmstatUnreadable = true; s.append(" vmstat unreadable;"); }
        }
        s.setLength(s.length() - 1);
        return s.toString();
    }

    private static ArrayList<String[]> clockFiles;  // a name, its frequency's file, its governor's
    private static boolean clocksUnreadableSaid;

    /** Potato Mode: CPU, GPU and memory clocks, as far as this firmware lets an app read them. What it won't isn't asked again. */
    private static String clockUse() {
        if (clockFiles == null) {
            clockFiles = new ArrayList<>();
            for (int cpu : new int[] {0, 4, 7}) {
                String folder = "/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/";
                clockFiles.add(new String[] {"cpu" + cpu, folder + "scaling_cur_freq", folder + "scaling_governor"});
            }
            File[] devices = new File("/sys/class/devfreq").listFiles();
            if (devices != null) {
                for (int i = 0; i < devices.length && i < 6; i++) {
                    clockFiles.add(new String[] {devices[i].getName(), devices[i].getPath() + "/cur_freq", devices[i].getPath() + "/governor"});
                }
            } else {  // not listable: the RK3566's own names
                clockFiles.add(new String[] {"gpu", "/sys/class/devfreq/fde60000.gpu/cur_freq", "/sys/class/devfreq/fde60000.gpu/governor"});
                clockFiles.add(new String[] {"ddr", "/sys/class/devfreq/dmc/cur_freq", "/sys/class/devfreq/dmc/governor"});
            }
        }
        StringBuilder s = new StringBuilder("[clocks]");
        for (java.util.Iterator<String[]> it = clockFiles.iterator(); it.hasNext(); ) {
            String[] clock = it.next();
            String value = readShort(clock[1]);
            long mhz;
            try {
                mhz = Long.parseLong(value) / (clock[0].startsWith("cpu") ? 1000 : 1000000);  // kHz, and devfreq's Hz
            } catch (NumberFormatException e) {  // null too
                it.remove();
                continue;
            }
            String governor = readShort(clock[2]);
            s.append(' ').append(clock[0]).append(' ').append(mhz).append(" MHz");
            if (governor != null) s.append(' ').append(governor);
            s.append(',');
        }
        if (clockFiles.isEmpty()) {
            if (clocksUnreadableSaid) return null;
            clocksUnreadableSaid = true;
            return "[clocks] unreadable";
        }
        s.setLength(s.length() - 1);
        return s.toString();
    }

    private static final String[] OS_WORDS = {"gamma", "dualstack", "nano", "performance", "perf_mode", "perfmode", "storagemedia",
        "ro.build.display.id", "ro.build.version.incremental", "ro.product.name", "ro.product.system.name"};

    private static boolean osWord(String text) {
        String lower = text.toLowerCase(Locale.US);
        for (String word : OS_WORDS) if (lower.contains(word)) return true;
        return false;
    }

    /** Potato Mode, once a launch: which OS build this is, and what it says of its performance modes. Firmware names them as it likes, hence by keyword. */
    private void reportOs(PowerManager power) {
        report("[os] " + Build.FINGERPRINT + ", kernel " + System.getProperty("os.version") + ", power save " + power.isPowerSaveMode());
        if (Build.VERSION.SDK_INT >= 31) {
            try {
                android.app.GameManager games = (android.app.GameManager) getSystemService(Context.GAME_SERVICE);
                if (games != null) report("[os] game mode " + games.getGameMode());
            } catch (Exception e) { report("[os] game mode: " + e); }
        }
        int shown = 0;
        try {
            Process getprop = new ProcessBuilder("/system/bin/getprop").redirectErrorStream(true).start();
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(getprop.getInputStream()))) {
                for (String line; (line = r.readLine()) != null; ) {
                    if (shown < 30 && osWord(line)) {
                        shown++;
                        report("[os] " + (line.length() > 120 ? line.substring(0, 120) : line));
                    }
                }
            }
            getprop.destroy();
        } catch (Exception e) { report("[os] getprop: " + e); }
        for (Uri table : new Uri[] {android.provider.Settings.System.CONTENT_URI, android.provider.Settings.Global.CONTENT_URI, android.provider.Settings.Secure.CONTENT_URI}) {
            try (Cursor c = getContentResolver().query(table, new String[] {"name", "value"}, null, null, null)) {
                while (c != null && c.moveToNext()) {
                    String name = c.getString(0);
                    if (name != null && shown < 50 && osWord(name)) {
                        shown++;
                        report("[os] " + table.getLastPathSegment() + " " + name + " = " + c.getString(1));
                    }
                }
            } catch (Exception e) { report("[os] " + table.getLastPathSegment() + " settings: " + e.getClass().getSimpleName()); }
        }
    }

    private static final java.util.HashMap<String, Long> threadTicks = new java.util.HashMap<>();
    private static final java.util.HashMap<String, Long> threadFaults = new java.util.HashMap<>();
    private static long threadsAt;

    /** Potato Mode: which threads the CPU went to since the last report, as a share of one core, each with its tid, its state and any major faults. */
    private static String threadUse() {
        File[] tasks = new File("/proc/self/task").listFiles();
        if (tasks == null) return "[threads] unreadable";
        long now = android.os.SystemClock.elapsedRealtime();
        double secs = threadsAt != 0 ? (now - threadsAt) / 1000.0 : 0;
        threadsAt = now;
        java.util.TreeMap<Long, String> top = new java.util.TreeMap<>(java.util.Collections.reverseOrder());
        long total = 0, allFaults = 0;
        StringBuilder stillFaulting = new StringBuilder();  // no CPU to show for it: a thread waiting on the disk may be the whole story
        for (File t : tasks) {
            try {
                String stat = new String(java.nio.file.Files.readAllBytes(new File(t, "stat").toPath()));
                int open = stat.indexOf('('), close = stat.lastIndexOf(')');
                String[] w = stat.substring(close + 2).split(" ");
                long ticks = Long.parseLong(w[11]) + Long.parseLong(w[12]);  // utime, stime
                long faults = Long.parseLong(w[9]);  // majflt
                Long faultsBefore = threadFaults.put(t.getName(), faults);
                long newFaults = faultsBefore != null ? faults - faultsBefore : 0;
                allFaults += newFaults;
                String about = stat.substring(open + 1, close) + "(" + t.getName() + " " + w[0] + (newFaults > 0 ? " mf" + newFaults : "") + ")";
                Long before = threadTicks.put(t.getName(), ticks);
                if (before != null && ticks > before) {
                    long used = ticks - before;
                    total += used;
                    top.put(used * 1000 + Long.parseLong(t.getName()) % 1000, about);
                } else if (newFaults > 0) {
                    stillFaulting.append(' ').append(about);
                }
            } catch (Exception e) { /* a thread that just ended */ }
        }
        if (secs <= 0) return "[threads] counting";
        StringBuilder s = new StringBuilder(String.format(Locale.US, "[threads] %.0f s, all %.0f%%, %d major faults:", secs, total / secs, allFaults));
        int shown = 0;
        for (java.util.Map.Entry<Long, String> en : top.entrySet()) {
            if (++shown > 9 && !en.getValue().contains(" mf")) continue;
            s.append(String.format(Locale.US, " %s %.0f%%", en.getValue(), (en.getKey() / 1000) / secs));
        }
        return s.append(stillFaulting).toString();
    }

    /** Settings → Two Screens (Potato): remembered for the next launch (BlitzLauncher.twoScreens). */
    public static void setPotatoTwoScreens(boolean on) {
        BlitzActivity a = current;
        if (a == null) return;
        a.getSharedPreferences(BlitzLauncher.PREFS, Context.MODE_PRIVATE).edit().putBoolean(BlitzLauncher.POTATO_TWO, on).apply();
        report("[device] potato two screens: " + on + ", from the next launch");
    }

    /** Settings → Potato Mode: remembered for the next launch (BlitzLauncher.lean). */
    public static void setPotato(boolean on) {
        BlitzActivity a = current;
        if (a == null) return;
        a.getSharedPreferences(BlitzLauncher.PREFS, Context.MODE_PRIVATE).edit().putBoolean(BlitzLauncher.POTATO, on).apply();
        report("[device] potato mode: " + on + ", from the next launch");
    }

    /** Settings → Swap Screens: remembered for BlitzLauncher, which flips which screen is the upper one at the next launch. */
    public static void setSwapScreens(boolean swap) {
        BlitzActivity a = current;
        if (a == null) return;
        a.getSharedPreferences(BlitzLauncher.PREFS, Context.MODE_PRIVATE).edit().putBoolean(BlitzLauncher.SWAP, swap).apply();
        report("[ds] swap screens: " + swap + ", from the next launch");
    }

    static boolean isRunning() { return current != null; }

    // Test builds: the Controller Paks go up once a launch, so a crash that depends on a save can be reproduced.
    private void uploadPaks(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            if (f.isDirectory()) uploadPaks(f);
            else if (f.getName().endsWith(".mpk")) upload(f, "pak-" + f.getParentFile().getName(), false);
        }
    }

    /** Settings → Send Logs: up to the developer. No share sheet: without a touch screen, nothing can close it and the game is stuck behind it. */
    public static void sendLogs(final String path) {
        final BlitzActivity a = current;
        if (a == null) return;
        new Thread(() -> {
            final boolean sent = a.upload(new File(path), "logs", false);
            report("[logs] send logs: " + (sent ? "sent" : "not sent"));
            a.runOnUiThread(() -> android.widget.Toast.makeText(a, sent ? "Logs sent" : "Logs not sent. Check Wi-Fi and try again.", android.widget.Toast.LENGTH_LONG).show());
        }, "BlitzSendLogs").start();
    }
}
