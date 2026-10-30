/*
 * Virtual Toy Pad console overlay - ShellUI controller.
 *
 * Loads the overlay renderer into SceShellUI and services its one-shot hook requests
 * (Application.Update / main-thread guard), using the hardware-proven Common FPS /
 * SimpleFPS / HUD overlay loader (stable_shellui_injector, shellui_hook_controller).
 *
 * Unlike the HUD overlay's ensure_shellui_renderer(), this is a non-blocking state
 * machine: ovl_ctl_tick() is called from the injector's monitor loop every ~25 ms, the
 * same thread that owns every other kernel read/write in ldinject (the SDK's kernel r/w
 * channel is not thread-safe). Only the actual injection blocks, for about a second.
 *
 * Copyright (C) 2026 SpiderNic96. Control flow modelled on shellui_injector.cpp,
 * Copyright (C) 2026 porhe911 / erickdavestech.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovl_controller.h"
#include "shellui_hook_controller.hpp"
#include "stable_shellui_injector.hpp"
#include "common_fps/shellui_stage.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

extern "C" void web_log(const char* fmt, ...);
extern "C" const unsigned char OVL_RENDERER_ELF[];
extern "C" const unsigned int OVL_RENDERER_ELF_len;

namespace {

using namespace common_fps::ps5;

constexpr const char* kMarker = "/system_tmp/vtpovl_shellui.pid";

enum class Phase { Off, Scan, WaitMarker, Online, Quarantine };

Phase g_phase = Phase::Scan;
pid_t g_shellui = -1;
pid_t g_seen = -1;
unsigned g_seen_ticks = 0;
std::uint64_t g_next_scan = 0;
std::uint64_t g_wait_start = 0;
std::uint64_t g_last_pid_check = 0;
int g_enabled = 1;
volatile int g_online = 0;

/* crash-loop guard (see ovl_controller.h) */
constexpr std::uint64_t kCrashWindowMs = 180000;   /* a ShellUI restart this soon after loading = ours */
std::uint64_t g_inject_t = 0;                       /* when the renderer was last loaded into ShellUI */
char g_guard_path[160];
void (*g_guard_notify)(const char*) = nullptr;
volatile int g_guarded = 0;

std::uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (std::uint64_t)ts.tv_sec * 1000 + (std::uint64_t)ts.tv_nsec / 1000000;
}

pid_t find_shellui() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t sz = 0;
    if (sysctl(mib, 4, nullptr, &sz, nullptr, 0) || !sz) return -1;
    sz += sz / 4 + 4096;
    char* buf = static_cast<char*>(std::malloc(sz));
    if (!buf) return -1;
    pid_t found = -1;
    if (!sysctl(mib, 4, buf, &sz, nullptr, 0)) {
        for (char* p = buf; p + sizeof(int) <= buf + sz && found < 0;) {
            auto* ki = reinterpret_cast<struct kinfo_proc*>(p);
            if (ki->ki_structsize <= 0) break;
            if (!std::strcmp(ki->ki_comm, "SceShellUI")) found = ki->ki_pid;
            p += ki->ki_structsize;
        }
    }
    std::free(buf);
    return found;
}

/* when this boot started (seconds): the guard file only counts for the boot that wrote it */
long boot_time() {
    struct timeval tv{};
    size_t len = sizeof tv;
    if (!sysctlbyname("kern.boottime", &tv, &len, nullptr, 0) && tv.tv_sec > 0) return tv.tv_sec;
    timespec up{};
    clock_gettime(CLOCK_MONOTONIC, &up);
    return (long)std::time(nullptr) - (long)up.tv_sec;
}

bool same_boot(long a, long b) { return a > 0 && b > 0 && (a > b ? a - b : b - a) <= 10; }

/* "<boot time> <ShellUI pid that crashed>" */
bool guard_tripped_this_boot() {
    if (!g_guard_path[0]) return false;
    FILE* fp = std::fopen(g_guard_path, "r");
    if (!fp) return false;
    long bt = 0;
    int pid = 0;
    const int n = std::fscanf(fp, "%ld %d", &bt, &pid);
    std::fclose(fp);
    return n >= 1 && same_boot(bt, boot_time());
}

