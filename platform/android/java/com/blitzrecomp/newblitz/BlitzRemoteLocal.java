package com.blitzrecomp.newblitz;

/**
 * The remote test service inside the game's own process. On an RG DS, the separate process polled once at launch and then went
 * quiet while the game played on, so Potato Mode uses this one. It can't outlive a crash of the game.
 */
public class BlitzRemoteLocal extends BlitzRemote {
    /** A crash or an exit mustn't bring the game's process back with no game in it. */
    @Override protected int afterItsProcessDies() { return START_NOT_STICKY; }
}
