// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bk_split_flash: split secondary flash copy.
// This header is the HAL boundary between the protocol/transport logic
// (bk_split_flash.c) and the action/flash writer (bk_split_flash_flash.c).
// The pure helpers (CRC-32, length and image-magic checks) are self-contained
// and host-testable: the native test harness includes only this header.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Protocol version for the capability/fingerprint probe/echo exchanged between
// the halves. The probe carries the initiator's image length + CRC-32 and the
// reply carries the peer's OWN version, length and CRC-32. There is no
// cross-version compatibility: whatever this module ships is the current
// protocol, and a reply that does not match it is treated as not-ready / not
// current and simply retried until the handshake window ends.
#define BK_SPLIT_FLASH_PROTOCOL_VERSION 2

// Master -> slave capability probe.
typedef struct {
    uint8_t  magic;     // BK_SPLIT_FLASH_MAGIC, sanity check
    uint8_t  version;   // BK_SPLIT_FLASH_PROTOCOL_VERSION
    uint8_t  seq;       // echoed back so the round-trip is observable
    uint32_t image_len; // initiator's image length [XIP_BASE, __flash_binary_end)
    uint32_t image_crc; // initiator's CRC-32 over that region
} bk_split_flash_probe_t;

// Slave -> master capability reply.
typedef struct {
    uint8_t  magic;     // BK_SPLIT_FLASH_MAGIC
    uint8_t  version;   // the peer's OWN protocol version
    uint8_t  seq;       // mirrored from the probe
    uint8_t  status;    // BK_SPLIT_FLASH_STATUS_* - capability/ack
    uint32_t image_len; // peer's image length
    uint32_t image_crc; // peer's CRC-32 over its own image
} bk_split_flash_echo_t;

// Master-side handshake decision (plan section 5 skip policy).
enum {
    BK_SPLIT_FLASH_DECISION_SKIP_TIMEOUT     = 0, // no reply within the handshake bound
    BK_SPLIT_FLASH_DECISION_SKIP_INVALID     = 1, // reply does not look like a v2 peer
    BK_SPLIT_FLASH_DECISION_SKIP_VERSION     = 2, // peer protocol version differs
    BK_SPLIT_FLASH_DECISION_SKIP_IDENTICAL   = 3, // same length + same CRC-32 -> nothing to do
    BK_SPLIT_FLASH_DECISION_BEGIN_TRANSFER   = 4, // fingerprints differ -> clone (Phase 3)
};

#define BK_SPLIT_FLASH_MAGIC 0x5F
#define BK_SPLIT_FLASH_STATUS_OK 0x00
// Failure/guard statuses for the flash action HAL.
#define BK_SPLIT_FLASH_STATUS_ERR_ALIGN 0x01 // write not page/offset-aligned as required
#define BK_SPLIT_FLASH_STATUS_ERR_RANGE 0x02 // offset/out-of-bounds write or bad state
#define BK_SPLIT_FLASH_STATUS_ERR_SIZE 0x03  // image length outside [MIN, MAX]
#define BK_SPLIT_FLASH_STATUS_ERR_MAGIC 0x04 // staged image fails the plausibility check
#define BK_SPLIT_FLASH_STATUS_ERR_CRC 0x05   // staged image CRC mismatch
#define BK_SPLIT_FLASH_STATUS_ERR_FLASH 0x06 // erase/program failure
#define BK_SPLIT_FLASH_STATUS_BUSY 0x07      // writer busy (staging in progress)

// A valid firmware image must at least contain boot2 + the vector table and
// some code. Used by the length/plausibility checks.
#ifndef BK_SPLIT_FLASH_MIN_IMAGE_SIZE
#    define BK_SPLIT_FLASH_MIN_IMAGE_SIZE 4096
#endif

// ---------------------------------------------------------------------------
// Pure, host-testable helpers (no hardware headers needed).
// ---------------------------------------------------------------------------

static inline uint32_t bk_split_flash_read32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Standard CRC-32 (IEEE 802.3 / "CRC-32/ISO-HDLC", poly 0xEDB88320).
// crc32("123456789") == 0xCBF43926.
static inline uint32_t bk_split_flash_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

static inline uint8_t bk_split_flash_btrev8(uint8_t x) {
    x = (uint8_t)(((x >> 1) & 0x55u) | ((x << 1) & 0xAAu));
    x = (uint8_t)(((x >> 2) & 0x33u) | ((x << 2) & 0xCCu));
    x = (uint8_t)(((x >> 4) & 0x0Fu) | ((x << 4) & 0xF0u));
    return x;
}

static inline uint32_t bk_split_flash_btrev32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x << 1) & 0xAAAAAAAAu);
    x = ((x >> 2) & 0x33333333u) | ((x << 2) & 0xCCCCCCCCu);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x << 4) & 0xF0F0F0F0u);
    x = ((x >> 8) & 0x00FF00FFu) | ((x << 8) & 0xFF00FF00u);
    return (x >> 16) | (x << 16);
}

