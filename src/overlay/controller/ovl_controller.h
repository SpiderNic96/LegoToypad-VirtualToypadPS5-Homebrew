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
#ifdef __cplusplus
}
#endif
