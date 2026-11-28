package com.blitzrecomp.newblitz;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.Presentation;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * About 1 GB of RAM, no ROM yet: setup's first screen, before the game loads. It asks for the ROM to be sent from Blitz Builder
 * on a computer (the usual way since 2026-11-15) and starts the game when it arrives; or the system picker, then a copy into
 * the app folder. The game imports what it finds there at startup, and setup (firstrun.cpp) goes on from its next screen.
 * Behind the picker a running game gets killed for its memory, so this screen is drawn here, in the app's own look (BlitzLook:
 * firstrun_screen.cpp's). With two screens it's on the lower one, the touch screen (BlitzLauncher), and the other says where
 * to look. A ROM already in the app folder (sent by the Builder) skips it: BlitzLauncher starts the game.
 */
public class BlitzRomFirst extends Activity {
    /** The display BlitzLauncher was opened on: the game goes there, as it would have without this screen. */
    static final String GAME_DISPLAY = "game_display";
    private static final int PICK = 1;
    private static final String TAG = "BlitzRecomp";

    private BlitzLook look;
    private LinearLayout content;
    private TextView primary;
    private boolean busy;
    private Presentation other;

    // A log of this screen, in the app folder beside blitz.log (the game's own log isn't up yet).
    private static void report(Activity a, String line) {
        Log.i(TAG, line);
        File app = a.getExternalFilesDir(null);
        if (app == null) return;
        File log = new File(app, "blitz.romfirst.log");
        try (OutputStream o = new FileOutputStream(log, log.length() < 64 * 1024)) {  // started again once it's big
            o.write((String.format(Locale.US, "%tT ", System.currentTimeMillis()) + line + "\n").getBytes("UTF-8"));
        } catch (Exception e) {
            Log.w(TAG, "[romfirst] log: " + e);
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        look = new BlitzLook(this);
        Display here = getWindowManager().getDefaultDisplay();
        report(this, "[romfirst] setup's ROM screen on display " + here.getDisplayId() + ", " + BlitzLauncher.allDisplays(this).size()
                + " display(s), scale " + String.format(Locale.US, "%.2f", look.s));
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(BlitzLook.VEIL);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        FrameLayout centre = new FrameLayout(this);
        BlitzLook.Panel card = look.panel();
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        card.addView(content, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        int width = Math.min(look.px(720 + 10), Math.round(getResources().getDisplayMetrics().widthPixels * 0.96f));
        FrameLayout.LayoutParams cardAt = new FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        cardAt.setMargins(0, look.px(16), 0, look.px(56));  // clear of the hints
        centre.addView(card, cardAt);
        scroll.addView(centre, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        View hints = look.hint("A", "CHOOSE");
        FrameLayout.LayoutParams hintsAt = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        hintsAt.bottomMargin = look.px(18);
        root.addView(hints, hintsAt);
        setContentView(root);
        hideSystemBars();  // once there's a window's view to ask
        showSend();
        showOther(here);
        root.post(() -> shoot(root, "romfirst"));
    }

    // --- The screens -------------------------------------------------------------------------------------------------

    // The top of every one: the eyebrow, the title on its flame rule (firstrun_screen.cpp).
    private void head(String title) {
        content.removeAllViews();
        // Setup's steps on a phone (firstrun.cpp plan()): the ROM, the 2026 season unless the app carries it, Recommended or
        // Customize, updates and reports. This is the first.
        add(look.eyebrow("SETUP · STEP 1 OF " + (seasonBundled() ? 3 : 4)), 0);
        add(look.title(title), 8);
        View rule = look.rule();
        add(rule, 8);
        rule.getLayoutParams().height = Math.max(2, look.px(4));
    }

    // A second screen (an RG DS): where Potato Mode's two-screen setting is (on by default since 2026-11-15).
    private void twoScreensNote() {
        if (BlitzLauncher.allDisplays(this).size() > 1) {
            add(look.para("Two screens? New Blitz plays across both. For one, which runs a little faster, turn off Settings "
                    + "→ General → Two Screens (Potato).", 13, BlitzLook.STEEL), 6);
        }
    }

    // Exit, on the right, as every setup screen has it: before the game there's nothing to go back to, so the app closes. With
    // another button on the left (left: null for none).
    private void bar(TextView left) {
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        if (left != null) bar.addView(left);
        bar.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1f));
        TextView exit = look.button("Exit", false, 38);
        exit.setMinWidth(look.px(110));
        exit.setOnClickListener(v -> {
            report(this, "[romfirst] Exit: no ROM, the app closes");
            finish();
        });
        bar.addView(exit);
        add(bar, 12);
    }

    /**
     * No ROM here yet. The computer's New Blitz sets a device up (Blitz Builder sends the app, the ROM and the setup over USB:
     * The team, 2026-11-15, the computer is the hub), so that's what this asks for; picking the file here is the small way round.
     * New Blitz starts by itself once the Builder's files arrive (watch).
     */
    private void showSend() {
        // The same words as firstrun.cpp's screen for a phone with no ROM (the hub model).
        head("Set up New Blitz from your computer");
        add(look.para("Open New Blitz on your Mac, Windows or Linux computer → Blitz Builder → Phone or Handheld. It sends the "
                + "game, your ROM and your setup here over USB, and New Blitz starts here when they arrive.", 15, BlitzLook.TEXT), 8);
        add(look.para("No computer? Pick a File Instead: your NFL Blitz 2000 ROM, the USA cartridge, v1.1 or v1.0 (.z64, .n64, .v64 or a "
                + ".zip). No touch screen? Press L1 in the file picker for its sidebar.", 13, BlitzLook.GOLD), 12);
        twoScreensNote();
        primary = look.button("Pick a File Instead", false, 38);
        primary.setOnClickListener(v -> openPicker());
        bar(primary);
        primary.requestFocus();  // a controller's A presses it
    }

    /** firstrun.cpp's RomBad: what was wrong with the picked file, and Try Another. */
    private void showRom(String problem) {
        head("That ROM didn't work");
        add(look.para(problem, 15, BlitzLook.TEXT), 8);
        add(look.para("The right file is NFL Blitz 2000 for the N64, USA, version 1.1 or 1.0. Or send it from Blitz Builder on your "
                + "computer. No touch screen? Press L1 in the file picker for its sidebar.", 13, BlitzLook.GOLD), 12);
        twoScreensNote();
        primary = look.button("Try Another", true, 46);
        primary.setOnClickListener(v -> openPicker());
        add(primary, 12);
        bar(null);
        primary.requestFocus();
    }

    // Blitz Builder's files over adb (device_send.cpp: the ROM as blitz.z64, the setup, builder_done last): the game starts once
    // they're in, as if it had been opened then. Without builder_done, a ROM still being copied waits for the folder to hold
    // its size for a look.
    private final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());
    private long lastSize = -1;
    private final Runnable watch = new Runnable() {
        @Override public void run() {
            if (isFinishing()) return;
            ui.postDelayed(this, 2000);
            if (busy || !BlitzLauncher.hasRom(BlitzRomFirst.this)) return;
            File app = getExternalFilesDir(null);
            long size = 0;
            File[] files = app != null ? app.listFiles() : null;
            if (files != null) for (File f : files) size += f.length();
            boolean done = app != null && new File(app, "builder_done").isFile();
            if (done || size == lastSize) {
                ui.removeCallbacks(this);
                report(BlitzRomFirst.this, "[romfirst] a ROM arrived" + (done ? " from Blitz Builder" : "") + ": the game starts");
                startGame();
            }
            lastSize = size;
        }
    };

