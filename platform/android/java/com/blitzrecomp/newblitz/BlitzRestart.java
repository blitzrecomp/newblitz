package com.blitzrecomp.newblitz;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.util.List;

/**
 * The whole app started again (1.0.6): Reset Data, which clears the folder the settings came from, a restart in place that
 * didn't work, and Restart after a setting read at launch changed (main.cpp restart_whole_app). See-through, in a process of
 * its own (the manifest's ":restart"), so it outlives the game's: started by BlitzActivity.relaunch as the game's
 * process ends itself (main.cpp relaunch_now: main() can't run twice in one process), it waits for that process to go,
 * then starts the launcher icon's own activity, which picks the screens as a start from the icon does (the Surface Duo's
 * span included: this task is gone by the time the launcher places the game), and goes.
 */
public class BlitzRestart extends Activity {
    static final String PID = "pid";
    // The game's process ends by itself well within this (Reset Data clears a folder first); else it's ended here.
    private static final long WAIT_MS = 10000;

    /** Whether one of this app's processes has that pid (another app's /proc entries aren't visible, nor are ours). */
    private static boolean running(Context context, int pid) {
        ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> list = am != null ? am.getRunningAppProcesses() : null;
        if (list == null) return false;
        for (ActivityManager.RunningAppProcessInfo p : list) if (p.pid == pid) return true;
        return false;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final int pid = getIntent().getIntExtra(PID, 0);
        final Context app = getApplicationContext();
        final Handler handler = new Handler(Looper.getMainLooper());
        final long began = SystemClock.uptimeMillis();
        handler.post(new Runnable() {
            @Override
            public void run() {
                boolean alive = pid > 0 && pid != Process.myPid() && running(app, pid);
                if (alive && SystemClock.uptimeMillis() - began < WAIT_MS) {
                    handler.postDelayed(this, 50);
                    return;
                }
                if (alive) {
                    Log.w("BlitzRecomp", "[restart] the game's process " + pid + " is still there after " + WAIT_MS + " ms: ending it");
                    Process.killProcess(pid);
                    handler.postDelayed(this::startLauncher, 300);
                    return;
                }
                startLauncher();
            }

            private void startLauncher() {
                Intent start = new Intent(app, BlitzLauncher.class);
                start.setAction(Intent.ACTION_MAIN);
                start.addCategory(Intent.CATEGORY_LAUNCHER);
                start.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try {
                    startActivity(start);
                    Log.i("BlitzRecomp", "[restart] the launcher starts again, " + (SystemClock.uptimeMillis() - began) + " ms after the restart began");
                } catch (Exception e) {
                    Log.w("BlitzRecomp", "[restart] couldn't start the launcher again: " + e);
                }
                finish();
                overridePendingTransition(0, 0);
                handler.postDelayed(() -> Process.killProcess(Process.myPid()), 1000);  // this process's one job is done
            }
        });
    }
}
