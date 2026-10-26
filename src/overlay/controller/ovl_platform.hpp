/* Firmware version helper used by the reused Common FPS / SimpleFPS hook controller.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <sys/sysctl.h>

namespace common_fps::ps5::stable_sampler {
inline std::uint32_t firmware_sdk_version() noexcept {
    std::uint32_t sdk = 0;
    std::size_t len = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &len, nullptr, 0) != 0 || len != sizeof(sdk))
        return 0;
    return sdk;
}
} // namespace common_fps::ps5::stable_sampler
