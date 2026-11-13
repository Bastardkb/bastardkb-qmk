// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The module's split transaction ID (injected into `enum serial_transaction_id`
// via the generated split_transaction_id_community_modules.inc).
#define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH

// RPC buffer sizing (global split RPC macros). m2s carries the capability probe
// (magic/version/seq + image length + CRC-32); s2m returns the peer's own
// version/length/CRC-32 reply.
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
// Handshake knobs (Phase 2). The master's capability probe is settled by the
// first valid reply or by this bound, whichever comes first, so the primary's
// boot is never blocked (plan section 5 / section 13 Phase 2). The default is
// 4000 ms because on the dilemma the secondary boots ~3 s later than the
// primary (measured 2026-11-13); a shorter window makes the primary give up
// before the secondary can answer. The probe never blocks the main loop: it is
// rate-limited housekeeping work.
// ---------------------------------------------------------------------------
#ifndef SPLIT_FLASH_HANDSHAKE_TIMEOUT
#    define SPLIT_FLASH_HANDSHAKE_TIMEOUT 4000 // ms; whole-probe bound, must cover the halves' boot skew
#endif
// Internal handshake cadence: the master re-probes every PROBE_INTERVAL_MS while
// inside the timeout window, so a still-booting secondary is not skipped by one
// raced probe. Not a Phase-4-finalized knob.
#ifndef BK_SPLIT_FLASH_PROBE_INTERVAL_MS
#    define BK_SPLIT_FLASH_PROBE_INTERVAL_MS 200
#endif

// LED phases use the board's bootloader status LED (GP17 on BKB RP2040 boards);
// best-effort during the Phase 3 commit, no longer part of the handshake.
#ifndef BK_SPLIT_FLASH_PROGRESS_LED
#    if defined(RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED)
#        define BK_SPLIT_FLASH_PROGRESS_LED RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED
#    endif
#endif