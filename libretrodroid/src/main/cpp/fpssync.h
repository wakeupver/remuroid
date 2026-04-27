/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 * Frame-pacing strategy (mirrors RetroArch's swap-interval model):
 *
 *   swap_interval = round(screenRefreshRate / contentRefreshRate)
 *
 * advanceFrames() is called once per display vsync (GLSurfaceView onDrawFrame).
 * It returns 1 every swap_interval-th call and 0 for the intermediate ones.
 * This gives retro_run exactly (screenRefreshRate / swap_interval) calls/s,
 * matching the core's target FPS for any integer-ratio pair (e.g. 30 fps on a
 * 60 Hz display → swap_interval 2 → retro_run at 30 Hz).
 *
 * For non-integer ratios (e.g. PAL 50 fps on a 60 Hz display, swap_interval 1
 * but timing_skew > MAX_TIMING_SKEW), the vsync path cannot deliver exact
 * pacing.  In that case FPSSync falls back to a software-timer path: wait()
 * uses a hybrid sleep+spin to hit the deadline and advanceFrames() returns 0
 * when the deadline has not been reached yet.
 */

#ifndef LIBRETRODROID_FPSSYNC_H
#define LIBRETRODROID_FPSSYNC_H

#include <chrono>
#include <cmath>
#include <thread>

namespace libretrodroid {

typedef std::chrono::steady_clock::time_point TimePoint;
typedef std::chrono::duration<long, std::micro> Duration;

class FPSSync {
public:
    FPSSync(double contentRefreshRate, double screenRefreshRate);
    ~FPSSync() = default;

    void reset();
    unsigned advanceFrames();
    void wait();
    double getTimeStretchFactor();

private:
    double screenRefreshRate;
    double contentRefreshRate;

    // swap_interval mirrors RetroArch's video_swap_interval_auto:
    //   swap_interval = max(1, round(screen / content))
    // retro_run fires once every swap_interval vsync ticks.
    unsigned swap_interval = 1;

    // vsync_tick counts advanceFrames() calls; wraps at swap_interval.
    unsigned vsync_tick = 0;

    // useVSync: true  → vsync-divisor path (accurate for integer ratios).
    //           false → software-timer path (non-integer ratios, e.g. 50/60).
    bool useVSync = true;

    // Maximum fractional timing skew for the vsync path.
    // Mirrors RetroArch's audio_max_timing_skew default (5 %).
    static constexpr double MAX_TIMING_SKEW = 0.05;

    // Software-timer path (useVSync == false only).
    static constexpr int MAX_CATCH_UP_FRAMES = 2;
    Duration sampleInterval {};
    const TimePoint MIN_TIME = TimePoint::min();
    TimePoint lastFrame = MIN_TIME;

    void start();
};

} // namespace libretrodroid

#endif // LIBRETRODROID_FPSSYNC_H
