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
 */

#include <cmath>
#include "fpssync.h"
#include "log.h"

namespace libretrodroid {

FPSSync::FPSSync(double contentRefreshRate, double screenRefreshRate)
    : screenRefreshRate(screenRefreshRate), contentRefreshRate(contentRefreshRate)
{
    // --- Compute swap_interval (mirrors runloop_set_video_swap_interval) ----
    // Guard: content fps must be positive and not exceed the display rate.
    if (contentRefreshRate <= 0.0 || screenRefreshRate <= 0.0
            || contentRefreshRate > screenRefreshRate) {
        swap_interval = 1;
        useVSync      = true;
        LOGI("FPSSync: degenerate fps (content=%.3f screen=%.3f) — swap_interval=1 vsync",
             contentRefreshRate, screenRefreshRate);
        return;
    }

    double ratio     = screenRefreshRate / contentRefreshRate;
    unsigned si      = static_cast<unsigned>(ratio + 0.5);   // round to nearest int
    if (si < 1) si = 1;
    if (si > 4) si = 4;   // cap at 4 (RetroArch uses same limit)

    // Timing skew: how far is screen/si from the requested content fps?
    //   skew = |1 - content / (screen / si)|
    double effective_fps = screenRefreshRate / static_cast<double>(si);
    double timing_skew   = std::fabs(1.0 - contentRefreshRate / effective_fps);

    swap_interval = si;
    useVSync      = (timing_skew <= MAX_TIMING_SKEW);

    if (!useVSync) {
        // Software-timer path: deadline = 1 / contentRefreshRate.
        sampleInterval = std::chrono::microseconds(
            static_cast<long>(1000000.0 / contentRefreshRate));
    }

    LOGI("FPSSync: content=%.3f screen=%.3f swap_interval=%u effective=%.3f skew=%.3f useVSync=%d",
         contentRefreshRate, screenRefreshRate, swap_interval,
         effective_fps, timing_skew, static_cast<int>(useVSync));
}

unsigned FPSSync::advanceFrames() {
    if (useVSync) {
        // Vsync-divisor path: fire retro_run once every swap_interval ticks.
        vsync_tick = (vsync_tick + 1) % swap_interval;
        return (vsync_tick == 0) ? 1u : 0u;
    }

    // Software-timer path (non-integer fps ratios, e.g. 50 fps on 60 Hz).
    if (lastFrame == MIN_TIME) {
        start();
        return 1;
    }

    auto now = std::chrono::steady_clock::now();

    if (now < lastFrame) return 0;

    auto elapsed = now - lastFrame;
    unsigned frames = static_cast<unsigned>(elapsed / sampleInterval);
    if (frames < 1)                    frames = 1;
    if (frames > MAX_CATCH_UP_FRAMES)  frames = MAX_CATCH_UP_FRAMES;

    lastFrame = lastFrame + sampleInterval * frames;
    return frames;
}

void FPSSync::wait() {
    if (useVSync) return;

    // Hybrid sleep+spin: sleep until ~800 µs before deadline, then busy-spin.
    // Eliminates OS scheduler jitter without burning a full busy-wait cycle.
    constexpr auto SPIN_THRESHOLD = std::chrono::microseconds(800);
    const auto deadline  = lastFrame;
    const auto sleepUntil = deadline - SPIN_THRESHOLD;
    std::this_thread::sleep_until(sleepUntil);
    while (std::chrono::steady_clock::now() < deadline) {
        __builtin_arm_yield();
    }
}

double FPSSync::getTimeStretchFactor() {
    // When vsync-paced, the audio resampler must stretch to the effective fps.
    if (useVSync) {
        double effective = screenRefreshRate / static_cast<double>(swap_interval);
        return effective / screenRefreshRate;
    }
    return 1.0;
}

void FPSSync::reset() {
    lastFrame  = MIN_TIME;
    vsync_tick = 0;
}

void FPSSync::start() {
    LOGI("FPSSync::start — content=%.3f screen=%.3f swap_interval=%u useVSync=%d",
         contentRefreshRate, screenRefreshRate, swap_interval, static_cast<int>(useVSync));
    lastFrame = std::chrono::steady_clock::now();
}

} // namespace libretrodroid
