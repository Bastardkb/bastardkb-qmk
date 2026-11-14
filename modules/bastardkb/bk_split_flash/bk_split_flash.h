// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

// Split-secondary flash clone: the master streams its own firmware image to the
// secondary over the split link, one fixed-size transaction per chunk.
//
// Step 3: the secondary writes each received chunk into a staging slot in flash
// (erased at boot, ahead of the stream) and the result is verified by reading the
// slot back and comparing its CRC-32 against the master's whole-image CRC-32. No
// commit and no reboot: the keyboard stays usable. Transport and flash integrity
// are reported with distinct markers, so a failure is unambiguously one or the
// other.
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

// Slave acknowledgements (s2m.ack). An accepted data chunk is acked with its own seq
// (1..N), so these sentinels cannot collide with a valid ack.
#define BK_SPLIT_FLASH_ACK_BEGIN 0xFFFFFFFFu    // begin / state reset accepted
#define BK_SPLIT_FLASH_ACK_SEQ_ERR 0u           // out-of-order chunk, or no stream active
#define BK_SPLIT_FLASH_ACK_CRC8_ERR 0xFFFFFFFEu // chunk CRC-8 mismatch
#define BK_SPLIT_FLASH_ACK_LEN_ERR 0xFFFFFFFDu  // begin rejected: image larger than the erased staging slot

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
