// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
// Optional observation ABI. 0.1.2 does not export this: report unavailable.
struct PvbPerfStats {
    uint32_t version;
    uint64_t buffer_create_calls, memory_allocate_calls, descriptor_pool_create_calls;
    uint64_t requested_buffer_bytes, replayed_binds, replayed_pushes;
};
using PvbGetPerfStats=void (*)(PvbPerfStats*);
