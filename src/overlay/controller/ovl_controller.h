/* Virtual Toy Pad console overlay - ShellUI controller (see ovl_controller.cpp).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* monitor thread only (it owns kernel r/w): scans for SceShellUI, loads the renderer once,
 * services its hook requests while it starts up */
void ovl_ctl_tick(void);
void ovl_ctl_enable(int on);
/* 1 once the renderer has reported in from SceShellUI */
int ovl_ctl_online(void);
/* Crash-loop guard. If SceShellUI restarts within a few minutes of the renderer being loaded into
 * it, that is treated as the renderer having crashed it: the overlay stays off for the rest of
 * this boot instead of being loaded again (which would crash it again, in a loop). `path` is a
 * small file in the payload's data folder that keeps this across payload restarts in the same
 * boot; `notify` (may be 0) tells the user. Call both before the first ovl_ctl_tick(). */
void ovl_ctl_set_guard(const char *path, void (*notify)(const char *msg));
/* 1 when the guard has switched the overlay off for this boot */
int ovl_ctl_guarded(void);
#ifdef __cplusplus
}
#endif
