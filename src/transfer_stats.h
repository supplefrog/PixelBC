// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
// Additive diagnostics: preserve the existing PvbStats binary layout.
struct PvbTransferStats {
    uint32_t version;
    uint64_t live_block_stores, live_block_bytes, recorded_block_copy_bytes;
};
using PvbGetTransferStats = void (*)(PvbTransferStats*);
