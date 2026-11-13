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
// TEMPORARY Phase 1 on-device test hook (removed in Phase 2).
//
// Non-blocking state machine: each housekeeping tick does at most one flash
// page, so the slave's main loop (and therefore the split transport) keeps
// running. Progress is reported to the master through the probe echo status,
// which makes the run observable on the primary's qmk console.
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

// Echo status values while the test hook runs (0 = idle, matches Phase 0).
enum {
    BK_TEST_IDLE     = 0,
    BK_TEST_ARMED    = 1,  // triggered; armed/pre-arm wait
    BK_TEST_STAGING  = 2,  // writing pages into the staging slot
    BK_TEST_VERIFIED = 3,  // verify passed; pre-commit pause
    BK_TEST_COMMIT   = 4,  // commit about to run / running
    BK_TEST_DONE     = 5,  // boot2 written; pre-reboot pause
    BK_TEST_FAIL     = 0xF0, // OR'd with the failing stage (1..5)
};

static uint8_t  bk_test_state    = BK_TEST_IDLE;
static uint8_t  bk_test_fail_stage;
static uint32_t bk_test_phase_start;
static uint32_t bk_test_len;
static uint32_t bk_test_crc;
static uint32_t bk_test_cursor;
static uint32_t bk_test_last_toggle;

static void bk_split_flash_test_fail_stage(uint8_t stage) {
    bk_test_fail_stage = stage;
    bk_test_state      = BK_TEST_FAIL;
    bk_test_phase_start = timer_read32();
    xprintf("bk_split_flash: TEST FAIL stage=%u\r\n", (unsigned)stage);
}

