// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Declares this module's split transaction id. The generated
// split_transaction_id_community_modules.inc expands it into the
// serial_transaction_id enum as PUT_BK_SPLIT_FLASH, and its presence turns on
// SPLIT_TRANSACTION_RPC, which provides the shared-memory buffers we reuse
// (a module has no buffer of its own).
#define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH

// Step 1 uses the RPC m2s shared-memory buffer as the chunk buffer, so it must
// hold one full-size chunk. The default is 32 bytes.
#define RPC_M2S_BUFFER_SIZE 240
