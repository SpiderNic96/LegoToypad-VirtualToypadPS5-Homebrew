/* The console picker overlay must have a way into ShellUI on every firmware from 7.00 to 13.60:
 * the Application.Update hook, or else the background (main-thread guard) mode.
 * Copyright (C) 2026 SpiderNic96. SPDX-License-Identifier: GPL-3.0-or-later */
#include <cstdio>
#include <cstdint>
#include "common_fps/shellui_hook_protocol.hpp"
using namespace common_fps;
int main() {
    const unsigned fws[] = {0x0700, 0x0701, 0x0720, 0x0740, 0x0760, 0x0761, 0x0800, 0x0820, 0x0840, 0x0860,
                            0x0900, 0x0905, 0x0920, 0x0940, 0x0960, 0x1000, 0x1001, 0x1020, 0x1040, 0x1060,
                            0x1100, 0x1120, 0x1140, 0x1160, 0x1200, 0x1202, 0x1220, 0x1240, 0x1260, 0x1270,
                            0x1300, 0x1320, 0x1340, 0x1342, 0x1360};
    int bad = 0;
    for (unsigned f : fws) {
        const std::uint32_t sdk = f << 16;
        const bool probe = shellui_hook_probe_backend(sdk) != ShellUiHookBackend::Unsupported;
        const bool update = shellui_hook_backend(sdk, false) != ShellUiHookBackend::Unsupported;
        const bool guard = shellui_main_thread_guard_backend(sdk) != ShellUiHookBackend::Unsupported;
        if (!probe || !(update || guard)) { std::printf("FW %x.%02x: no overlay path\n", f >> 8, f & 0xff); bad++; }
    }
    if (bad) return 1;
    std::printf("overlay firmware paths: 7.00-13.60 all covered\n");
    return 0;
}
