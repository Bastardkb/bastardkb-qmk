// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

// Step 2: stream the master's whole firmware image to the secondary in fixed-size
// chunks and check transport integrity end to end. The slave accumulates a rolling
// CRC-32 over the bytes it receives and reports it back at end of stream; the
// master compares it with the CRC-32 of the bytes it sent. Neither half keeps a copy
// of the image and no flash is written.
//
// The split protocol takes the byte counts from the transaction table, not the wire,
// so the sizes are fixed at compile time (unchanged from step 1's proven 240/4).
#define BK_SPLIT_FLASH_M2S_SIZE 240
#define BK_SPLIT_FLASH_S2M_SIZE 4

// Payload bytes per chunk: m2s minus the {seq, offset} words and the crc8 byte.
#define BK_SPLIT_FLASH_CHUNK_DATA (BK_SPLIT_FLASH_M2S_SIZE - 2 * sizeof(uint32_t) - sizeof(uint8_t))

// Reserved `seq` values; data chunks are numbered 1..N.
#define BK_SPLIT_FLASH_SEQ_BEGIN 0u           // offset carries the image length; (re)sets the slave state
#define BK_SPLIT_FLASH_SEQ_REPORT 0xFFFFFFFFu // slave answers with its finalized CRC-32

// Slave acknowledgements (s2m.ack). An accepted data chunk is acked with its own seq
// (1..N), so these sentinels cannot collide with a valid ack.
#define BK_SPLIT_FLASH_ACK_BEGIN 0xFFFFFFFFu    // begin / state reset accepted
#define BK_SPLIT_FLASH_ACK_SEQ_ERR 0u           // out-of-order chunk, or no stream active
#define BK_SPLIT_FLASH_ACK_CRC8_ERR 0xFFFFFFFEu // chunk CRC-8 mismatch

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
