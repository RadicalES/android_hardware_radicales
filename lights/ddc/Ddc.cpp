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

#include "Ddc.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace ddc {

namespace {

// DDC/CI (VESA DDC/CI 1.1): the monitor listens at I2C address 0x37. Host
// messages start with the host's source address 0x51 and a length byte
// (0x80 | n); the checksum XORs the destination address (0x6E, i.e. 0x37 << 1)
// with every byte. Replies start with the monitor's address 0x6E and are
// checked against the virtual host address 0x50.
constexpr uint16_t kDdcCiAddr = 0x37;
constexpr uint8_t kHostAddr = 0x51;
constexpr uint8_t kDestSeed = 0x6E;
constexpr uint8_t kReplySeed = 0x50;
constexpr uint8_t kGetVcp = 0x01;
constexpr uint8_t kGetVcpReply = 0x02;
constexpr uint8_t kSetVcp = 0x03;

// Delays from the DDC/CI spec: a reply is ready 40 ms after a request, and a
// monitor needs 50 ms after a set before the next message.
constexpr auto kReplyDelay = std::chrono::milliseconds(40);
constexpr auto kSetDelay = std::chrono::milliseconds(50);
constexpr int kTries = 3;

uint8_t checksum(uint8_t seed, const uint8_t* buf, size_t len) {
    uint8_t c = seed;
    for (size_t i = 0; i < len; i++) c ^= buf[i];
    return c;
}

int openBus(int bus) {
    std::string path = "/dev/i2c-" + std::to_string(bus);
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        PLOG(WARNING) << "open " << path;
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, kDdcCiAddr) < 0) {
        PLOG(WARNING) << path << ": I2C_SLAVE 0x37";
        ::close(fd);
        return -1;
    }
    return fd;
}

// The bus behind a connector's ddc link (.../i2c-9), or -1.
int ddcBus(const std::string& connectorDir) {
    std::string target;
    if (!android::base::Readlink(connectorDir + "/ddc", &target)) return -1;
    size_t slash = target.rfind('/');
    std::string name = slash == std::string::npos ? target : target.substr(slash + 1);
    int bus = -1;
    if (sscanf(name.c_str(), "i2c-%d", &bus) != 1) return -1;
    return bus;
}

}  // namespace

bool getVcp(int fd, uint8_t code, int* current, int* max) {
    uint8_t req[5] = {kHostAddr, 0x82, kGetVcp, code, 0};
    req[4] = checksum(kDestSeed, req, 4);

    for (int i = 0; i < kTries; i++) {
        if (write(fd, req, sizeof(req)) != sizeof(req)) continue;
        std::this_thread::sleep_for(kReplyDelay);

        // 0x6E 0x88 0x02 <result> <code> <type> <max hi> <max lo> <cur hi> <cur lo> <chk>
        uint8_t r[11] = {};
        if (read(fd, r, sizeof(r)) != sizeof(r)) continue;
        if (r[0] != kDestSeed || r[1] != 0x88 || r[2] != kGetVcpReply || r[4] != code ||
            r[10] != checksum(kReplySeed, r, 10)) {
            continue;
        }
        if (r[3] != 0) return false;  // monitor does not support the feature
        *max = (r[6] << 8) | r[7];
        *current = (r[8] << 8) | r[9];
        return true;
    }
    return false;
}

bool setVcp(int fd, uint8_t code, int value) {
    uint8_t msg[7] = {kHostAddr, 0x84, kSetVcp, code, uint8_t(value >> 8), uint8_t(value), 0};
    msg[6] = checksum(kDestSeed, msg, 6);

    bool ok = write(fd, msg, sizeof(msg)) == sizeof(msg);
    std::this_thread::sleep_for(kSetDelay);
    return ok;
}

std::vector<Monitor> findMonitors() {
    static const std::string kDrm = "/sys/class/drm";
    std::vector<Monitor> monitors;

    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(kDrm.c_str()), closedir);
    if (!dir) {
        PLOG(WARNING) << "opendir " << kDrm;
        return monitors;
    }
    while (dirent* e = readdir(dir.get())) {
        // Connectors are card<N>-<type>-<id>; card<N> and renderD<N> are not.
        std::string name = e->d_name;
        if (!android::base::StartsWith(name, "card") || name.find('-') == std::string::npos) {
            continue;
        }
        std::string connectorDir = kDrm + "/" + name;
        std::string status;
        if (!android::base::ReadFileToString(connectorDir + "/status", &status) ||
            android::base::Trim(status) != "connected") {
            continue;
        }
        int bus = ddcBus(connectorDir);
        if (bus < 0) continue;

        int fd = openBus(bus);
        if (fd < 0) continue;
        int current = 0, max = 0;
        if (!getVcp(fd, kVcpLuminance, &current, &max) || max <= 0) {
            LOG(INFO) << name << " (i2c-" << bus << "): monitor has no DDC/CI brightness";
            ::close(fd);
            continue;
        }
        LOG(INFO) << name << " (i2c-" << bus << "): DDC/CI brightness " << current << "/" << max;
        monitors.push_back({name, bus, fd, max});
    }
    return monitors;
}

void close(Monitor& monitor) {
    if (monitor.fd >= 0) ::close(monitor.fd);
    monitor.fd = -1;
}

}  // namespace ddc
