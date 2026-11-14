// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

// Split-secondary flash clone: the master streams its own firmware image to the
// secondary over the split link, one fixed-size transaction per chunk.
//
// Step 5: the whole flow is gated by an automatic trigger. On boot the master
// compares its own firmware fingerprint (whole-image CRC-32) against the value
// persisted in its module EEPROM datablock; a mismatch (new image, or a fresh
// zeroed datablock) means a new firmware was just written. Only then
// does the master stream its image to the secondary, which verifies it (boot2
// checksum + vector-table plausibility + read-back CRC) and commits it (erase
// active and the wear-leveling EEPROM, copy staging, boot2 last, reset), so the
// secondary boots the new image with a fresh EEPROM. Outside the trigger no stream runs
// and no re-flash happens. The re-clone guard still skips the commit when the
// staged image already matches the secondary's active image.
//
// The split protocol takes the byte counts from the transaction table, not the
// wire, so the sizes are fixed at compile time (unchanged from step 1's proven
// 240/4).
#define BK_SPLIT_FLASH_M2S_SIZE 240
#define BK_SPLIT_FLASH_S2M_SIZE 4

// Payload bytes per chunk: m2s minus the {seq, offset} words and the crc8 byte.
#define BK_SPLIT_FLASH_CHUNK_DATA (BK_SPLIT_FLASH_M2S_SIZE - 2 * sizeof(uint32_t) - sizeof(uint8_t))

// Reserved `seq` values; data chunks are numbered 1..N.
#define BK_SPLIT_FLASH_SEQ_BEGIN 0u            // offset carries the image length; (re)sets the slave state
#define BK_SPLIT_FLASH_SEQ_REPORT 0xFFFFFFFFu  // slave answers with its rolling CRC-32 over the received bytes
#define BK_SPLIT_FLASH_SEQ_VERIFY 0xFFFFFFFEu  // slave answers with a CRC-32 of the staging read-back
#define BK_SPLIT_FLASH_SEQ_PREPARE 0xFFFFFFFDu // offset carries the image length; requests deferred staging erase
// `offset` carries the force flag: non-zero commits even when the staged image
// already matches the active one (used by the step-4 validation one-shot).
#define BK_SPLIT_FLASH_SEQ_COMMIT 0xFFFFFFFCu
#define BK_SPLIT_FLASH_SEQ_READY 0xFFFFFFFBu   // slave reports whether deferred staging erase is complete

// Slave acknowledgements (s2m.ack). An accepted data chunk is acked with its own seq
// (1..N), so these sentinels cannot collide with a valid ack.
#define BK_SPLIT_FLASH_ACK_BEGIN 0xFFFFFFFFu     // begin / state reset accepted
#define BK_SPLIT_FLASH_ACK_SEQ_ERR 0u            // out-of-order chunk, or no stream active
#define BK_SPLIT_FLASH_ACK_CRC8_ERR 0xFFFFFFFEu  // chunk CRC-8 mismatch
#define BK_SPLIT_FLASH_ACK_LEN_ERR 0xFFFFFFFDu   // begin rejected: image larger than the erased staging slot
#define BK_SPLIT_FLASH_ACK_COMMIT 0xFFFFFFFBu    // bootable and (forced or differing): the secondary will commit
#define BK_SPLIT_FLASH_ACK_MATCH 0xFFFFFFFAu     // staged image already matches the active image; commit skipped
#define BK_SPLIT_FLASH_ACK_BOOT_ERR 0xFFFFFFF9u  // boot2 checksum or vector-table check failed
#define BK_SPLIT_FLASH_ACK_FLASH_ERR 0xFFFFFFF8u // staged read-back differs from what was received
#define BK_SPLIT_FLASH_ACK_PREPARE 0xFFFFFFF7u  // staging erase request accepted
#define BK_SPLIT_FLASH_ACK_READY 0xFFFFFFF6u    // requested staging length is erased
#define BK_SPLIT_FLASH_ACK_BUSY 0xFFFFFFF5u     // staging erase is pending or in progress

// The secondary commits from its housekeeping loop, not from the transaction
// callback: an erase+copy takes seconds and must run interrupts-off, which would
// freeze the split thread. The short delay lets the master receive the COMMIT ack
// before the link is torn down.
#define BK_SPLIT_FLASH_COMMIT_DELAY_MS 100

// Staging slot in flash (offset from the start of flash): 1 MiB in, 256 KiB
// capacity, clear of the running image and of the wear-leveling EEPROM region
// (base 0x1FE000 on this board). Only the image-length bytes are erased/written.
#define BK_SPLIT_FLASH_STAGING_OFFSET 0x100000u
#define BK_SPLIT_FLASH_STAGING_MAX 0x40000u

// A successful secondary commit leaves a small provenance marker outside the
// image slot. This survives the secondary EEPROM reset and prevents that side
// from looking like a newly flashed master if USB is later moved to it.
#define BK_SPLIT_FLASH_CLONE_MARKER_OFFSET (BK_SPLIT_FLASH_STAGING_OFFSET + BK_SPLIT_FLASH_STAGING_MAX)
#define BK_SPLIT_FLASH_CLONE_MARKER_MAGIC 0x424B434Cu // "BKCL"

// Master run cadence: the clone is one-shot (gated by the trigger), so there is
// no periodic re-run. Instead of a fixed delay to clear the ~3 s master/secondary
// boot skew, the master uses the BEGIN transaction itself as a readiness probe:
// the secondary only acks BEGIN once its split thread is up and its staging slot
// is erased, so the stream starts as soon as the secondary is actually able to
// receive it. Probes retry every PROBE_RETRY_MS and give up after
// PROBE_TIMEOUT_MS if the secondary never appears (e.g. a lone master).
#define BK_SPLIT_FLASH_PROBE_RETRY_MS 100
#define BK_SPLIT_FLASH_PROBE_TIMEOUT_MS 15000

// During the active data stream, temporary keyboard unresponsiveness is allowed;
// bound each master-loop burst so USB and recovery work still get service.
#define BK_SPLIT_FLASH_DATA_BURST_CHUNKS 8

typedef struct bk_split_flash_m2s_t {
    uint32_t seq;
    uint32_t offset;
    uint8_t  crc8;
    uint8_t  data[BK_SPLIT_FLASH_CHUNK_DATA];
} bk_split_flash_m2s_t;

typedef struct bk_split_flash_s2m_t {
    uint32_t ack;
} bk_split_flash_s2m_t;
