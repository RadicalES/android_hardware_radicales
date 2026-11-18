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

#include <cstdint>
#include <string>
#include <vector>

namespace ddc {

// VESA MCCS feature code for luminance (the monitor's brightness control).
constexpr uint8_t kVcpLuminance = 0x10;

// A connected display whose monitor answers DDC/CI.
struct Monitor {
    std::string connector;  // DRM connector, e.g. card0-HDMI-A-1
    int bus = -1;           // /dev/i2c-<bus>
    int fd = -1;            // open on the DDC/CI address
    int max = 0;            // monitor's maximum for kVcpLuminance
};

// Finds the connected DRM connectors (/sys/class/drm) and, through each one's
// ddc link, the I2C bus of its monitor. Returns those that answer a DDC/CI
// luminance query. Only these buses are ever opened.
std::vector<Monitor> findMonitors();

void close(Monitor& monitor);

// VCP (MCCS) feature access over DDC/CI. Each call waits out the delays the
// DDC/CI spec requires, so it takes some 40-50 ms.
bool getVcp(int fd, uint8_t code, int* current, int* max);
bool setVcp(int fd, uint8_t code, int value);

}  // namespace ddc
