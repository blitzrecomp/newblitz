package com.blitzrecomp.newblitz;

import android.app.Activity;

/**
 * GammaOS (an RG DS): see-through, and gone as soon as it's up. The game's activity resuming after it reports idle, which lets
 * Android finish a boot GammaOS's quick boot left open, and that turns input dispatch on (BlitzActivity.rootInputFix).
 */
public class BlitzNudge extends Activity {
    @Override
    protected void onResume() {
        super.onResume();
        finish();
        overridePendingTransition(0, 0);
    }
}
