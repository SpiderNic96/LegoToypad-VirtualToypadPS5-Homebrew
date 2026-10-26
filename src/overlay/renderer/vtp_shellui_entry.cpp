/*
 * Virtual Toy Pad console overlay - renderer entry point.
 * Derived from Common FPS for PS5 (commonfps_shellui_entry.cpp):
 *   Copyright (C) 2026 porhe911
 *   Modifications Copyright (C) 2026 erickdavestech
 *   Modifications Copyright (C) 2026 SpiderNic96 (own marker/stage paths, TCP server)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vtp_shellui.hpp"
#include "common_fps/shellui_stage.hpp"

#include <cstdio>
#include <unistd.h>

namespace {

constexpr const char* kMarker = "/system_tmp/vtpovl_shellui.pid";

void write_online_marker() {
    FILE* fp = std::fopen(kMarker, "w");
    if (!fp)
        return;
    std::fprintf(fp, "%d\n", getpid());
    std::fclose(fp);
}

[[noreturn]] void park_renderer_failure(const char* stage) {
    std::FILE* fp = std::fopen("/system_tmp/vtpovl_shellui.log", "a");
    if (fp) {
        std::fprintf(fp, "renderer parked stage=%s\n", stage);
        std::fclose(fp);
    }
    /* Returning an injected ShellUI thread can destabilize the console (Common FPS TEST8),
     * so a failure keeps the thread resident and inert. */
    for (;;)
        usleep(1000000);
}

} // namespace

namespace common_fps::ps5::shellui {

void record_stage(const char* stage) noexcept {
    if (FILE* fp = std::fopen(kShellUiStagePath, "w")) {
        std::fprintf(fp, "%d %.47s\n", getpid(), stage);
        std::fclose(fp);
    }
}

} // namespace common_fps::ps5::shellui

extern "C" __attribute__((noreturn, visibility("default")))
void* elf_main(void* payload_args) {
    using namespace common_fps::ps5::shellui;
    (void)payload_args;   /* a shared renderer does not start the payload CRT in SceShellUI */

    record_stage("entry");
    if (std::FILE* fp = std::fopen("/system_tmp/vtpovl_shellui.log", "w")) {
        std::fputs("Virtual Toy Pad overlay renderer\n", fp);
        std::fclose(fp);
    }

    bool runtime_ready = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (initialize_runtime()) {
            runtime_ready = true;
            break;
        }
        usleep(1000000);
    }
    if (!runtime_ready)
        park_renderer_failure("runtime");
    record_stage("runtime_ready");

    if (!initialize_receiver())
        park_renderer_failure("receiver");
    record_stage("receiver_ready");

    write_online_marker();
    run_receiver_loop();
}
