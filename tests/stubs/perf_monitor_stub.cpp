// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>
#include "common/types.h"

namespace Common {

// The SHADGT_DISABLE_PERF switches as common/perf_monitor.cpp reads them, so the inspection
// tools keep their performance-switch behavior in standalone tests. perf_monitor.h declares
// PerfFeatureEnabled in every build (DIAG-246), so the stub defines it in every build too.
bool PerfFeatureEnabled(u32 id) {
    const char* env = std::getenv("SHADGT_DISABLE_PERF");
    if (!env) return true;
    const std::string list = std::string(",") + env + ",";
    return list.find("," + std::to_string(id) + ",") == std::string::npos;
}

} // namespace Common
