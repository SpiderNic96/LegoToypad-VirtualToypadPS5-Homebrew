/* PS4 GoldHEN plugin: the plugin runs inside LEGO Dimensions, so "game memory" is our own
 * memory. These stand in for the PS5 SDK's cross-process kernel copy helpers used by
 * webremote.c, keeping that file shared between the two builds. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

static inline int kernel_proc_copyout(pid_t pid, intptr_t addr, void *buf, size_t len) {
    (void)pid;
    memcpy(buf, (const void *)addr, len);
    return 0;
}

static inline int kernel_proc_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len) {
    (void)pid;
    memcpy((void *)addr, buf, len);
    return 0;
}