    @Override
    protected void onResume() {
        super.onResume();
        ui.removeCallbacks(watch);
        lastSize = -1;
        ui.postDelayed(watch, 2000);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(watch);
        super.onPause();
    }

    // content_dirs.cpp has_season, for the app's own resources (a private build carries the season).
    private boolean seasonBundled() {
        try (InputStream in = getAssets().open("res/packs/blitz2026/pack.json")) {
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private void add(View v, int topGap) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = look.px(topGap);
        content.addView(v, p);
    }

    private void setStatus(String label) {
        if (primary != null) primary.setText(label.toUpperCase(Locale.US));
    }

    // A, B, Start or the D-pad's centre press the focused button (the RG DS's own A can arrive as either face button: its
    // pad is laid out Nintendo's way); the D-pad moves between them, as Android does by itself.
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER:
                if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0 && !busy) {
                    View f = getCurrentFocus();
                    if (f == null || !f.isClickable()) f = primary;
                    if (f != null) f.performClick();
                }
                return true;
            default:
                return super.dispatchKeyEvent(e);
        }
    }

    private void openPicker() {
        if (busy) return;
        report(this, "[romfirst] Load ROM: the system picker");
        setStatus("Opening the File Picker…");
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, PICK);
        } catch (Exception e) {
            report(this, "[romfirst] no file picker: " + e);
            showRom("This device has no file picker for New Blitz to use. Copy the ROM into Android/data/"
                    + getPackageName() + "/files with a file manager or a computer, then open New Blitz again.");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK) return;
        Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {  // the picker closed without a file: stay here, with the button
            report(this, "[romfirst] the picker closed without a file");
            showSend();
            return;
        }
        busy = true;
        setStatus("Copying the ROM…");
        new Thread(() -> {
            String problem = copy(uri);
            runOnUiThread(() -> {
                busy = false;
                if (problem == null) startGame();
                else showRom(problem);
            });
        }).start();
    }

    // The N64's three byte orders, by the first word of the header.
    private static String romExtension(byte[] head) {
        int w = ((head[0] & 0xff) << 24) | ((head[1] & 0xff) << 16) | ((head[2] & 0xff) << 8) | (head[3] & 0xff);
        if (w == 0x80371240) return ".z64";
        if (w == 0x37804012) return ".v64";
        if (w == 0x40123780) return ".n64";
        return null;
    }

    private static boolean isZip(byte[] head) {
        return head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
    }

    private static int readHead(InputStream in, byte[] head) throws java.io.IOException {
        int n = 0;
        for (int r; n < head.length && (r = in.read(head, n, head.length - n)) > 0; ) n += r;
        return n;
    }

    /** Null once the picked file is in the app folder, else what's wrong with it (main.cpp rom_error_text's words). */
    private String copy(Uri uri) {
        String name = "picked";
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) name = new File(c.getString(0)).getName();
        } catch (Exception e) {
            report(this, "[romfirst] no name for the picked file: " + e);
        }
        File app = getExternalFilesDir(null);
        File to = new File(app, "picking.tmp");
        long bytes = 0;
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                bytes += n;
            }
        } catch (Exception e) {
            report(this, "[romfirst] couldn't copy the picked file: " + e);
            to.delete();
            return "Couldn't read that file. Check it isn't empty and that the app may open it.";
        }
        // What it is, by its first bytes: a ROM in any byte order, or a zip holding one. The game checks the rest at its start.
        String ext = null;
        try (InputStream in = new FileInputStream(to)) {
            byte[] head = new byte[4];
            if (readHead(in, head) == 4) {
                ext = romExtension(head);
                if (ext == null && isZip(head)) ext = zipHoldsRom(to) ? ".zip" : "";
            }
        } catch (Exception e) {
            report(this, "[romfirst] couldn't read the copy: " + e);
        }
        report(this, "[romfirst] picked \"" + name + "\", " + bytes + " bytes: " + (ext == null ? "not an N64 ROM" : ext.isEmpty() ? "a zip with no ROM" : ext));
        if (ext == null || ext.isEmpty()) {
            to.delete();
            return ext == null ? "That isn't an N64 ROM. Pick the game's .z64, .n64 or .v64 file, or a .zip holding one."
                               : "That zip has no N64 ROM in it. Pick the game's .z64, .n64 or .v64 file, or a .zip holding one.";
        }
        // Its own name where it has the right ending, so the game's log names it; else the ending its bytes call for.
        String base = name.replaceAll("[/\\\\]", "_");
        File rom = new File(app, base.toLowerCase(Locale.US).endsWith(ext) ? base : base + ext);
        rom.delete();
        if (!to.renameTo(rom)) {
            to.delete();
            return "Couldn't save the ROM in New Blitz's folder. Check the device has 16 MB free.";
        }
        return null;
    }

    private static boolean zipHoldsRom(File zip) {
        try (ZipInputStream z = new ZipInputStream(new FileInputStream(zip))) {
            byte[] head = new byte[4];
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory()) continue;
                if (readHead(z, head) == 4 && romExtension(head) != null) return true;
            }
        } catch (Exception e) {
            Log.w(TAG, "[romfirst] zip: " + e);
        }
        return false;
    }

    private void startGame() {
        Intent game = new Intent(this, BlitzActivity.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        // Where BlitzLauncher would have put it: its two-screen choice, else the display it was opened on (this screen may be
        // on the other one, the touch screen).
        Bundle options = BlitzLauncher.launchOptions(this);
        int display = getIntent().getIntExtra(GAME_DISPLAY, -1);
        if (options == null && display >= 0 && display != getWindowManager().getDefaultDisplay().getDisplayId()) {
            options = ActivityOptions.makeBasic().setLaunchDisplayId(display).toBundle();
        }
        report(this, "[romfirst] ROM saved: the game starts" + (options != null ? " (launch options)" : ""));
        try {
            startActivity(game, options);
        } catch (Exception e) {
            report(this, "[romfirst] couldn't start the game on its display: " + e);
            startActivity(game);
        }
        finish();
    }

    // --- The other screen ---------------------------------------------------------------------------------------------

    /** With two screens: the logo and where to look, on the screen this isn't on, as a Presentation (else it stays as it is). */
    private void showOther(Display here) {
        Display second = null;
        for (Display d : BlitzLauncher.allDisplays(this)) {
            if (d.getDisplayId() != here.getDisplayId() && d.getState() != Display.STATE_OFF) second = d;
        }
        if (second == null) return;
        final Display d = second;
        boolean upper = BlitzLauncher.firstIsUpper(this, d, here);
        try {
            other = new Presentation(this, d) {
                @Override protected void onCreate(Bundle s) {
                    super.onCreate(s);
                    setContentView(otherScreen(new BlitzLook(getContext()), upper));
                    getWindow().getDecorView().post(() -> shoot(getWindow().getDecorView(), "romfirst_other"));
                }
            };
            other.show();
            report(this, "[romfirst] display " + d.getDisplayId() + " (the " + (upper ? "upper" : "lower") + " screen) shows where to look");
        } catch (Exception e) {
            report(this, "[romfirst] nothing on display " + d.getDisplayId() + ": " + e);
            other = null;
        }
    }

    private View otherScreen(BlitzLook l, boolean upper) {
        LinearLayout box = new LinearLayout(l.context);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(BlitzLook.VEIL);
        Bitmap logo = l.bitmap("logo27_lockup");
        if (logo != null) {
            ImageView v = new ImageView(l.context);
            v.setImageBitmap(logo);
            v.setAdjustViewBounds(true);
            int w = l.px(480);
            box.addView(v, new LinearLayout.LayoutParams(w, w * logo.getHeight() / logo.getWidth()));
        }
        TextView t = l.title(upper ? "Setup is on the lower screen" : "Setup is on the upper screen");
        t.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, l.px(32));
        t.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams at = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        at.topMargin = l.px(28);
        box.addView(t, at);
        TextView p = l.para("New Blitz asks for your ROM there.", 15, BlitzLook.STEEL);
        p.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams pat = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        pat.topMargin = l.px(8);
        box.addView(p, pat);
        return box;
    }

    @Override
    protected void onDestroy() {
        if (other != null) {
            try { other.dismiss(); } catch (Exception e) { /* its display is gone */ }
            other = null;
        }
        super.onDestroy();
    }

    // --- Helpers --------------------------------------------------------------------------------------------------------

    private void hideSystemBars() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            android.view.WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(android.view.WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
    }

    /** What a screen came out as, for checking it without seeing the device: a PNG in the app's private cache. */
    private void shoot(View v, String name) {
        if (v.getWidth() <= 0 || v.getHeight() <= 0) return;
        try {
            Bitmap b = Bitmap.createBitmap(v.getWidth(), v.getHeight(), Bitmap.Config.ARGB_8888);
            v.draw(new Canvas(b));
            new Thread(() -> {
                try (OutputStream o = new FileOutputStream(new File(getCacheDir(), name + ".png"))) {
                    b.compress(Bitmap.CompressFormat.PNG, 100, o);
                } catch (Exception e) {
                    Log.w(TAG, "[romfirst] shot: " + e);
                }
                b.recycle();
            }).start();
        } catch (Throwable t) {
            Log.w(TAG, "[romfirst] shot: " + t);
        }
    }
}
