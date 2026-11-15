package com.blitzrecomp.newblitz;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.Gravity;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * About 1 GB of RAM, no ROM yet: the system picker, then a copy into the app folder, all before the game loads. The game
 * imports what it finds there at startup. Behind the picker a running game gets killed for its memory.
 */
public class BlitzRomFirst extends Activity {
    private static final int PICK = 1;
    private TextView text;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setGravity(Gravity.CENTER);
        text.setTextSize(22);
        text.setText("Pick your Blitz ROM");
        setContentView(text);
        if (state != null) return;  // the picker's still up
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK) return;
        Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) { startGame(); return; }
        text.setText("Copying the ROM...");
        new Thread(() -> {
            copy(uri);
            runOnUiThread(this::startGame);
        }).start();
    }

    private void copy(Uri uri) {
        String name = "picked.z64";
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) name = new File(c.getString(0)).getName();
        } catch (Exception e) {
            Log.w("BlitzRecomp", "[rom] no name for the picked file: " + e);
        }
        File to = new File(getExternalFilesDir(null), name);
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        } catch (Exception e) {
            Log.e("BlitzRecomp", "[rom] couldn't copy the picked file", e);
            to.delete();
        }
    }

    private void startGame() {
        Intent game = new Intent(this, BlitzActivity.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(game, BlitzLauncher.launchOptions(this));
        finish();
    }
}