// RP2040 boot2 checksum, as the bootrom validates it (empirically verified
// against all six prebuilt blobs in platforms/chibios/vendors/RP/stage2_bootloaders.c):
//   checksum = bitrev32(crc_reg(bitrev8_each_byte(boot2[0:252])))
// where crc_reg is the raw CRC register (no final complement), and the stored
// word at boot2[252:256] must equal that value.
static inline bool bk_split_flash_boot2_checksum_valid(const uint8_t *boot2) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 252; ++i) {
        crc ^= bk_split_flash_btrev8(boot2[i]);
        for (uint32_t b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return bk_split_flash_btrev32(crc) == bk_split_flash_read32_le(boot2 + 252);
}

static inline bool bk_split_flash_len_in_range(uint32_t len) {
    return len >= (uint32_t)BK_SPLIT_FLASH_MIN_IMAGE_SIZE && len <= (uint32_t)SPLIT_FLASH_MAX_IMAGE_SIZE;
}

// "Magic" header check for an RP2040 firmware image. The image layout is
// [0,256) boot2, [0x100,..) the ARM vector table, so the initial stack pointer
// and reset vector words live at offsets 0x100/0x104, and boot2 must carry the
// bootrom checksum. Build independent.
static inline bool bk_split_flash_image_plausible(const uint8_t *image, uint32_t len) {
    if (!bk_split_flash_len_in_range(len)) {
        return false;
    }
    uint32_t sp = bk_split_flash_read32_le(image + 0x100);
    uint32_t pc = bk_split_flash_read32_le(image + 0x104);
    if (sp < 0x20000000u || sp >= 0x20042000u) {
        return false;
    }
    if ((pc & 1u) == 0u) {
        return false;
    }
    pc &= ~1u;
    if (pc < 0x10000000u || pc >= (0x10000000u + (uint32_t)SPLIT_FLASH_MAX_IMAGE_SIZE)) {
        return false;
    }
    return bk_split_flash_boot2_checksum_valid(image);
}

// Reply acceptability for the current protocol: magic and seq echo back, and
// the reported image length is in the plausible range. Everything else is
// "not a current-firmware reply" (secondary mid-boot, not-yet-ready, or a
// peer running something else) and is retried until the handshake window ends -
// there is deliberately no handling for older internal versions. Host-testable.
static inline bool bk_split_flash_reply_usable(bool magic_ok, bool seq_ok, uint32_t peer_len) {
    return magic_ok && seq_ok && bk_split_flash_len_in_range(peer_len);
}

// Plan section 5 skip policy, given peer data that already passed
// bk_split_flash_reply_usable(). Only the version and the fingerprint decide.
static inline uint8_t bk_split_flash_handshake_decide(uint8_t peer_version, uint32_t peer_len, uint32_t peer_crc, uint32_t my_len, uint32_t my_crc) {
    if (peer_version != BK_SPLIT_FLASH_PROTOCOL_VERSION) {
        return BK_SPLIT_FLASH_DECISION_SKIP_VERSION;
    }
    if (peer_len == my_len && peer_crc == my_crc) {
        return BK_SPLIT_FLASH_DECISION_SKIP_IDENTICAL;
    }
    return BK_SPLIT_FLASH_DECISION_BEGIN_TRANSFER;
}

// ---------------------------------------------------------------------------
// Split-transport constraint guards (config-level; apply to every build unit
// that includes this header when the module is enabled).
// ---------------------------------------------------------------------------
#if defined(EE_HANDS)
#    error "bk_split_flash: EE_HANDS boards are not supported (a single image must be valid for both halves; used boards rely on SPLIT_HAND_PIN)."
#endif
#if defined(USE_I2C)
#    error "bk_split_flash: only the serial split transport is supported (the module's RPC and staging depend on the half-duplex serial protocol)."
#endif

// ---------------------------------------------------------------------------
// Action HAL — implemented in bk_split_flash_flash.c (RP2040 only).
// ---------------------------------------------------------------------------

// Length in bytes of this half's active image region [XIP_BASE, __flash_binary_end).
uint32_t bk_split_flash_active_image_len(void);

// Erase the staging slot and reset the staging writer. Returns STATUS_*.
uint8_t bk_split_flash_stage_begin(void);

// Append `len` bytes to the staged image. The master-facing transport writes
// chunks strictly sequentially (offset must equal the current cursor).
// Returns STATUS_*; STATUS_ERR_RANGE on non-sequential writes.
uint8_t bk_split_flash_stage_bytes(uint32_t offset, const void *data, uint32_t len);

// Flush the last partial page (padding with 0xFF) and finish staging.
uint8_t bk_split_flash_stage_end(void);

// Compare the staged image against the expected length + CRC-32, after an
// in-region length/magic check. Returns STATUS_OK / ERR_SIZE / ERR_MAGIC / ERR_CRC.
uint8_t bk_split_flash_staged_verify(uint32_t expected_len, uint32_t expected_crc);

// Arm (erase sector 0) + erase the rest of the active region, copy the staged
// image into it page by page, write boot2 last. Runs RAM-resident with
// interrupts disabled. Returns STATUS_* (STATUS_OK after boot2 is written).
uint8_t bk_split_flash_commit(uint32_t image_len);

// Configure the progress LED pin as an SIO output (call once per boot).
void bk_split_flash_progress_led_init(void);