void guard_trip(pid_t old_pid, pid_t new_pid, const char* when) {
    g_guarded = 1;
    g_online = 0;
    g_phase = Phase::Off;
    web_log("overlay: SceShellUI restarted %llus after the picker was loaded into it (%s; pid %d -> %d) - "
            "treating it as a crash: the console picker stays off until the next reboot",
            (unsigned long long)((now_ms() - g_inject_t) / 1000), when, old_pid, new_pid);
    if (g_guard_path[0]) {
        if (FILE* fp = std::fopen(g_guard_path, "w")) { std::fprintf(fp, "%ld %d\n", boot_time(), old_pid); std::fclose(fp); }
    }
    if (g_guard_notify)
        g_guard_notify("The system software restarted after the console picker loaded.\n"
                       "The picker is off until you reboot - the phone remote still works.");
}

/* SceShellUI has a new pid. Returns true when the guard tripped (the caller stops). */
bool shellui_restarted(pid_t old_pid, pid_t new_pid, const char* when) {
    if (g_inject_t && now_ms() - g_inject_t < kCrashWindowMs) { guard_trip(old_pid, new_pid, when); return true; }
    return false;
}

bool marker_matches(pid_t pid) {
    FILE* fp = std::fopen(kMarker, "r");
    if (!fp) return false;
    int marked = -1;
    const int rc = std::fscanf(fp, "%d", &marked);
    std::fclose(fp);
    return rc == 1 && marked == pid;
}

const char* last_stage(pid_t pid, char (&stage)[48]) {
    std::snprintf(stage, sizeof stage, "unavailable");
    FILE* fp = std::fopen(common_fps::kShellUiStagePath, "r");
    if (!fp) return stage;
    int recorded = -1;
    char text[48]{};
    if (std::fscanf(fp, "%d %47s", &recorded, text) == 2 && recorded == pid)
        std::memcpy(stage, text, sizeof text);
    std::fclose(fp);
    return stage;
}

void inject(pid_t pid) {
    clear_shellui_hook_protocol_files();
    (void)unlink(common_fps::kShellUiStagePath);
    web_log("overlay: injecting renderer into SceShellUI pid %d (%u bytes)", pid, OVL_RENDERER_ELF_len);
    g_inject_t = now_ms();
    const std::uint64_t t0 = now_ms();
    const StableInjectionResult r = inject_shellui_stable(pid, OVL_RENDERER_ELF, OVL_RENDERER_ELF_len,
                                                          StableInjectionMode::StartRendererThread);
    web_log("overlay: bootstrap auth=%d/%d attach=%d load=%d args=%d thread=%d rc=%d detach=%d "
            "imports=%u/%u first=%s %llums",
            r.auth_changed, r.auth_restored, r.attached, r.elf_loaded, r.payload_args_ready,
            r.bootstrap_started, r.pthread_create_rc, r.detached, r.imports_resolved, r.imports_unresolved,
            r.first_unresolved && *r.first_unresolved ? r.first_unresolved : "none",
            (unsigned long long)(now_ms() - t0));
    if (!r.pthread_create_ok || !r.auth_restored) {
        /* never stack a second image into the same ShellUI once one may exist */
        g_phase = r.elf_loaded ? Phase::Quarantine : Phase::Scan;
        g_next_scan = now_ms() + 5000;
        web_log("overlay: renderer start failed (%s)", r.elf_loaded ? "no retry for this ShellUI" : "retry in 5 s");
        return;
    }
    g_phase = Phase::WaitMarker;
    g_wait_start = now_ms();
}

} // namespace

