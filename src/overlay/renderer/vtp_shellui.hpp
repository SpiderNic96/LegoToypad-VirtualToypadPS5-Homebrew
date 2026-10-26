/* Virtual Toy Pad overlay renderer entry points (derived from Common FPS for PS5).
 * Copyright (C) 2026 porhe911; modifications (C) 2026 SpiderNic96.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

namespace common_fps::ps5::shellui {
void record_stage(const char* stage) noexcept;
bool initialize_runtime();
bool initialize_receiver();
[[noreturn]] void run_receiver_loop();
} // namespace common_fps::ps5::shellui
