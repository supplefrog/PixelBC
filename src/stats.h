// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
struct PvbStats {
    uint32_t version;
    uint64_t decode_dispatches, recording_failures, live_upload_resources, backend_recursion_rejections;
    char backend[256];
};
using PvbGetStats = void (*)(PvbStats*);
using PvbGetVersion = const char* (*)();
