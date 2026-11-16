package com.blitzrecomp.newblitz;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.InputStream;
import java.util.HashMap;

/**
 * The Blitz '27 look, for the screens Java draws before the game's own UI is up (BlitzRomFirst): the tokens of
 * src/game/ui_theme.cpp, the framed panel of src/main/ui_kit.cpp (its own 9-slice art, from the APK's res/assets/ui) and the
 * layout of src/main/firstrun_screen.cpp. Sizes are the design's px at 960x720, as there, scaled to the screen.
 */
final class BlitzLook {
    // ui_theme.cpp
    static final int VEIL = Color.rgb(4, 10, 26);           // firstrun_screen.cpp's root
    static final int DEEP_NAVY = Color.rgb(13, 26, 58);     // Background3: idle buttons
    static final int TEXT = Color.rgb(242, 242, 242);
    static final int STEEL = Color.rgb(184, 196, 218);      // TextDim
    static final int TEAL = Color.rgb(143, 239, 245);       // the eyebrow, the cyan rim
    static final int RIM_SOFT = Color.argb(61, 143, 239, 245);  // BorderSoft
    static final int YELLOW = Color.rgb(255, 225, 74);      // SecondaryL: titles, the focused button
    static final int GOLD = Color.rgb(246, 212, 0);         // notes
    static final int RED = Color.rgb(179, 16, 30);          // Primary: the focus bar
    static final int RED_DARK = Color.rgb(110, 8, 18);      // PrimaryD: the primary button, idle

    private static final String ASSETS = "res/assets/";     // the game's resources in the APK (android_stage.cmake)

    final Context context;
    final float s;  // screen px per design px
    final Typeface display, label, body, bodyBold;
    private final HashMap<String, Bitmap> art = new HashMap<>();

    BlitzLook(Context context) {
        this.context = context;
        DisplayMetrics m = context.getResources().getDisplayMetrics();
        int w = Math.max(m.widthPixels, m.heightPixels), h = Math.min(m.widthPixels, m.heightPixels);  // landscape, as the game
        s = Math.max(0.5f, Math.min(w / 960f, h / 720f));
        display = font("BarlowCondensed-ExtraBoldItalic.ttf", 0);
        label = font("BarlowCondensed-BoldItalic.ttf", 0);
        body = font("InterVariable.ttf", 500);
        bodyBold = font("InterVariable.ttf", 700);
    }

    int px(float design) { return Math.round(design * s); }

    private Typeface font(String file, int weight) {
        try {
            Typeface.Builder b = new Typeface.Builder(context.getAssets(), ASSETS + file);
            if (weight > 0) b.setFontVariationSettings("'wght' " + weight);
            Typeface t = b.build();
            if (t != null) return t;
        } catch (Exception e) {
            Log.w("BlitzRecomp", "[look] font " + file + ": " + e);
        }
        return weight >= 600 ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT;
    }

    Bitmap bitmap(String name) {
        if (art.containsKey(name)) return art.get(name);
        Bitmap b = null;
        try (InputStream in = context.getAssets().open(ASSETS + "ui/" + name + ".png")) {
            b = BitmapFactory.decodeStream(in);
        } catch (Exception e) {
            Log.w("BlitzRecomp", "[look] art " + name + ": " + e);
        }
        art.put(name, b);
        return b;
    }

    TextView text(String s, Typeface face, float size, int color) {
        TextView t = new TextView(context);
        t.setText(s);
        t.setTypeface(face);
        t.setTextSize(TypedValue.COMPLEX_UNIT_PX, size * this.s);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        return t;
    }

    /** firstrun_screen.cpp's eyebrow: small, tracked caps in teal. */
    TextView eyebrow(String s) {
        TextView t = text(s, bodyBold, 11, TEAL);
        t.setLetterSpacing(0.24f);
        return t;
    }

    /** Header2: Barlow Condensed ExtraBold Italic, 40, in the selected yellow. */
    TextView title(String s) {
        TextView t = text(s, display, 40, YELLOW);
        t.setLetterSpacing(0.02f);
        t.setPadding(0, 0, px(6), 0);  // the italic's overhang
        return t;
    }

    /** Body: Inter 500 at 15 (13 for a note), 1.35 lines. */
    TextView para(String s, float size, int color) {
        TextView t = text(s, body, size, color);
        t.setLineSpacing(0, 1.12f);
        return t;
    }

    /** The flame rule under a title (rule_flame, stretched). */
    View rule() {
        View v = new View(context);
        Bitmap b = bitmap("rule_flame");
        if (b != null) v.setBackground(new android.graphics.drawable.BitmapDrawable(context.getResources(), b));
        else v.setBackgroundColor(RED);
        return v;
    }

