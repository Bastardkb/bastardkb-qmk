// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// argos/config.h declares PUT_BK_SPLIT_FLASH in SPLIT_TRANSACTION_IDS_KB;
// argos/split_transaction_ids.h permanently assigns it to 25. The keyboard RPC
// IDs also provide the shared-memory buffers reused by this module.

// The chunk is carried in the RPC m2s shared-memory buffer (a module has no
// buffer of its own), so that buffer must hold one full-size chunk. The default
// is 32 bytes.
#define RPC_M2S_BUFFER_SIZE 240

// SPLIT_MODS_ENABLE is frozen by argos/config.h, not locally here: argos is
// common to the BastardKB keymaps, and both transaction and shared-memory
// layouts must stay stable when optional modules are added on only one half.

// The master's firmware fingerprint (whole-image CRC-32, 4 bytes) is stored in
// the argos EEPROM block at ARGOS_OFFSET_SPLIT_FLASH_CONFIG; see
// argos/post_config.h.
