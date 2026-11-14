// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Declares this module's split transaction id. The generated
// split_transaction_id_community_modules.inc expands it into the
// serial_transaction_id enum as PUT_BK_SPLIT_FLASH, and its presence turns on
// SPLIT_TRANSACTION_RPC, which provides the shared-memory buffers we reuse
// (a module has no buffer of its own).
// #define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH

// The chunk is carried in the RPC m2s shared-memory buffer (a module has no
// buffer of its own), so that buffer must hold one full-size chunk. The default
// is 32 bytes.
#define RPC_M2S_BUFFER_SIZE 240

// Module EEPROM datablock: 4 bytes holding the master's firmware fingerprint
// (whole-image CRC-32). The master persists it after a successful clone and
// compares it on boot: any mismatch (a new image, or a zeroed/fresh datablock)
// means "new firmware".
#define EECONFIG_MODULE_BK_SPLIT_FLASH_DATA_SIZE 4
