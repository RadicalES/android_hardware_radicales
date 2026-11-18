/*
 * Copyright (C) 2026 RadicalES
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights-ddc"

#include "Lights.h"

#include <android-base/logging.h>

#include <chrono>

namespace aidl::android::hardware::light {

namespace {

const HwLight kBacklight = {.id = 0, .ordinal = 0, .type = LightType::BACKLIGHT};

// At boot the monitor may not have answered yet (HDMI still coming up): keep
// looking for this long before dropping a request.
constexpr int kFindTries = 12;
constexpr auto kFindInterval = std::chrono::seconds(5);

// The framework sends the backlight level as a grey ARGB colour.
int brightnessOf(const HwLightState& state) {
    uint32_t c = state.color;
    return ((77 * ((c >> 16) & 0xff)) + (150 * ((c >> 8) & 0xff)) + (29 * (c & 0xff))) >> 8;
}

}  // namespace

Lights::Lights() : mThread(&Lights::worker, this) {}

Lights::~Lights() {
    {
        std::lock_guard lock(mLock);
        mStop = true;
    }
    mCond.notify_one();
    mThread.join();
    for (auto& m : mMonitors) ddc::close(m);
}

ndk::ScopedAStatus Lights::setLightState(int id, const HwLightState& state) {
    if (id != kBacklight.id) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    int brightness = brightnessOf(state);
    // The framework sends 0 when the screen turns off. The display output is
    // off then anyway; a monitor at its lowest level would stay that dark
    // through the next boot, so keep its level instead.
    if (brightness == 0) return ndk::ScopedAStatus::ok();

    {
        std::lock_guard lock(mLock);
        mRequested = brightness;
    }
    mCond.notify_one();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Lights::getLights(std::vector<HwLight>* lights) {
    lights->push_back(kBacklight);
    return ndk::ScopedAStatus::ok();
}

// Writes brightness (1..255) to every monitor, scaled to each one's range.
// Drops the monitors when a write fails (unplugged, or the bus went away).
bool Lights::apply(int brightness) {
    if (mMonitors.empty()) mMonitors = ddc::findMonitors();
    if (mMonitors.empty()) return false;

    bool ok = true;
    for (auto& m : mMonitors) {
        int value = std::max(1, (brightness * m.max + 127) / 255);
        if (!ddc::setVcp(m.fd, ddc::kVcpLuminance, value)) {
            LOG(WARNING) << m.connector << ": DDC/CI write failed";
            ok = false;
        }
    }
    if (!ok) {
        for (auto& m : mMonitors) ddc::close(m);
        mMonitors.clear();
    }
    return ok;
}

void Lights::worker() {
    std::unique_lock lock(mLock);
    int tries = 0;
    while (true) {
        if (tries > 0) {
            mCond.wait_for(lock, kFindInterval, [this] { return mStop; });
        } else {
            mCond.wait(lock, [this] { return mStop || mRequested >= 0; });
        }
        if (mStop) return;
        if (mRequested < 0) {
            tries = 0;
            continue;
        }

        int brightness = mRequested;
        lock.unlock();
        // A new value, or the monitors were found again (they keep their own
        // level, so write it even when the value is the same as before).
        bool ok = brightness == mApplied && !mMonitors.empty();
        if (!ok) {
            ok = apply(brightness) || apply(brightness);  // once more after a failed write
        }
        lock.lock();

        if (ok) {
            mApplied = brightness;
            tries = 0;
        } else if (++tries >= kFindTries) {
            LOG(WARNING) << "no monitor with DDC/CI brightness, dropping level " << brightness;
            tries = 0;
        } else {
            continue;  // keep the request and look again
        }
        // Done unless a newer value came in meanwhile.
        if (mRequested == brightness) mRequested = -1;
    }
}

}  // namespace aidl::android::hardware::light
