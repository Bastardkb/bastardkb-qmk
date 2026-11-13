// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The module's split transaction ID (injected into `enum serial_transaction_id`
// via the generated split_transaction_id_community_modules.inc).
#define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH

// RPC buffer sizing (global split RPC macros). m2s packets carry
// opcode + seq + payload + CRC; s2m returns the capability/ACK.
#ifndef RPC_M2S_BUFFER_SIZE
#    define RPC_M2S_BUFFER_SIZE 246
#endif
#ifndef RPC_S2M_BUFFER_SIZE
#    define RPC_S2M_BUFFER_SIZE 16
#endif

// Flash layout knobs (Phase 1+). Staging slot: the preliminary new image is
// written here, verified, then committed into the active region. All offsets
// are flash offsets from the XIP base (0x10000000).
#ifndef SPLIT_FLASH_STAGING_OFFSET
#    define SPLIT_FLASH_STAGING_OFFSET (1 * 1024 * 1024)
#endif
#ifndef SPLIT_FLASH_MAX_IMAGE_SIZE
#    define SPLIT_FLASH_MAX_IMAGE_SIZE (256 * 1024)
#endif

// ---------------------------------------------------------------------------
// TEMPORARY Phase 1 on-device test hook knobs. The hook lets the safety core be
// exercised with one UF2 before any transport exists: hold the configured key
// on the secondary half at power-on to run erase-staging -> stage the board's
// own image -> verify -> arm+commit -> reboot. REMOVED in Phase 2 (replaced by
// the capability/fingerprint handshake). The final product must not carry it.
// ---------------------------------------------------------------------------
#ifndef BK_SPLIT_FLASH_TEST_ENABLE
#    define BK_SPLIT_FLASH_TEST_ENABLE 1
#endif

#ifndef BK_SPLIT_FLASH_TEST_KEY_ROW
#    define BK_SPLIT_FLASH_TEST_KEY_ROW 0
#endif
#ifndef BK_SPLIT_FLASH_TEST_KEY_COL
#    define BK_SPLIT_FLASH_TEST_KEY_COL 0
#endif

// The trigger requires the key to be held continuously this long after boot.
#ifndef BK_SPLIT_FLASH_TEST_HOLD_MS
#    define BK_SPLIT_FLASH_TEST_HOLD_MS 1000
#endif
// Solid-LED "armed" pause before staging starts (cut power here = nothing touched).
#ifndef BK_SPLIT_FLASH_TEST_START_DELAY_MS
#    define BK_SPLIT_FLASH_TEST_START_DELAY_MS 3000
#endif
// Solid-LED "verified, about to commit" pause (cut power here = old image boots).
#ifndef BK_SPLIT_FLASH_TEST_COMMIT_DELAY_MS
#    define BK_SPLIT_FLASH_TEST_COMMIT_DELAY_MS 3000
#endif
// LED-on pause after boot2 is written, before the reboot (cut = new image boots).
#ifndef BK_SPLIT_FLASH_TEST_REBOOT_DELAY_MS
#    define BK_SPLIT_FLASH_TEST_REBOOT_DELAY_MS 2000
#endif

// LED phases use the board's bootloader status LED (GP17 on BKB RP2040 boards).
#ifndef BK_SPLIT_FLASH_PROGRESS_LED
#    if defined(RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED)
#        define BK_SPLIT_FLASH_PROGRESS_LED RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED
#    endif
#endif