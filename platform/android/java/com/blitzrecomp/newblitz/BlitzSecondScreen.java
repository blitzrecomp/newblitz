package com.blitzrecomp.newblitz;

import android.app.Activity;
import android.os.Bundle;
import android.view.MotionEvent;

/** The second screen, on a display Android won't give a Presentation (BlitzActivity.showSecondScreen). */
public class BlitzSecondScreen extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        BlitzActivity.secondActivity = this;
        BlitzActivity.secondActivityOpening = false;
        setContentView(BlitzActivity.makeSecondView(this));
        BlitzActivity.setupSecondWindow(getWindow());
    }

    @Override
    protected void onDestroy() {
        if (BlitzActivity.secondActivity == this) BlitzActivity.secondActivity = null;
        super.onDestroy();
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        BlitzActivity.rawTouch("second", e);
        return super.dispatchTouchEvent(e);
    }

    // The back gesture here shouldn't leave a dead panel behind.
    @Override
    public void onBackPressed() { }
}
