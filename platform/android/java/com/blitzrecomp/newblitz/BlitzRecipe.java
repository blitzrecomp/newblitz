package com.blitzrecomp.newblitz;

import android.content.Context;
import android.os.SystemClock;
import android.system.Os;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/**
 * Test builds in Potato Mode: an experiment with no install and no trip to Settings (docs/BLITZ_DS.md). A short text at the log
 * Worker, res/recipe, is read once at launch and sets that one run, ahead of what Settings saved and never written back to it:
 *   name=run1 two=1 scale=1.5 frame_copy=0 barriers=full flat1x=0 ram_panels=0 texmode=x evict=120 evictfix=1 depthclamp=legacy
 * Each key reaches native code as BLITZ_RECIPE_KEY. An unknown key is logged and skipped. No answer in time, or a text that isn't
 * a recipe, is a run on the saved settings, and says so on screen, so no name means none is posted. A Retroid out of Potato Mode,
 * or a build without an upload address, never asks.
 */
final class BlitzRecipe {
    /** On the game's intent: the launcher asked, in this process, for the game it's starting. */
    static final String LAUNCHER_ASKED = "blitz_recipe_asked";
    private static final String TAG = "BlitzRecomp";
    private static final long WAIT_MS = 1500;
    private static final long FRESH_MS = 15000;  // an answer still good for a start that didn't come through the launcher
    private static final int MOST_BYTES = 4096;
    /** Key: the values it can take. A value outside them makes the whole text "not a recipe": half an experiment misleads. */
    private static final Map<String, String> KEYS = new LinkedHashMap<>();
    static {
        KEYS.put("name", "[A-Za-z0-9._-]{1,40}");
        KEYS.put("two", "[01]");
        KEYS.put("scale", "1|1\\.25|1\\.5|1\\.75|2");
        KEYS.put("frame_copy", "[012]");
        KEYS.put("barriers", "narrow|full");
        KEYS.put("flat1x", "[01]");
        KEYS.put("ram_panels", "[01]");
        KEYS.put("texmode", "[A-Za-z0-9_]{1,20}");
        KEYS.put("evict", "[0-9]{1,6}");
        KEYS.put("evictfix", "[01]");
        KEYS.put("depthclamp", "spec|legacy");
    }

    private static Map<String, String> values = new HashMap<>();
    private static String logLine, showLine;
    private static long askedAt;

    private BlitzRecipe() {}

    /** This run's two= (what BlitzLauncher.twoScreens answers), null if nothing says. */
    static Boolean two() {
        String two = values.get("two");
        return two == null ? null : Boolean.valueOf(two.equals("1"));
    }

    /**
     * Called on the main thread, from a launcher that has to finish in onCreate, so it waits here, at most WAIT_MS. The asking is on
     * its own thread: DNS can outlast any timeout, and an answer after the wait is dropped with the thread.
     */
    static void fetch(Context context) {
        values = new HashMap<>();
        logLine = showLine = null;
        askedAt = SystemClock.elapsedRealtime();
        if (!BlitzLauncher.lean(context)) return;
        String base = BlitzRemote.base(context);
        if (base.isEmpty()) return;
        final String address = base + "/res/recipe";
        FutureTask<String> task = new FutureTask<>(() -> get(address));
        Thread t = new Thread(task, "BlitzRecipe");
        t.setDaemon(true);
        t.start();
        String text;
        try {
            text = task.get(WAIT_MS, TimeUnit.MILLISECONDS);
        } catch (TimeoutException e) {
            task.cancel(true);
            logLine = "none: no answer in " + WAIT_MS + " ms, the saved settings";
            showLine = "Recipe: none (no answer)";
            return;
        } catch (Exception e) {
            logLine = "none: " + e.getCause() + ", the saved settings";
            showLine = "Recipe: none (no answer)";
            return;
        }
        read(text);
    }

    private static String get(String address) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(address).openConnection();
        try {
            c.setConnectTimeout((int) WAIT_MS);
            c.setReadTimeout((int) WAIT_MS);
            c.setUseCaches(false);
            int code = c.getResponseCode();
            if (code == 404) return "";  // none posted yet
            if (code != 200) throw new IOException("HTTP " + code);
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[1024];
                for (int n; b.size() <= MOST_BYTES && (n = in.read(buf)) > 0; ) b.write(buf, 0, n);
            }
            return b.toString("UTF-8");
        } finally {
            c.disconnect();
        }
    }

    private static void read(String text) {
        text = text.trim();
        if (text.isEmpty()) {  // an empty post is how one's taken away
            logLine = "none posted, the saved settings";
            return;
        }
        Map<String, String> found = new HashMap<>();
        StringBuilder ignored = new StringBuilder();
        boolean good = text.length() <= MOST_BYTES;
        if (good) {
            for (String pair : text.split("[\\s;,]+")) {
                int eq = pair.indexOf('=');
                if (eq <= 0) { good = false; break; }
                String key = pair.substring(0, eq).toLowerCase(Locale.US), value = pair.substring(eq + 1);
                String allowed = KEYS.get(key);
                if (allowed == null) {
                    ignored.append(' ').append(key.replaceAll("[^a-z0-9._-]", "?"));
                    continue;
                }
                if (!value.matches(allowed)) { good = false; break; }
                found.put(key, value);
            }
        }
        if (!good) {
            logLine = "not understood, the saved settings";
            showLine = "Recipe: not understood";
            return;
        }
        if (!found.containsKey("name")) found.put("name", "unnamed");
        values = found;
        StringBuilder line = new StringBuilder(found.get("name")).append(", this run only:");
        for (String key : KEYS.keySet()) {
            if (!key.equals("name") && found.containsKey(key)) line.append(' ').append(key).append('=').append(found.get(key));
        }
        if (ignored.length() > 0) line.append(", unknown keys ignored:").append(ignored);
        logLine = line.toString();
        showLine = "Recipe: " + found.get("name");
    }

    /**
     * Before the library's loaded (BlitzActivity.onCreate). Started some other way than the launcher, it asks now. Through the
     * launcher, however long the unpacking took, its answer stands, since it's already chosen the screen from it (and a second ask
     * could fail and lose the recipe). askedAt: Android can bring the same intent back in a new process, which hasn't asked. A safe
     * start (BlitzActivity.safeStartCheck) runs without one, whatever's posted: a bad recipe may be why it's needed.
     */
    static void forThisRun(Context context, boolean safeStart, boolean launcherAsked) throws Exception {
        if (safeStart) {
            values = new HashMap<>();
            showLine = null;
            logLine = BlitzRemote.base(context).isEmpty() ? null : "none: a safe start runs without one";
            askedAt = SystemClock.elapsedRealtime();
        } else if (!(launcherAsked && askedAt != 0) && SystemClock.elapsedRealtime() - askedAt > FRESH_MS) {
            fetch(context);
        }
        for (String key : KEYS.keySet()) {
            String name = "BLITZ_RECIPE_" + key.toUpperCase(Locale.US), value = values.get(key);
            if (value != null) Os.setenv(name, value, true);
            else Os.unsetenv(name);
        }
        // Native code logs it, in blitz.log once the settings are up, and shows it.
        if (logLine != null) {
            Log.i(TAG, "[recipe] " + logLine);
            Os.setenv("BLITZ_RECIPE_LOG", logLine, true);
        } else {
            Os.unsetenv("BLITZ_RECIPE_LOG");
        }
        if (showLine != null) Os.setenv("BLITZ_RECIPE_SHOW", showLine, true);
        else Os.unsetenv("BLITZ_RECIPE_SHOW");
    }
}
