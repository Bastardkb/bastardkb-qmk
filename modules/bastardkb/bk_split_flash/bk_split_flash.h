// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

// Step 1 round-trip: one full-size master->slave chunk and a small slave->master
// ack. The split protocol takes both lengths from the transaction table, not the
// wire, so they are fixed at compile time.
#define BK_SPLIT_FLASH_M2S_SIZE 240
#define BK_SPLIT_FLASH_S2M_SIZE 4

typedef struct bk_split_flash_m2s_t {
    uint32_t seq;
    uint8_t  data[BK_SPLIT_FLASH_M2S_SIZE - sizeof(uint32_t)];
} bk_split_flash_m2s_t;

typedef struct bk_split_flash_s2m_t {
    uint32_t ack;
} bk_split_flash_s2m_t;
