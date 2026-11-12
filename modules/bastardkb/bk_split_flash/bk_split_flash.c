// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Phase 0 walking skeleton: register one RPC transaction ID and round-trip a
// capability probe + echo between the halves via housekeeping, logging to console.
// No flash writes, no boot2 changes.
#include "debug.h"
#include "timer.h"
#include "transactions.h"
#include "community_modules.h"

#include "bk_split_flash.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 2);

// Slave side of the RPC: reflect the probe back as an echo.
static void bk_split_flash_slave_handler(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
    const bk_split_flash_probe_t *probe = (const bk_split_flash_probe_t *)in_data;
    bk_split_flash_echo_t        *echo  = (bk_split_flash_echo_t *)out_data;

    if (in_buflen < sizeof(*probe) || out_buflen < sizeof(*echo)) {
        print("bk_split_flash: slave probe buffer too small\r\n");
        return;
    }

    echo->magic   = probe->magic;
    echo->version = probe->version;
    echo->seq     = probe->seq;
    echo->status  = (probe->magic == BK_SPLIT_FLASH_MAGIC) ? BK_SPLIT_FLASH_STATUS_OK : 0xFF;
}

void keyboard_post_init_bk_split_flash(void) {
    transaction_register_rpc(PUT_BK_SPLIT_FLASH, bk_split_flash_slave_handler);
    xprintf("bk_split_flash: post_init on %s half, RPC registered\r\n", is_keyboard_master() ? "master" : "slave");
}

void housekeeping_task_bk_split_flash(void) {
    // Runs on both halves every main loop (caveat #5 evidence lives in the console).
    if (is_keyboard_master()) {
        static bool both_once = false;
        if (!both_once) {
            both_once = true;
            print("bk_split_flash: housekeeping on master half, probing\r\n");
        }

        static uint32_t last_probe = 0;
        if (timer_elapsed32(last_probe) > 500) {
            last_probe = timer_read32();

            bk_split_flash_probe_t probe = {.magic = BK_SPLIT_FLASH_MAGIC, .version = BK_SPLIT_FLASH_PROTOCOL_VERSION, .seq = (uint8_t)(last_probe & 0xFF)};
            bk_split_flash_echo_t echo  = {0};

            if (transaction_rpc_exec(PUT_BK_SPLIT_FLASH, sizeof(probe), &probe, sizeof(echo), &echo)) {
                xprintf("bk_split_flash: probe seq=%u -> echo seq=%u magic=0x%02x version=%u status=%u\r\n", probe.seq, echo.seq, echo.magic, echo.version, echo.status);
            } else {
                print("bk_split_flash: probe failed (secondary not connected?)\r\n");
            }
        }
    } else {
        static bool slave_once = false;
        if (!slave_once) {
            slave_once = true;
            print("bk_split_flash: housekeeping on slave half\r\n");
        }
    }
}