extern "C" void ovl_ctl_enable(int on) { g_enabled = on; }
extern "C" void ovl_ctl_set_guard(const char* path, void (*notify)(const char*)) {
    std::snprintf(g_guard_path, sizeof g_guard_path, "%s", path ? path : "");
    g_guard_notify = notify;
    if (guard_tripped_this_boot()) {   /* the payload was sent again after a crash in this boot */
        g_guarded = 1;
        g_phase = Phase::Off;
        web_log("overlay: SceShellUI crashed after loading the console picker earlier this boot; "
                "the picker stays off until the next reboot");
    }
}
extern "C" int ovl_ctl_guarded(void) { return g_guarded; }
extern "C" int ovl_ctl_online(void) { return g_online; }

extern "C" void ovl_ctl_tick(void) {
    if (!g_enabled) return;
    const std::uint64_t t = now_ms();
    switch (g_phase) {
    case Phase::Off:
        return;
    case Phase::Scan: {
        if (t < g_next_scan) return;
        g_next_scan = t + 1000;
        const pid_t pid = find_shellui();
        if (pid <= 0) { g_seen = -1; return; }
        if (pid != g_seen) { g_seen = pid; g_seen_ticks = 0; }
        if (marker_matches(pid)) {   /* already resident from an earlier run of this payload */
            g_shellui = pid;
            g_phase = Phase::Online;
            g_online = 1;
            web_log("overlay: renderer already running in SceShellUI pid %d", pid);
            return;
        }
        if (++g_seen_ticks < 3) return;   /* ShellUI stable for a few seconds */
        g_shellui = pid;
        inject(pid);
        return;
    }
    case Phase::WaitMarker: {
        if (marker_matches(g_shellui)) {
            g_phase = Phase::Online;
            g_online = 1;
            web_log("overlay: renderer online in SceShellUI pid %d after %llums", g_shellui,
                    (unsigned long long)(t - g_wait_start));
            return;
        }
        if (t - g_last_pid_check > 1000) {
            g_last_pid_check = t;
            const pid_t now = find_shellui();
            if (now > 0 && now != g_shellui) {
                if (shellui_restarted(g_shellui, now, "while it was starting")) return;
                char stage[48];
                web_log("overlay: ShellUI restarted before the renderer came up (last stage %s); not retrying",
                        last_stage(g_shellui, stage));
                g_phase = Phase::Quarantine;
                return;
            }
        }
        ShellUiHookPatchReport report{};
        const ShellUiHookPollResult res = poll_and_apply_shellui_hook(g_shellui, report);
        if (res != ShellUiHookPollResult::NoRequest)
            web_log("overlay: hook request sdk=0x%08x mode=%s backend=%s phase=%s method=0x%llx status=%d "
                    "read=%d write=%d verified=%d detached=%d auth=%d",
                    report.sdk_version, report.main_thread_guard ? "thread-guard" : (report.eta_chain ? "eta-chain" : "native"),
                    report.backend, report.probe ? "probe" : (report.main_thread_guard ? "guard" : "patch"),
                    (unsigned long long)report.method_address, (int)report.status, report.read_rc, report.write_rc,
                    report.verified, report.detached, report.auth_restored);
        if (t - g_wait_start > 75000) {
            char stage[48];
            web_log("overlay: renderer did not come up (last stage %s); classic menu stays active",
                    last_stage(g_shellui, stage));
            g_phase = Phase::Quarantine;
        }
        return;
    }
    case Phase::Online:
        if (t - g_last_pid_check > 3000) {
            g_last_pid_check = t;
            const pid_t now = find_shellui();
            if (now > 0 && now != g_shellui) {   /* ShellUI restarted (e.g. after rest mode): load again */
                if (shellui_restarted(g_shellui, now, "while it was running")) return;
                web_log("overlay: SceShellUI restarted (pid %d -> %d)", g_shellui, now);
                g_online = 0;
                g_phase = Phase::Scan;
                g_seen = -1;
            }
        }
        return;
    case Phase::Quarantine:
        if (t - g_last_pid_check > 3000) {
            g_last_pid_check = t;
            const pid_t now = find_shellui();
            if (now > 0 && now != g_shellui) {
                if (shellui_restarted(g_shellui, now, "after it failed to start")) return;
                g_phase = Phase::Scan; g_seen = -1;
            }
        }
        return;
    }
}
