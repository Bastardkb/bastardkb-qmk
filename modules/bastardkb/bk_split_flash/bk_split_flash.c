// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bk_split_flash trigger + transport side: Phase 0 capability probe/echo over
// one RPC transaction ID, plus the TEMPORARY Phase 1 on-device test hook that
// drives the flash action HAL through a self-clone (stage own image -> verify
// -> arm+commit -> reboot) so the safety core can be validated with one UF2.
#include "debug.h"
#include "timer.h"
#include "transactions.h"
#include "community_modules.h"
#include "matrix.h"
#include "wait.h"
#include "bootloader.h"
#include "split_util.h"
#include "gpio.h"
#include "hardware/regs/addressmap.h"

#include "bk_split_flash.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 2);

// ---------------------------------------------------------------------------
// Phase 0 walking skeleton: probe + echo over the module's RPC transaction.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// TEMPORARY Phase 1 on-device test hook (removed in Phase 2).
// ---------------------------------------------------------------------------
#if BK_SPLIT_FLASH_TEST_ENABLE

#    if defined(BK_SPLIT_FLASH_PROGRESS_LED)
#        define BK_FLASH_TEST_LED_ON()     gpio_write_pin_high(BK_SPLIT_FLASH_PROGRESS_LED)
#        define BK_FLASH_TEST_LED_OFF()    gpio_write_pin_low(BK_SPLIT_FLASH_PROGRESS_LED)
#        define BK_FLASH_TEST_LED_TOGGLE() gpio_toggle_pin(BK_SPLIT_FLASH_PROGRESS_LED)
#    else
#        define BK_FLASH_TEST_LED_ON()     ((void)0)
#        define BK_FLASH_TEST_LED_OFF()    ((void)0)
#        define BK_FLASH_TEST_LED_TOGGLE() ((void)0)
#    endif

static void bk_split_flash_test_fail(const char *which) {
    // Indefinite blink on failure so the state is obvious in a power-cut test.
    xprintf("bk_split_flash: TEST FAIL at %s\r\n", which);
    for (;;) {
        BK_FLASH_TEST_LED_ON();
        wait_ms(80);
        BK_FLASH_TEST_LED_OFF();
        wait_ms(80);
    }
}

static void bk_split_flash_test_run(void) {
    uint32_t len = bk_split_flash_active_image_len();
    if (!bk_split_flash_len_in_range(len)) {
        xprintf("bk_split_flash: TEST abort, bad image len=%lu\r\n", (unsigned long)len);
        return;
    }
    uint32_t crc = bk_split_flash_crc32((const uint8_t *)XIP_BASE, len);
    xprintf("bk_split_flash: TEST start len=%lu crc=0x%08lx\r\n", (unsigned long)len, (unsigned long)crc);

    // Phase: solid LED = armed, wait for the "cut before arm" case.
    BK_FLASH_TEST_LED_ON();
    wait_ms(BK_SPLIT_FLASH_TEST_START_DELAY_MS);

    uint8_t st = bk_split_flash_stage_begin();
    if (st != BK_SPLIT_FLASH_STATUS_OK) {
        bk_split_flash_test_fail("stage_begin");
    }

    // Phase: blink = staging. Cut power here -> old image intact, re-clone later.
    uint32_t      off         = 0;
    uint32_t      last_toggle = 0;
    while (off < len) {
        uint32_t n = len - off;
        if (n > 256u) {
            n = 256u;
        }
        st = bk_split_flash_stage_bytes(off, (const uint8_t *)XIP_BASE + off, n);
        if (st != BK_SPLIT_FLASH_STATUS_OK) {
            bk_split_flash_test_fail("stage_bytes");
        }
        off += n;
        if (timer_elapsed32(last_toggle) > 100) {
            last_toggle = timer_read32();
            BK_FLASH_TEST_LED_TOGGLE();
        }
    }
    st = bk_split_flash_stage_end();
    if (st != BK_SPLIT_FLASH_STATUS_OK) {
        bk_split_flash_test_fail("stage_end");
    }

    st = bk_split_flash_staged_verify(len, crc);
    if (st != BK_SPLIT_FLASH_STATUS_OK) {
        bk_split_flash_test_fail("staged_verify");
    }
    xprintf("bk_split_flash: TEST staged+verified\r\n");

    // Phase: solid LED again = verified, pre-commit pause. Cut here -> old boots.
    BK_FLASH_TEST_LED_ON();
    wait_ms(BK_SPLIT_FLASH_TEST_COMMIT_DELAY_MS);

    // Phase: LED goes OFF inside the commit and stays off until boot2 lands.
    // Cut while off -> ROM BOOTSEL. The LED is toggled by the RAM-resident
    // commit writer itself, so the window covers erase + copy + boot2.
    st = bk_split_flash_commit(len);
    if (st != BK_SPLIT_FLASH_STATUS_OK) {
        bk_split_flash_test_fail("commit");
    }

    // Phase: LED on = boot2 written. Cut here -> new (identical) image boots.
    wait_ms(BK_SPLIT_FLASH_TEST_REBOOT_DELAY_MS);
    xprintf("bk_split_flash: TEST commit ok, rebooting\r\n");
    mcu_reset();
}

// Returns true the first time the trigger key has been held long enough.
static bool bk_split_flash_test_trigger_passed(void) {
    static bool      done       = false;
    static bool      hold_seen  = false;
    static uint32_t  hold_start = 0;
    if (done) {
        return false;
    }
    bool held = matrix_is_on(BK_SPLIT_FLASH_TEST_KEY_ROW, BK_SPLIT_FLASH_TEST_KEY_COL);
    if (held && !hold_seen) {
        hold_seen  = true;
        hold_start = timer_read32();
    } else if (!held && hold_seen) {
        hold_seen = false;
    }
    if (hold_seen && timer_elapsed32(hold_start) > BK_SPLIT_FLASH_TEST_HOLD_MS) {
        done = true;
        return true;
    }
    return false;
}

#endif // BK_SPLIT_FLASH_TEST_ENABLE

void keyboard_post_init_bk_split_flash(void) {
    transaction_register_rpc(PUT_BK_SPLIT_FLASH, bk_split_flash_slave_handler);
#if BK_SPLIT_FLASH_TEST_ENABLE
    bk_split_flash_progress_led_init();
#endif
    xprintf("bk_split_flash: post_init on %s half, RPC registered\r\n", is_keyboard_master() ? "master" : "slave");
}

void housekeeping_task_bk_split_flash(void) {
    // Runs on both halves every main loop.
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
#if BK_SPLIT_FLASH_TEST_ENABLE
        if (bk_split_flash_test_trigger_passed()) {
            bk_split_flash_test_run();
        }
#endif
    }
}