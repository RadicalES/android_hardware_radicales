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

#pragma once

#include <aidl/android/hardware/light/BnLights.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "Ddc.h"

namespace aidl::android::hardware::light {

// The BACKLIGHT light for external monitors, which have no Linux backlight
// device: brightness goes to the monitor itself over DDC/CI.
class Lights : public BnLights {
  public:
    Lights();
    ~Lights() override;

    ndk::ScopedAStatus setLightState(int id, const HwLightState& state) override;
    ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) override;

  private:
    // DDC/CI is slow (~50 ms a write), so a worker thread writes the monitor
    // and binder calls only hand it the latest value: a slider drag collapses
    // into a few writes.
    void worker();
    bool apply(int brightness);

    std::mutex mLock;
    std::condition_variable mCond;
    bool mStop = false;
    int mRequested = -1;  // 1..255, -1 when nothing is pending

    // Worker thread only.
    std::vector<ddc::Monitor> mMonitors;
    int mApplied = -1;

    std::thread mThread;
};

}  // namespace aidl::android::hardware::light
