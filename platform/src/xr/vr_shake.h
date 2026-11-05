// Shake to spin: a flick of a controller is the Wii Remote's (or the
// Nunchuk's) shake, which the game reads as a spin.  One detector per hand,
// fed once per game frame with the controller's aim pose (xr_app.cpp); no
// OpenXR in here, so a test can drive it with made-up samples.
//
// The aim pose, not the grip: it is at the front of the controller, where a
// Wii Remote reaches beyond the hand, so a flick of the wrist moves it about
// as fast as a Wii Remote's flick moves its accelerometer (on the Steam
// Frame's controller it is 8 cm ahead of the grip pose, and the controller's
// own IMU sits next to it).  Its velocity includes the lever arm of a turn
// of the wrist; that is the point: the wrist flick is the gesture.
//
// The speed is the runtime's linear velocity when it reports one (the
// LINEAR_VALID bit), else the distance between this locate and the last
// over the time between their display times.  If the runtime calls its
// velocity valid while it stays far below what the positions say (a runtime
// reporting zeros: three fast locates with no agreeing one between), the detector moves to the
// positions for good.
#pragma once

#include <math.h>
#include <stdint.h>

namespace vr {

struct ShakeSample {
    int64_t timeNs = 0;         // the display time the pose was located for
    bool valid = false;         // the position is valid
    bool tracked = false;       // ... and tracked (not only inferred: a
                                // re-found controller jumps back)
    float pos[3] = {0, 0, 0};   // metres
    bool velocityValid = false;  // the runtime's linear velocity
    float vel[3] = {0, 0, 0};   // m/s
};

class ShakeDetector {
public:
    static constexpr int64_t kGapNs = 300000000;   // 0.3 s between spins (and after a menu)
    static constexpr float kJumpSpeed = 15.0f;     // m/s: faster between two locates is a tracking jump
    static constexpr float kMismatchSpeed = 2.0f;  // m/s by the positions ...
    static constexpr float kMismatchShare = 0.25f; // ... with the runtime's below this share of it
    static constexpr int kMismatchesToSwitch = 3;

    // One locate.  threshold: the speed that spins (m/s); suppressed: no
    // spin now (a menu, the settings panel, shaking turned off).  True when
    // this sample spins.
    bool update(const ShakeSample& s, float threshold, bool suppressed) {
        float fromPos = -1.0f;  // m/s, or -1 for none
        if (s.tracked && mHavePrev && s.timeNs > mPrevNs) {
            int64_t dt = s.timeNs - mPrevNs;
            if (dt >= 2000000 && dt <= 60000000) {
                float dx = s.pos[0] - mPrev[0], dy = s.pos[1] - mPrev[1], dz = s.pos[2] - mPrev[2];
                fromPos = sqrtf(dx * dx + dy * dy + dz * dz) / (dt / 1e9f);
                if (fromPos > kJumpSpeed) fromPos = -1.0f;
            }
        }
        mHavePrev = s.tracked;
        if (s.tracked) {
            mPrev[0] = s.pos[0];
            mPrev[1] = s.pos[1];
            mPrev[2] = s.pos[2];
            mPrevNs = s.timeNs;
        }
        if (s.valid && !mSeen) {
            mSeen = true;
            mFirstVelocityValid = s.velocityValid;
        }
        float fromRuntime = -1.0f;
        if (s.velocityValid && s.valid) {
            fromRuntime = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
            // Fast by the positions: counted when the runtime disagrees, and
            // the count starts again when it agrees (a single tracking jump
            // never adds up).
            if (!mUsePositions && fromPos > kMismatchSpeed) {
                if (fromRuntime >= kMismatchShare * fromPos) {
                    mMismatches = 0;
                } else if (++mMismatches >= kMismatchesToSwitch) {
                    mUsePositions = true;
                    mSwitched = true;
                }
            }
        }
        float speed = (fromRuntime >= 0.0f && !mUsePositions) ? fromRuntime : fromPos;
        if (suppressed) {
            if (speed > threshold && s.timeNs - mLastIgnoredNs > kGapNs) {
                ignored++;
                mLastIgnoredNs = s.timeNs;
            }
            mLastSpinNs = s.timeNs;  // none for 0.3 s after the menu closes
            return false;
        }
        if (fromRuntime > peakRuntime) peakRuntime = fromRuntime;
        if (fromPos > peakPositions) peakPositions = fromPos;
        if (speed > threshold && s.timeNs - mLastSpinNs > kGapNs) {
            spins++;
            mLastSpinNs = s.timeNs;
            return true;
        }
        return false;
    }

    // The first sample with a valid position has come (once; firstVelocityValid then
    // tells whether the runtime gave a velocity with it).
    bool takeFirstSeen() {
        bool r = mSeen && !mFirstReported;
        if (r) mFirstReported = true;
        return r;
    }
    bool firstVelocityValid() const { return mFirstVelocityValid; }
    // The runtime's velocity was given up for the positions (once).
    bool takeSwitched() {
        bool r = mSwitched;
        mSwitched = false;
        return r;
    }
    bool usingPositions() const { return mUsePositions; }

    int spins = 0;               // spins made
    int ignored = 0;             // shakes while suppressed (0.3 s apart)
    float peakRuntime = 0.0f;    // the fastest outside menus, m/s: by the runtime's velocity
    float peakPositions = 0.0f;  // ... and by the positions

private:
    bool mHavePrev = false;
    float mPrev[3] = {0, 0, 0};
    int64_t mPrevNs = 0;
    int64_t mLastSpinNs = INT64_MIN / 2, mLastIgnoredNs = INT64_MIN / 2;
    bool mSeen = false, mFirstReported = false, mFirstVelocityValid = false;
    bool mUsePositions = false, mSwitched = false;
    int mMismatches = 0;
};

}  // namespace vr