static void bk_split_flash_test_tick(void) {
    switch (bk_test_state) {
        case BK_TEST_IDLE: {
            // Hold the trigger key continuously for HOLD_MS to start a run.
            static bool     hold_seen  = false;
            static uint32_t hold_start = 0;
            bool held = matrix_is_on(BK_SPLIT_FLASH_TEST_KEY_ROW, BK_SPLIT_FLASH_TEST_KEY_COL);
            if (held && !hold_seen) {
                hold_seen  = true;
                hold_start = timer_read32();
            } else if (!held && hold_seen) {
                hold_seen = false;
            } else if (hold_seen && timer_elapsed32(hold_start) > BK_SPLIT_FLASH_TEST_HOLD_MS) {
                hold_seen        = false;
                bk_test_len      = bk_split_flash_active_image_len();
                if (!bk_split_flash_len_in_range(bk_test_len)) {
                    bk_split_flash_test_fail_stage(0);
                    break;
                }
                bk_test_crc = bk_split_flash_crc32((const uint8_t *)XIP_BASE, bk_test_len);
                xprintf("bk_split_flash: TEST start len=%lu crc=0x%08lx\r\n", (unsigned long)bk_test_len, (unsigned long)bk_test_crc);
                // Brief triggered burst (~1 s, the only blocking part).
                for (uint8_t i = 0; i < 4; ++i) {
                    BK_FLASH_TEST_LED_ON();
                    wait_ms(120);
                    BK_FLASH_TEST_LED_OFF();
                    wait_ms(120);
                }
                BK_FLASH_TEST_LED_ON();
                bk_test_phase_start = timer_read32();
                bk_test_state       = BK_TEST_ARMED;
            }
            break;
        }

        case BK_TEST_ARMED:
            // Solid LED, non-blocking armed wait. Cut power here = nothing touched.
            if (timer_elapsed32(bk_test_phase_start) > BK_SPLIT_FLASH_TEST_START_DELAY_MS) {
                uint8_t st = bk_split_flash_stage_begin();
                if (st != BK_SPLIT_FLASH_STATUS_OK) {
                    bk_split_flash_test_fail_stage(1);
                    break;
                }
                bk_test_cursor = 0;
                bk_test_state = BK_TEST_STAGING;
                xprintf("bk_split_flash: TEST staging\r\n");
            }
            break;

        case BK_TEST_STAGING: {
            uint32_t n = bk_test_len - bk_test_cursor;
            if (n > 256u) {
                n = 256u;
            }
            if (n > 0) {
                // One page per tick keeps the main loop (and transport) alive.
                uint8_t st = bk_split_flash_stage_bytes(bk_test_cursor, (const uint8_t *)XIP_BASE + bk_test_cursor, n);
                if (st != BK_SPLIT_FLASH_STATUS_OK) {
                    bk_split_flash_test_fail_stage(2);
                    break;
                }
                bk_test_cursor += n;
                if (timer_elapsed32(bk_test_last_toggle) > 100) {
                    bk_test_last_toggle = timer_read32();
                    BK_FLASH_TEST_LED_TOGGLE();
                }
                break;
            }
            uint8_t st = bk_split_flash_stage_end();
            if (st != BK_SPLIT_FLASH_STATUS_OK) {
                bk_split_flash_test_fail_stage(3);
                break;
            }
            st = bk_split_flash_staged_verify(bk_test_len, bk_test_crc);
            if (st != BK_SPLIT_FLASH_STATUS_OK) {
                bk_split_flash_test_fail_stage(4);
                break;
            }
            xprintf("bk_split_flash: TEST staged+verified\r\n");
            BK_FLASH_TEST_LED_ON();
            bk_test_phase_start = timer_read32();
            bk_test_state       = BK_TEST_VERIFIED;
            break;
        }

        case BK_TEST_VERIFIED:
            // Solid LED, non-blocking pre-commit pause. Cut here = old image boots.
            if (timer_elapsed32(bk_test_phase_start) > BK_SPLIT_FLASH_TEST_COMMIT_DELAY_MS) {
                bk_test_state = BK_TEST_COMMIT;
            }
            break;

        case BK_TEST_COMMIT: {
            // One bounded RAM-resident section. Interrupts are off inside it, so
            // probes briefly fail; cutting power during that blackout = BOOTSEL.
            uint8_t st = bk_split_flash_commit(bk_test_len);
            if (st != BK_SPLIT_FLASH_STATUS_OK) {
                bk_split_flash_test_fail_stage(5);
                break;
            }
            xprintf("bk_split_flash: TEST commit ok\r\n");
            bk_test_phase_start = timer_read32();
            bk_test_state       = BK_TEST_DONE;
            break;
        }

        case BK_TEST_DONE:
            // LED on (boot2 valid). Cut here = new (identical) image boots.
            if (timer_elapsed32(bk_test_phase_start) > BK_SPLIT_FLASH_TEST_REBOOT_DELAY_MS) {
                xprintf("bk_split_flash: TEST rebooting\r\n");
                mcu_reset();
            }
            break;

        case BK_TEST_FAIL:
        default: {
            // Non-blocking stage-encoded pulses: N slow blinks, then a long pause.
            uint32_t period = (uint32_t)bk_test_fail_stage * 300u + 1500u;
            uint32_t t      = timer_elapsed32(bk_test_phase_start) % period;
            bool     on     = false;
            for (uint8_t k = 0; k < bk_test_fail_stage; ++k) {
                if (t >= (uint32_t)k * 300u && t < (uint32_t)k * 300u + 150u) {
                    on = true;
                }
            }
            if (on) {
                BK_FLASH_TEST_LED_ON();
            } else {
                BK_FLASH_TEST_LED_OFF();
            }
            break;
        }
    }
}

static uint8_t bk_split_flash_test_status(void) {
    if (bk_test_state == BK_TEST_FAIL) {
        return (uint8_t)(BK_TEST_FAIL | bk_test_fail_stage);
    }
    return bk_test_state;
}

#endif // BK_SPLIT_FLASH_TEST_ENABLE

// ---------------------------------------------------------------------------
// Phase 0 walking skeleton: probe + echo over the module's RPC transaction.
// ---------------------------------------------------------------------------

// Slave side of the RPC: reflect the probe back as an echo. While the Phase 1
// test hook runs, `status` additionally reports the hook's state (0 = idle).
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
#if BK_SPLIT_FLASH_TEST_ENABLE
    echo->status = (probe->magic == BK_SPLIT_FLASH_MAGIC) ? bk_split_flash_test_status() : 0xFF;
#else
    echo->status = (probe->magic == BK_SPLIT_FLASH_MAGIC) ? BK_SPLIT_FLASH_STATUS_OK : 0xFF;
#endif
}

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
        bk_split_flash_test_tick();
#endif
    }
}