    /**
     * A Card button (recompui's Button, the Blitz '27 style): idle on deep navy with a soft cyan rim, or dark red with a red
     * rim for the primary one; focused, the red bar with a yellow rim and yellow text. Labels in caps, one line.
     */
    TextView button(String s, boolean primary, float height) {
        TextView b = text(s.toUpperCase(java.util.Locale.US), label, 20, TEXT);
        b.setLetterSpacing(0.04f);
        b.setGravity(Gravity.CENTER);
        b.setSingleLine(true);
        b.setMinHeight(px(height));
        b.setPadding(px(16), px(6), px(16), px(6));
        b.setFocusable(true);
        b.setClickable(true);
        float r = 3 * this.s, rim = Math.max(2, px(2));
        StateListDrawable bg = new StateListDrawable();
        GradientDrawable on = new GradientDrawable();
        on.setColor(RED);
        on.setStroke((int) rim, YELLOW);
        on.setCornerRadius(r);
        GradientDrawable idle = new GradientDrawable();
        idle.setColor(primary ? RED_DARK : DEEP_NAVY);
        idle.setStroke((int) rim, primary ? RED : RIM_SOFT);
        idle.setCornerRadius(r);
        bg.addState(new int[] {android.R.attr.state_focused}, on);
        bg.addState(new int[] {android.R.attr.state_pressed}, on);
        bg.addState(new int[] {}, idle);
        b.setBackground(bg);
        b.setTextColor(new android.content.res.ColorStateList(
                new int[][] {{android.R.attr.state_focused}, {android.R.attr.state_pressed}, {}}, new int[] {YELLOW, YELLOW, TEXT}));
        return b;
    }

    /** A pad hint as firstrun_screen.cpp's bar shows it: a round button face, then the word in tracked caps. */
    View hint(String face, String word) {
        android.widget.LinearLayout row = new android.widget.LinearLayout(context);
        row.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView f = text(face, bodyBold, 13, VEIL);
        f.setGravity(Gravity.CENTER);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(TEXT);
        f.setBackground(dot);
        int d = px(24);
        row.addView(f, new android.widget.LinearLayout.LayoutParams(d, d));
        TextView w = text(word, bodyBold, 17, TEXT);
        w.setLetterSpacing(0.14f);
        w.setPadding(px(10), 0, 0, 0);
        row.addView(w);
        return row;
    }

    /**
     * ui_kit.cpp's panel: panel_frame's nine pieces (corners 16 design px), the teal grid inside and the chrome corner brackets
     * 5 px outside the frame. The brackets need that room: the frame is drawn inset by it, and so is the content.
     */
    final class Panel extends FrameLayout {
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Paint grid = new Paint();
        private final int out = px(5), inset = px(16), corner = px(20);

        Panel() {
            super(context);
            setWillNotDraw(false);
            Bitmap g = bitmap("grid_tile");
            if (g != null) {
                Bitmap tile = Bitmap.createScaledBitmap(g, Math.max(8, px(g.getWidth() * 2 / 3f)), Math.max(8, px(g.getHeight() * 2 / 3f)), true);
                grid.setShader(new BitmapShader(tile, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT));
                grid.setAlpha(45);  // as faint as the game's own panels show it
            }
            int pad = out + px(20);
            setPadding(pad, pad, pad, pad);
        }

        private void piece(Canvas c, String name, float l, float t, float r, float b) {
            Bitmap bm = bitmap("panel_frame." + name);
            if (bm == null || r <= l || b <= t) return;
            c.drawBitmap(bm, null, new RectF(l, t, r, b), paint);
        }

        @Override
        protected void onDraw(Canvas c) {
            float l = out, t = out, r = getWidth() - out, b = getHeight() - out, i = inset;
            Bitmap center = bitmap("panel_frame.c");
            if (center == null) {  // no art: the panel glass and its rim
                Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
                p.setColor(Color.argb(246, 6, 18, 44));
                c.drawRect(l, t, r, b, p);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(px(2));
                p.setColor(TEAL);
                c.drawRect(l, t, r, b, p);
            } else {
                piece(c, "tl", l, t, l + i, t + i);
                piece(c, "t", l + i, t, r - i, t + i);
                piece(c, "tr", r - i, t, r, t + i);
                piece(c, "l", l, t + i, l + i, b - i);
                piece(c, "c", l + i, t + i, r - i, b - i);
                piece(c, "r", r - i, t + i, r, b - i);
                piece(c, "bl", l, b - i, l + i, b);
                piece(c, "b", l + i, b - i, r - i, b);
                piece(c, "br", r - i, b - i, r, b);
            }
            if (grid.getShader() != null) {
                int g = px(6);
                c.drawRect(l + g, t + g, r - g, b - g, grid);
            }
            float k = corner;
            bracket(c, "chrome_corner_tl", 0, 0, k);
            bracket(c, "chrome_corner_tr", getWidth() - k, 0, k);
            bracket(c, "chrome_corner_bl", 0, getHeight() - k, k);
            bracket(c, "chrome_corner_br", getWidth() - k, getHeight() - k, k);
        }

        private void bracket(Canvas c, String name, float x, float y, float k) {
            Bitmap bm = bitmap(name);
            if (bm != null) c.drawBitmap(bm, null, new RectF(x, y, x + k, y + k), paint);
        }
    }

    Panel panel() { return new Panel(); }
}
