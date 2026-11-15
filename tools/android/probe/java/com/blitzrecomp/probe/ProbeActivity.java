package com.blitzrecomp.probe;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * What a device can do for BLITZ DS, without the game: the screens Android reports, whether one Vulkan device can draw
 * to both and how fast, the graphics features RT64 leans on, memory, CPU and storage speed, and every touch and button
 * while it's open. The report goes up by itself once the tests finish, and Share sends it anywhere.
 */
public class ProbeActivity extends Activity {
    private final StringBuilder report = new StringBuilder();
    private final List<Surface> surfaces = new ArrayList<>();
    private final List<Presentation> presentations = new ArrayList<>();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView text, status;
    private ScrollView scroll;
    private int expectedSurfaces = 1;
    private boolean started;
    private volatile boolean finished;
    private long startMs;

    static { System.loadLibrary("blitzprobe"); }
    /** Native tests (probe.cpp). Each returns its report section. */
    static native void nativeTrace(String path);
    static native String nativeMachine();
    static native String nativeVulkan(Surface[] surfaces);

    private synchronized void log(String line) {
        report.append(line).append('\n');
        android.util.Log.i("BlitzProbe", line);
        ui.post(() -> { text.append(line + "\n"); scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN)); });
    }

    private void section(String name) { log("\n== " + name + " =="); }

    private void toggleBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    SurfaceView surfaceView(final String who) {
        SurfaceView v = new SurfaceView(this) {
            // Presentation contexts differ; the view is all a surface needs.
        };
        v.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {
                log("surface created: " + who);
            }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int height) {
                log("surface " + who + ": " + w + "x" + height + " format " + f);
                addSurface(h.getSurface());
            }
            @Override public void surfaceDestroyed(SurfaceHolder h) {
                log("surface destroyed: " + who);
            }
        });
        return v;
    }

    private synchronized void addSurface(Surface s) {
        if (!surfaces.contains(s)) surfaces.add(s);
        if (!started && surfaces.size() >= expectedSurfaces) { started = true; new Thread(this::runTests, "Probe").start(); }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        startMs = System.currentTimeMillis();
        toggleBars();
        FrameLayout root = new FrameLayout(this);
        root.addView(surfaceView("main"), new ViewGroup.LayoutParams(-1, -1));

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        status = new TextView(this);
        status.setTextColor(Color.YELLOW);
        status.setTextSize(13);
        status.setBackgroundColor(0xCC000000);
        status.setPadding(12, 6, 12, 6);
        status.setText("Testing. Keep this open.");
        column.addView(status);
        scroll = new ScrollView(this);
        text = new TextView(this);
        text.setTextColor(Color.WHITE);
        text.setTextSize(10);
        text.setTypeface(android.graphics.Typeface.MONOSPACE);
        text.setBackgroundColor(0xB0000000);
        text.setPadding(12, 6, 12, 6);
        scroll.addView(text);
        column.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout buttons = new LinearLayout(this);
        Button send = new Button(this);
        send.setText("Send again");
        send.setOnClickListener(v -> new Thread(this::upload, "ProbeUp").start());
        Button share = new Button(this);
        share.setText("Share");
        share.setOnClickListener(v -> share());
        buttons.addView(send);
        buttons.addView(share);
        column.addView(buttons);
        root.addView(column, new ViewGroup.LayoutParams(-1, -1));
        setContentView(root);

        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        section("displays at launch");
        for (Display d : dm.getDisplays()) log(describe(d));
        Display here = getWindowManager().getDefaultDisplay();
        log("this activity is on display " + here.getDisplayId());
        Display[] presentable = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        log("presentation displays: " + presentable.length + " (a Presentation needs FLAG_PRESENTATION, flags bit 0x8; the game falls back to an activity)");
        for (Display d : dm.getDisplays()) {
            if (d.getDisplayId() == here.getDisplayId()) continue;
            expectedSurfaces++;
            showOn(d);
        }
        dm.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { log("display added: " + id); }
            @Override public void onDisplayRemoved(int id) { log("display removed: " + id); }
            @Override public void onDisplayChanged(int id) { log("display changed: " + id); }
        }, ui);
        // Don't wait forever for a second surface that never comes.
        ui.postDelayed(() -> {
            synchronized (this) {
                if (!started && !surfaces.isEmpty()) {
                    log("only " + surfaces.size() + " of " + expectedSurfaces + " surfaces after 6 s; testing those");
                    started = true;
                    new Thread(this::runTests, "Probe").start();
                }
            }
        }, 6000);
    }

    static ProbeActivity current;

    /** The second screen, where a Presentation is refused, as the game does it. */
    public static class ProbeSecond extends Activity {
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            setContentView(current.surfaceView("activity on display " + getWindowManager().getDefaultDisplay().getDisplayId()));
        }
    }

    private void showOn(final Display d) {
        try {
            Presentation p = new Presentation(this, d) {
                @Override protected void onCreate(Bundle b) {
                    super.onCreate(b);
                    FrameLayout f = new FrameLayout(getContext());
                    f.addView(surfaceView("display " + d.getDisplayId()), new ViewGroup.LayoutParams(-1, -1));
                    TextView label = new TextView(getContext());
                    label.setTextColor(Color.WHITE);
                    label.setTextSize(16);
                    label.setGravity(Gravity.CENTER);
                    label.setBackgroundColor(0x88000000);
                    label.setText("Display " + d.getDisplayId() + "\nTap here: taps are logged");
                    f.addView(label, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
                    setContentView(f);
                }
                @Override public boolean dispatchTouchEvent(MotionEvent e) { logTouch(e, d.getDisplayId()); return super.dispatchTouchEvent(e); }
                @Override public boolean dispatchKeyEvent(KeyEvent e) { logKey(e, d.getDisplayId()); return super.dispatchKeyEvent(e); }
            };
            p.show();
            presentations.add(p);
            log("presentation shown on display " + d.getDisplayId());
        } catch (Exception e) {
            log("presentation FAILED on display " + d.getDisplayId() + ": " + e + "; trying an activity there");
            try {
                current = this;
                startActivity(new Intent(this, ProbeSecond.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK),
                        android.app.ActivityOptions.makeBasic().setLaunchDisplayId(d.getDisplayId()).toBundle());
                log("activity opening on display " + d.getDisplayId());
            } catch (Exception e2) {
                log("activity FAILED on display " + d.getDisplayId() + ": " + e2);
                expectedSurfaces--;
            }
        }
    }

    private static String describe(Display d) {
        Display.Mode m = d.getMode();
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        d.getRealMetrics(dm);
        StringBuilder rates = new StringBuilder();
        for (Display.Mode x : d.getSupportedModes()) rates.append(x.getPhysicalWidth()).append('x').append(x.getPhysicalHeight()).append('@').append(x.getRefreshRate()).append(' ');
        return "display " + d.getDisplayId() + " \"" + d.getName() + "\" " + dm.widthPixels + "x" + dm.heightPixels + " dpi " + dm.densityDpi
                + " (" + dm.xdpi + "," + dm.ydpi + ") rotation " + d.getRotation() + " refresh " + d.getRefreshRate() + " state " + d.getState()
                + " flags 0x" + Integer.toHexString(d.getFlags()) + " mode " + m.getPhysicalWidth() + "x" + m.getPhysicalHeight() + " modes [" + rates.toString().trim() + "]";
    }

    private void machine() {
        section("device");
        log("build: " + Build.MANUFACTURER + " / " + Build.BRAND + " / " + Build.MODEL + " / " + Build.DEVICE + " / " + Build.PRODUCT);
        log("board " + Build.BOARD + " hardware " + Build.HARDWARE + " soc " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : "?"));
        log("android " + Build.VERSION.RELEASE + " sdk " + Build.VERSION.SDK_INT + " id " + Build.DISPLAY + " abis " + Arrays.toString(Build.SUPPORTED_ABIS));
        log("fingerprint " + Build.FINGERPRINT);
        ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        log(String.format(Locale.US, "ram total %d MB, available %d MB, low threshold %d MB, low %b, heap class %d MB (large %d)", mi.totalMem >> 20, mi.availMem >> 20, mi.threshold >> 20, mi.lowMemory, am.getMemoryClass(), am.getLargeMemoryClass()));
        log("low ram device: " + am.isLowRamDevice());
        File files = getFilesDir();
        StatFs st = new StatFs(files.getPath());
        log("storage: " + (st.getAvailableBytes() >> 20) + " MB free of " + (st.getTotalBytes() >> 20) + " MB");
        for (String path : new String[] { "/proc/meminfo" }) {
            try (BufferedReader r = new BufferedReader(new FileReader(path))) {
                for (String l; (l = r.readLine()) != null; ) if (l.matches("(MemTotal|MemAvailable|SwapTotal|SwapFree|CommitLimit).*")) log(l.replaceAll("\\s+", " "));
            } catch (Exception e) { log(path + ": " + e); }
        }
        section("feature flags");
        for (String f : new String[] { "android.hardware.vulkan.level", "android.hardware.vulkan.version", "android.hardware.touchscreen", "android.hardware.touchscreen.multitouch", "android.hardware.gamepad", "android.software.activities_on_secondary_displays", "android.hardware.type.watch" })
            log(f + ": " + getPackageManager().hasSystemFeature(f));
        section("input devices");
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null) log("input " + id + " \"" + d.getName() + "\" sources 0x" + Integer.toHexString(d.getSources()) + " vendor " + d.getVendorId() + " product " + d.getProductId() + " external " + d.isExternal());
            if (d != null) for (InputDevice.MotionRange r : d.getMotionRanges()) {
                if ((r.getSource() & InputDevice.SOURCE_JOYSTICK) != 0) log("  axis " + MotionEvent.axisToString(r.getAxis()) + " " + r.getMin() + " to " + r.getMax() + " flat " + r.getFlat());
            }
        }
    }

    private void storage() {
        section("storage speed");
        File f = new File(getCacheDir(), "speed.bin");
        byte[] chunk = new byte[1 << 20];
        new java.util.Random(1).nextBytes(chunk);
        try {
            long t0 = System.nanoTime();
            try (FileOutputStream o = new FileOutputStream(f)) { for (int i = 0; i < 64; i++) o.write(chunk); o.getFD().sync(); }
            long t1 = System.nanoTime();
            try (FileInputStream in = new FileInputStream(f)) { while (in.read(chunk) > 0) { } }
            long t2 = System.nanoTime();
            log(String.format(Locale.US, "64 MB: write %.1f MB/s, read %.1f MB/s (read may be cached)", 64e9 / (t1 - t0), 64e9 / (t2 - t1)));
        } catch (Exception e) { log("storage test: " + e); }
        f.delete();
    }

    private File trace() { return new File(getCacheDir(), "native-trace.txt"); }

    private static String readAll(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0; ) b.write(buf, 0, n);
            return b.toString("UTF-8");
        } catch (Exception e) { return ""; }
    }

    private void runTests() {
        // A driver can hang or kill the process: the trace of a run that died goes up with this one, and a run stuck
        // for a minute reports what it got so far.
        File tr = trace();
        if (tr.exists()) {
            section("the last run died during this");
            log(readAll(tr));
            tr.delete();
        }
        nativeTrace(tr.getPath());
        new Thread(() -> {
            try { Thread.sleep(60000); } catch (InterruptedException e) { return; }
            if (!finished) {
                section("STILL RUNNING after 60 s; the native trace so far");
                log(readAll(tr));
                upload();
            }
        }, "ProbeWatch").start();
        try {
            machine();
            section("native: cpu and memory");
            log(nativeMachine());
            storage();
            section("vulkan");
            Surface[] all;
            synchronized (this) { all = surfaces.toArray(new Surface[0]); }
            log(nativeVulkan(all));
        } catch (Throwable t) {
            log("TEST FAILED: " + android.util.Log.getStackTraceString(t));
        }
        finished = true;
        tr.delete();
        log("\ntests finished in " + (System.currentTimeMillis() - startMs) + " ms");
        upload();
        ui.post(() -> status.setText(status.getText() + "  Now tap every screen and press every button, then Send again."));
    }

    private File writeReport() throws Exception {
        File out = new File(getCacheDir(), "blitz-probe-" + Build.DEVICE + ".txt");
        try (FileOutputStream o = new FileOutputStream(out)) {
            synchronized (this) { o.write(report.toString().getBytes("UTF-8")); }
        }
        return out;
    }

    private void upload() {
        try {
            File f = writeReport();
            String address = getString(getResources().getIdentifier("upload", "string", getPackageName()));
            HttpURLConnection c = (HttpURLConnection) new URL(address + "?device=" + Uri.encode(Build.DEVICE) + "&kind=probe").openConnection();
            c.setDoOutput(true);
            c.setRequestMethod("POST");
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setFixedLengthStreamingMode((int) f.length());
            try (OutputStream o = c.getOutputStream(); FileInputStream in = new FileInputStream(f)) {
                byte[] b = new byte[1 << 16];
                for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            }
            int code = c.getResponseCode();
            log("upload: HTTP " + code);
            final String msg = code == 200 ? "Sent to Claude." : "Upload failed (HTTP " + code + "). Use Share.";
            ui.post(() -> status.setText(msg));
        } catch (Exception e) {
            log("upload failed: " + e);
            ui.post(() -> status.setText("Upload failed, no internet? Use Share."));
        }
    }

    private void share() {
        try {
            File f = writeReport();
            Uri uri = Uri.parse("content://com.blitzrecomp.probe.files/" + f.getName());
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType("text/plain");
            i.putExtra(Intent.EXTRA_STREAM, uri);
            i.putExtra(Intent.EXTRA_SUBJECT, "Blitz probe: " + Build.DEVICE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(i, "Send the report"));
        } catch (Exception e) { log("share failed: " + e); }
    }

    private void logTouch(MotionEvent e, int display) {
        int a = e.getActionMasked();
        if (a != MotionEvent.ACTION_DOWN && a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_POINTER_DOWN) return;
        InputDevice d = e.getDevice();
        log(String.format(Locale.US, "touch %s display %d at %.0f,%.0f raw %.0f,%.0f pointers %d device \"%s\" tool %d", MotionEvent.actionToString(a), display, e.getX(), e.getY(), e.getRawX(), e.getRawY(), e.getPointerCount(), d == null ? "?" : d.getName(), e.getToolType(0)));
    }

    private void logKey(KeyEvent e, int display) {
        InputDevice d = e.getDevice();
        log("key " + KeyEvent.keyCodeToString(e.getKeyCode()) + (e.getAction() == KeyEvent.ACTION_DOWN ? " down" : " up") + " device \"" + (d == null ? "?" : d.getName()) + "\" display " + display);
    }

    @Override public boolean dispatchTouchEvent(MotionEvent e) { logTouch(e, getWindowManager().getDefaultDisplay().getDisplayId()); return super.dispatchTouchEvent(e); }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getRepeatCount() == 0) logKey(e, getWindowManager().getDefaultDisplay().getDisplayId());
        return super.dispatchKeyEvent(e);
    }
    private String lastAxes = "";
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (e.getAction() == MotionEvent.ACTION_MOVE && (e.getSource() & InputDevice.SOURCE_JOYSTICK) != 0 && e.getDevice() != null) {
            StringBuilder b = new StringBuilder();
            for (InputDevice.MotionRange r : e.getDevice().getMotionRanges()) {
                float v = e.getAxisValue(r.getAxis());
                if (Math.abs(v) > 0.5f) b.append(String.format(Locale.US, " %s=%.1f", MotionEvent.axisToString(r.getAxis()).replace("AXIS_", ""), v));
            }
            String moved = b.toString();
            if (!moved.isEmpty() && !moved.equals(lastAxes)) log("axes" + moved + " device \"" + e.getDevice().getName() + "\"");
            lastAxes = moved;
        }
        return super.dispatchGenericMotionEvent(e);
    }
}
