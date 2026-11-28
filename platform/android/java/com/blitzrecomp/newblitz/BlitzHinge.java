package com.blitzrecomp.newblitz;

import android.content.Context;
import android.graphics.Rect;
import android.os.Build;
import android.view.Display;

import java.util.List;

/**
 * A dual-screen phone's hinge (Surface Duo): one display across both panels, with a band in the middle that shows nothing.
 * Spanned and held like a DS (hinge across), the window is taller than wide and the band runs side to side; held like a book,
 * it runs top to bottom. Nothing here assumes a device list beyond the fallback sizes: where the hinge is comes from
 * Microsoft's DisplayMask when the system has it (uses-library, not required), else the Duo's known panel sizes.
 * BLITZ_HINGE=left,top,right,bottom (screen pixels, test_env.txt) stands in for both. docs/SURFACE_DUO.md.
 */
final class BlitzHinge {
    private BlitzHinge() {}

    static final String FEATURE = "com.microsoft.device.display.displaymask";
    static String source = "none";

    /** A Surface Duo, or a test run that says it has a hinge. */
    static boolean present(Context c) {
        if (override() != null) return true;
        if (c.getPackageManager().hasSystemFeature(FEATURE)) return true;
        return "Microsoft".equalsIgnoreCase(Build.MANUFACTURER) && Build.MODEL != null && Build.MODEL.toLowerCase().contains("duo");
    }

    private static Rect override() {
        String s = System.getenv("BLITZ_HINGE");
        if (s == null || !s.matches("\\d+,\\d+,\\d+,\\d+")) return null;
        String[] p = s.split(",");
        return new Rect(Integer.parseInt(p[0]), Integer.parseInt(p[1]), Integer.parseInt(p[2]), Integer.parseInt(p[3]));
    }

    /** The hinge in screen pixels for the display's rotation now, or null when there's none (or it can't be found). */
    static Rect onScreen(Context c, Display d) {
        Rect r = override();
        if (r != null) { source = "BLITZ_HINGE"; return r; }
        // Microsoft's own: com.microsoft.device.display.DisplayMask, by reflection so the build needs nothing of it.
        try {
            Class<?> mask = Class.forName("com.microsoft.device.display.DisplayMask");
            Object m = mask.getMethod("fromResourcesRectApproximation", Context.class).invoke(null, c);
            @SuppressWarnings("unchecked")
            List<Rect> rects = (List<Rect>) mask.getMethod("getBoundingRectsForRotation", int.class).invoke(m, d.getRotation());
            if (rects != null && !rects.isEmpty() && !rects.get(0).isEmpty()) { source = "DisplayMask"; return new Rect(rects.get(0)); }
        } catch (Throwable t) {
            source = "DisplayMask unavailable (" + t.getClass().getSimpleName() + ")";
        }
        // The Duos' panels: Duo 1350x1800 with an 84-pixel hinge, Duo 2 1344x1892 with 66. Spanned, the display is both.
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        d.getRealMetrics(m);
        int w = m.widthPixels, h = m.heightPixels;
        int[][] known = { { 1350, 1800, 84 }, { 1344, 1892, 66 } };
        for (int[] k : known) {
            int panel = k[0], other = k[1], gap = k[2];
            if (w == 2 * panel + gap && h == other) { source = "known size"; return new Rect(panel, 0, panel + gap, h); }
            if (h == 2 * panel + gap && w == other) { source = "known size"; return new Rect(0, panel, w, panel + gap); }
        }
        return null;
    }

    static String describe(Rect r) {
        return r == null ? "none" : r.left + "," + r.top + " to " + r.right + "," + r.bottom + " (" + r.width() + "x" + r.height() + ", " + source + ")";
    }
}
