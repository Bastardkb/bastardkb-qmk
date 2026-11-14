// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

// Split-secondary flash clone: the master streams its own firmware image to the
// secondary over the split link, one fixed-size transaction per chunk.
//
// Step 4: on top of step 3 (write the stream to staging and verify the read-back
// CRC), the secondary also proves the staged image is *bootable* (RP2040 boot2
// checksum + vector-table plausibility) and then commits it: one interrupts-off,
// RAM-resident routine erases the active region, copies staging over it (boot2
// last) and resets into the new image.
//
// The commit is skipped when the staged image already matches the secondary's
// active image, so the 20 s cadence does not reboot the secondary forever. For
// this validation step the master forces exactly one commit on its first run
// after boot (the images are identical when both halves run the same UF2); step 5
// replaces that with a Bootmagic-gated trigger.
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
// `offset` carries the force flag: non-zero commits even when the staged image
// already matches the active one (used by the step-4 validation one-shot).
#define BK_SPLIT_FLASH_SEQ_COMMIT 0xFFFFFFFCu

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

// The secondary commits from its housekeeping loop, not from the transaction
// callback: an erase+copy takes seconds and must run interrupts-off, which would
// freeze the split thread. The short delay lets the master receive the COMMIT ack
// before the link is torn down.
#define BK_SPLIT_FLASH_COMMIT_DELAY_MS 250

// Staging slot in flash (offset from the start of flash): 1 MiB in, 256 KiB
// capacity, clear of the running image and of the wear-leveling EEPROM region
// (base 0x1FE000 on this board). Only the image-length bytes are erased/written.
#define BK_SPLIT_FLASH_STAGING_OFFSET 0x100000u
#define BK_SPLIT_FLASH_STAGING_MAX 0x40000u

// Master run schedule: first run after boot, then one run every period. The first
// delay clears the ~3 s the two halves' boot times differ by on this board.
#define BK_SPLIT_FLASH_FIRST_RUN_DELAY_MS 5000
#define BK_SPLIT_FLASH_RUN_PERIOD_MS 20000

typedef struct bk_split_flash_m2s_t {
    uint32_t seq;
    uint32_t offset;
    uint8_t  crc8;
    uint8_t  data[BK_SPLIT_FLASH_CHUNK_DATA];
} bk_split_flash_m2s_t;

typedef struct bk_split_flash_s2m_t {
    uint32_t ack;
} bk_split_flash_s2m_t;
