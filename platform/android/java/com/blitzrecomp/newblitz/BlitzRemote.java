package com.blitzrecomp.newblitz;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.Process;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.file.Files;

/**
 * Test builds: lets Claude work the handheld from anywhere (docs/ANDROID.md, tools/android/remote.sh). In its own process, so
 * it outlives a crash of the game and can start it again. It polls the log Worker's mailbox for commands, a fixed set, with
 * files only in the app's folder; a build without an upload address has none of this.
 */
public class BlitzRemote extends Service {
    static final String SHOT = ".REMOTE_SHOT";
    private static final long POLL_MS = 1500;
    private static boolean started = false;
    private android.os.PowerManager.WakeLock awake;
    private Thread poller;
    private volatile boolean stopping = false;  // on top of the interrupt, which the HTTP code can swallow

    static String base(Context c) {
        try (InputStream in = c.getAssets().open("res/logs_hub")) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[256];
            for (int n; (n = in.read(buf)) > 0; ) b.write(buf, 0, n);
            String address = b.toString("UTF-8").trim();
            return address.endsWith("/up") ? address.substring(0, address.length() - 3) : "";
        } catch (Exception e) {
            return "";
        }
    }

    static int post(String address, byte[] body) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(address).openConnection();
        try {
            c.setDoOutput(true);
            c.setRequestMethod("POST");
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setFixedLengthStreamingMode(body.length);
            try (OutputStream o = c.getOutputStream()) { o.write(body); }
            return c.getResponseCode();
        } finally {
            c.disconnect();
        }
    }

    private static byte[] fetch(String address) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(address).openConnection();
        try {
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            if (c.getResponseCode() != 200) return new byte[0];
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) b.write(buf, 0, n);
            }
            return b.toByteArray();
        } finally {
            c.disconnect();
        }
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel("remote", "Remote testing", NotificationManager.IMPORTANCE_LOW));
        // BRANDING: notification title
        Notification n = new Notification.Builder(this, "remote").setContentTitle("New Blitz: remote testing on")
                .setSmallIcon(android.R.drawable.stat_notify_sync).build();
        if (Build.VERSION.SDK_INT >= 34) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else startForeground(1, n);
        synchronized (BlitzRemote.class) {
            if (started) return afterItsProcessDies();
            started = true;
        }
        final String base = base(this);
        poller = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            while (!stopping) {
                try {
                    String lines = new String(fetch(base + "/cmd?pid=" + mainPid()), "UTF-8");
                    for (String line : lines.split("\n")) {
                        if (!line.trim().isEmpty()) run(base, line.trim());
                    }
                } catch (Exception e) { /* no network: the next poll */ }
                try { Thread.sleep(POLL_MS); } catch (InterruptedException e) { return; }
            }
        }, "BlitzRemote");
        poller.start();
        return afterItsProcessDies();
    }

    /** onStartCommand's answer. In its own process: Android starts it again, ready to start the game again. */
    protected int afterItsProcessDies() { return START_STICKY; }

    // Stopped for the other service (BlitzActivity.startRemote): this process stays, so the polling has to end by itself. A
    // batch already on its way still runs.
    @Override public void onDestroy() {
        synchronized (BlitzRemote.class) { started = false; }
        stopping = true;
        if (poller != null) poller.interrupt();
        if (awake != null && awake.isHeld()) awake.release();
        super.onDestroy();
    }

    private File app() {
        File app = getExternalFilesDir(null);
        return app != null ? app : new File(getFilesDir(), "app");
    }

    /** A path in the app's folder, or null. */
    private File inApp(String path) throws IOException {
        File root = app().getCanonicalFile();
        File f = new File(root, path).getCanonicalFile();
        return f.equals(root) || f.getPath().startsWith(root.getPath() + "/") ? f : null;
    }

    private int mainPid() {
        ActivityManager am = getSystemService(ActivityManager.class);
        for (ActivityManager.RunningAppProcessInfo p : am.getRunningAppProcesses()) {
            if (p.processName.equals(getPackageName())) return p.pid;
        }
        return 0;
    }

    private void startGame() {
        Intent i = new Intent(this, BlitzActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
    }

    /** The game's own process is about to go, with this service in it: the system has to start it again. */
    private void startGameAfterThisProcess() {
        Intent i = new Intent(this, BlitzActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        android.app.PendingIntent later = android.app.PendingIntent.getActivity(this, 0, i, android.app.PendingIntent.FLAG_IMMUTABLE | android.app.PendingIntent.FLAG_UPDATE_CURRENT);
        getSystemService(android.app.AlarmManager.class).set(android.app.AlarmManager.ELAPSED_REALTIME_WAKEUP, android.os.SystemClock.elapsedRealtime() + 1500, later);
    }

    private void result(String base, String name, byte[] body) throws IOException {
        post(base + "/res/" + name, body);
    }

    private void run(String base, String line) {
        // Every command keeps the screen on for a while: with it off, Android stops a game the moment it starts.
        if (awake == null) {
            awake = getSystemService(android.os.PowerManager.class).newWakeLock(android.os.PowerManager.SCREEN_BRIGHT_WAKE_LOCK | android.os.PowerManager.ACQUIRE_CAUSES_WAKEUP, "blitz:remote");
            awake.setReferenceCounted(false);
        }
        awake.acquire(10 * 60 * 1000L);
        String[] w = line.split(" ", 3);
        String verb = w[0];
        String name = w.length > 1 ? w[1] : "";
        String rest = w.length > 2 ? w[2] : "";
        try {
            switch (verb) {
                case "live":  // a live input command (src/main/dev_input.cpp), name and rest being its words
                    try (OutputStream o = new FileOutputStream(new File(app(), "remote_input.txt"), true)) {
                        o.write((line.substring(5) + "\n").getBytes("UTF-8"));
                    }
                    break;
                case "shot":
                    sendBroadcast(new Intent(getPackageName() + SHOT).setPackage(getPackageName()).putExtra("name", name));
                    break;
                case "log": {
                    File f = new File(app(), rest.isEmpty() ? "blitz.log" : rest);
                    byte[] all = Files.readAllBytes(f.toPath());
                    int keep = Math.min(all.length, 1500000);
                    result(base, name, java.util.Arrays.copyOfRange(all, all.length - keep, all.length));
                    break;
                }
                case "logcat": {
                    java.lang.Process p = Runtime.getRuntime().exec(new String[] {"logcat", "-d", "-t", "4000"});
                    // It can hang on a slow device, and the mailbox waits for it.
                    Thread limit = new Thread(() -> {
                        try { Thread.sleep(20000); p.destroyForcibly(); } catch (InterruptedException e) { /* finished */ }
                    });
                    limit.setDaemon(true);
                    limit.start();
                    ByteArrayOutputStream b = new ByteArrayOutputStream();
                    try (InputStream in = p.getInputStream()) {
                        byte[] buf = new byte[1 << 16];
                        for (int n; (n = in.read(buf)) > 0; ) b.write(buf, 0, n);
                    }
                    byte[] all = b.toByteArray();
                    limit.interrupt();
                    int keep = Math.min(all.length, 1500000);
                    result(base, name, java.util.Arrays.copyOfRange(all, all.length - keep, all.length));
                    break;
                }
                case "ls": {
                    File d = inApp(rest.isEmpty() ? "." : rest);
                    StringBuilder s = new StringBuilder();
                    File[] files = d != null ? d.listFiles() : null;
                    if (files != null) for (File f : files) s.append(f.isDirectory() ? "d " : "f ").append(f.length()).append(' ').append(f.getName()).append('\n');
                    result(base, name, s.toString().getBytes("UTF-8"));
                    break;
                }
                case "get": {
                    File f = inApp(rest);
                    if (f != null && f.length() <= 1500000) result(base, name, Files.readAllBytes(f.toPath()));
                    break;
                }
                case "put": {  // from the mailbox's file <name>
                    File f = inApp(rest);
                    byte[] body = fetch(base + "/res/" + name);
                    if (f != null && body.length > 0) {
                        f.getParentFile().mkdirs();
                        try (OutputStream o = new FileOutputStream(f)) { o.write(body); }
                    }
                    break;
                }
                case "mv": {  // "mv <from> <to>"
                    File a = inApp(name), b = inApp(rest);
                    if (a != null && b != null) a.renameTo(b);
                    break;
                }
                case "start":
                    startGame();
                    break;
                case "kill":
                case "restart": {
                    int pid = mainPid();
                    // BlitzRemoteLocal: nothing after the kill runs. Said first, since the alarm may not come.
                    if (verb.equals("restart") && pid == Process.myPid()) {
                        startGameAfterThisProcess();
                        result(base, "restart", "this is the game's own process: killing it, and an alarm starts it again in about 1.5 s\n".getBytes("UTF-8"));
                    }
                    if (pid != 0) Process.killProcess(pid);
                    if (verb.equals("restart")) {
                        Thread.sleep(1500);
                        startGame();
                    }
                    break;
                }
                default:
                    break;
            }
        } catch (Exception e) {
            try { result(base, "error", (line + ": " + e).getBytes("UTF-8")); } catch (Exception ignored) { }
        }
    }
}
