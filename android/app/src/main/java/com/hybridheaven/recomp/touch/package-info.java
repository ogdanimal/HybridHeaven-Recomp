/**
 * On-screen touch controls: an N64 pad drawn over SDL's surface that presents
 * itself to native code as an extra gamepad.
 *
 * <p>Written by <a href="https://github.com/epic-ship-it">epic-ship-it</a> for
 * Goemon64Recomp-Android
 * (<a href="https://github.com/ogdanimal/Goemon64Recomp-Android/pull/25">#25</a>)
 * and ported to this app. The native half is {@code src/main/android_touch.cpp};
 * the design and the one Hybrid Heaven-specific change (the default shoulder
 * layout) are in {@code docs/touch-controls.md}.
 */
package com.hybridheaven.recomp.touch;
