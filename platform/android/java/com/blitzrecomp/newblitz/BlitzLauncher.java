package com.blitzrecomp.newblitz;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;

/**
 * The launcher icon. Puts the game on display 0: its menus live on whichever screen it starts on, and that can only be
 * chosen at launch. The game then finds the other screen itself, and which of the two is on top (firstIsUpper).
 */
public class BlitzLauncher extends Activity {
    static final String PREFS = "blitz_ds", SWAP = "swap_screens", POTATO = "potato_mode", POTATO_TWO = "potato_two_screens_on", NO_FRAME = "starts_without_a_frame";

    /** About 1 GB of RAM (an RG DS Plus), or Settings → Potato Mode: original quality limits, no extras, straight in. */
    static boolean lean(Context context) {
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ((ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(memory);
        return (memory.totalMem >> 20) < 1500 || context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(POTATO, false);
    }

    /** A ROM, stored or waiting to be imported, in the app folder (Save Logs' zips aren't). */
    static boolean hasRom(Context context) {
        File[] files = context.getExternalFilesDir(null) != null ? context.getExternalFilesDir(null).listFiles() : null;
        if (files == null) return false;
        for (File f : files) {
            String n = f.getName().toLowerCase();
            if (n.endsWith(".z64") || n.endsWith(".n64") || n.endsWith(".v64")) return true;
            if (n.endsWith(".zip") && !n.startsWith("blitz logs")) return true;
        }
        return false;
    }

    /** GammaOS names itself here, on an image whose device name is generic ("GammaOS Core" in every probe of an RG DS). */
    static boolean gammaOs() {
        return Build.MODEL != null && Build.MODEL.startsWith("GammaOS");
    }

    /** Every display: the list, and any a firmware names only as a place for a Presentation. */
    static List<Display> allDisplays(Context context) {
        DisplayManager displays = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        ArrayList<Display> all = new ArrayList<>(Arrays.asList(displays.getDisplays()));
        for (Display extra : displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            boolean listed = false;
            for (Display d : all) listed |= d.getDisplayId() == extra.getDisplayId();
            if (!listed) {
                Log.i("BlitzRecomp", "[ds] display " + extra.getDisplayId() + " is only listed for a presentation");
                all.add(extra);
            }
        }
        return all;
    }

    // Is display 0 the upper screen? Only what a device has shown goes here, for where pixels would get it wrong. An RG DS: its
    // equal panels would tie anyway, and display 0 is the lower, with the touch.
    private static final HashMap<String, Boolean> DEFAULT_IS_UPPER = new HashMap<>();
    static { DEFAULT_IS_UPPER.put("GammaOS Core", false); }

    private static long pixels(Display d) {
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        d.getRealMetrics(m);
        return (long) m.widthPixels * m.heightPixels;
    }

    /**
     * Is the game's own screen the upper one? Android doesn't say. By model, then by pixels (the bigger is on top, on the Retroid
     * Pocket Duo and the Thor), and a tie means display 0 is the lower. Never dpi, which is wrong on both devices measured.
     * Pixels only with two touch panels, which those have: a monitor has none, and a smaller one mustn't take play select away
     * from the only screen that can be tapped. Settings → Swap Screens flips the answer, for a device this gets wrong.
     */
    static boolean firstIsUpper(Context context, Display first, Display other) {
        boolean firstIsDefault = first.getDisplayId() == Display.DEFAULT_DISPLAY;
        Boolean defaultIsUpper = DEFAULT_IS_UPPER.get(Build.MODEL);
        long firstPixels = pixels(first), otherPixels = pixels(other);
        boolean upper;
        if (defaultIsUpper != null) upper = defaultIsUpper.booleanValue() == firstIsDefault;
        else if (firstPixels != otherPixels && BlitzActivity.touchPanels() >= 2) upper = firstPixels > otherPixels;
        else upper = !firstIsDefault;
        return upper != context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(SWAP, false);
    }

    /**
     * With two screens, display 0, wherever the launcher itself lives (GammaOS's is on the top screen of an RG DS, and a game
     * there would have to take the bottom with an activity, which takes its focus).
     */
    static Bundle launchOptions(Context context) {
        int display = launchDisplay(context);
        if (display < 0) return null;
        Log.i("BlitzRecomp", "[ds] launching on display " + display);
        return ActivityOptions.makeBasic().setLaunchDisplayId(display).toBundle();
    }

    /** Potato Mode on two screens uses both unless Settings → Two Screens (Potato) is off (on by default since 2026-11-15). */
    static boolean twoScreens(Context context) {
        if (!lean(context)) return true;
        Boolean recipe = BlitzRecipe.two();  // this run's, ahead of the setting and not saved over it
        return recipe != null ? recipe : context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(POTATO_TWO, true);
    }

    /** Potato Mode, the last two launches never drew: this one is a safe start (BlitzActivity.safeStartCheck). */
    static boolean safeStartDue(Context context) {
        return lean(context) && context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(NO_FRAME, 0) >= 2;
    }

    /** The screen the game goes on, -1 with one screen. */
    static int launchDisplay(Context context) {
        if (!twoScreens(context) || safeStartDue(context)) return -1;
        for (Display d : allDisplays(context)) {
            if (d.getDisplayId() != Display.DEFAULT_DISPLAY && d.getState() != Display.STATE_OFF) return Display.DEFAULT_DISPLAY;
        }
        return -1;
    }

    /**
     * With two screens, the lower one: the touch screen on an RG DS (display 0 there), and on a device with two, where setup's
     * buttons belong. By firstIsUpper, so Swap Screens fixes a device it gets wrong. -1 with one screen.
     */
    static int touchDisplay(Context context) {
        Display first = null, other = null;
        for (Display d : allDisplays(context)) {
            if (d.getState() == Display.STATE_OFF) continue;
            if (d.getDisplayId() == Display.DEFAULT_DISPLAY) first = d;
            else if (other == null) other = d;
        }
        if (first == null || other == null) return -1;
        return (firstIsUpper(context, first, other) ? other : first).getDisplayId();
    }

    /** Setup's ROM screen (BlitzRomFirst), on the touch screen when there are two; the game later goes where this was opened. */
    private void startRomFirst() {
        Intent rom = new Intent(this, BlitzRomFirst.class);
        int here = getWindowManager().getDefaultDisplay().getDisplayId();
        rom.putExtra(BlitzRomFirst.GAME_DISPLAY, here);
        int touch = touchDisplay(this);
        if (touch < 0 || touch == here) {
            startActivity(rom);
            return;
        }
        // A task of its own (BlitzRomFirst's own affinity, in the manifest): a display other than the launcher's.
        rom.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            Log.i("BlitzRecomp", "[ds] setup's ROM screen on display " + touch + ", the lower one");
            startActivity(rom, ActivityOptions.makeBasic().setLaunchDisplayId(touch).toBundle());
        } catch (Exception e) {
            Log.w("BlitzRecomp", "[ds] couldn't put setup's ROM screen on display " + touch + ": " + e);
            startActivity(rom);
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // The picker in front of a running game gets the game killed there: pick first, with none of it loaded.
        if (lean(this) && !BlitzActivity.isRunning() && !hasRom(this)) {
            startRomFirst();
            finish();
            return;
        }
        // A run recipe can say how many screens, so it's asked for before the screen's chosen: at most 1.5 s, here because a
        // launcher with no window has to finish in onCreate.
        final boolean askRecipe = !BlitzActivity.isRunning() && !safeStartDue(this);
        if (askRecipe) BlitzRecipe.fetch(this);
        Intent game = new Intent(this, BlitzActivity.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (askRecipe) game.putExtra(BlitzRecipe.LAUNCHER_ASKED, true);  // the screen's been chosen from this answer: it stands
        Bundle options = BlitzActivity.isRunning() ? null : launchOptions(this);  // running: launch options could move it
        try {
            startActivity(game, options);
        } catch (Exception e) {
            Log.w("BlitzRecomp", "[ds] couldn't launch on the other screen: " + e);
            startActivity(game);
        }
        finish();
    }
}
