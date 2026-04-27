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

unsigned FPSSync::advanceFrames() {
    if (useVSync) return 1;

    if (lastFrame == MIN_TIME) {
        start();
        return 1;
    }

    auto now = std::chrono::steady_clock::now();

    // If we haven't reached the next frame deadline yet, tell the caller to skip
    // retro_run() this vsync.  wait() will then sleep until the deadline so the
    // next onDrawFrame() call arrives right on time.
    // This is the key fix for the "double speed" bug on 30 fps games:
    // Without this guard, std::max(..., 1) forced one retro_run() per vsync even
    // when the frame budget hadn't expired — doubling the effective step rate for
    // a 30 fps core on a 60 Hz display.
    if (now < lastFrame) return 0;

    auto elapsed  = now - lastFrame;
    auto frames   = elapsed / sampleInterval;   // integer division, >= 1 here
    if (frames < 1) frames = 1;                 // guard for rounding edge case
    if (frames > 2) frames = 2;                 // cap: prevent spiral-of-death

    lastFrame = lastFrame + sampleInterval * frames;
    return static_cast<unsigned>(frames);
}

FPSSync::FPSSync(double contentRefreshRate, double screenRefreshRate) {
    this->contentRefreshRate = contentRefreshRate;
    this->screenRefreshRate = screenRefreshRate;

    // useVSync=true when the display rate and core rate are within FPS_TOLERANCE.
    // In this mode, advanceFrames() always returns 1 and wait() is a no-op —
    // the display vsync itself provides the correct pacing.
    //
    // When the display is a near-exact integer multiple of the core rate
    // (e.g. 120 Hz screen + 60 fps core, ratio ≈ 2.0), we still use vsync mode
    // so we don't fight against eglSwapBuffers().  The extra vsync ticks are
    // absorbed by the non-vsync path's wait() sleep — but in practice it is
    // simpler and more accurate to let the non-vsync wait() handle it via the
    // sampleInterval deadline.  So we only set useVSync=true for the 1:1 ratio.
    this->useVSync = std::abs(contentRefreshRate - screenRefreshRate) < FPS_TOLERANCE;
    this->sampleInterval = std::chrono::microseconds((long)((1000000L / contentRefreshRate)));
    reset();
}

void FPSSync::start() {
    LOGI(
        "Starting game: core=%.3f Hz, screen=%.3f Hz, useVSync=%d, sampleInterval=%ld µs",
        contentRefreshRate,
        screenRefreshRate,
        useVSync,
        (long)(1000000L / contentRefreshRate)
    );
    lastFrame = std::chrono::steady_clock::now();
}

void FPSSync::reset() {
    lastFrame = MIN_TIME;
}

double FPSSync::getTimeStretchFactor() {
    return useVSync ? contentRefreshRate / screenRefreshRate : 1.0;
}

void FPSSync::wait() {
    if (useVSync) return;
    // Hybrid approach: sleep until ~1 ms before deadline, then busy-spin.
    // This eliminates the OS scheduler jitter (~1–3 ms) that causes frame drops
    // while avoiding a full busy-spin that would burn the CPU.
    constexpr auto SPIN_THRESHOLD = std::chrono::microseconds(800);
    const auto deadline = lastFrame;
    const auto sleepUntil = deadline - SPIN_THRESHOLD;
    std::this_thread::sleep_until(sleepUntil);
    // Busy-spin the last ~800 µs for precise wakeup
    while (std::chrono::steady_clock::now() < deadline) {
        // __builtin_arm_yield() hints the CPU to yield in a spin-wait loop,
        // reducing power consumption and contention on hyperthreaded cores.
        __builtin_arm_yield();
    }
}

} //namespace libretrodroid
