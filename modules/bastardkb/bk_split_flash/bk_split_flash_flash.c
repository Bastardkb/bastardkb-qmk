// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bk_split_flash action: RP2040 staging / verify / commit behind the
// bk_split_flash.h HAL. This is the only hardware-bound file of the module;
// everything in bk_split_flash.c is transport/host-testable.
//
// Flash-layout invariants (see the implementation plan §4):
//   - active image: [0, __flash_binary_end - XIP_BASE)
//   - staging slot: [SPLIT_FLASH_STAGING_OFFSET, +SPLIT_FLASH_MAX_IMAGE_SIZE)
//   - emulated EEPROM (wear leveling): top of flash, never touched
// The ROM-fallback safety model: boot2 is written LAST inside the commit.
// Any reset before that write (i.e. any power cut during commit) leaves boot2
// invalid and the RP2040 bootrom lands in the recoverable BOOTSEL state.
#include <stddef.h>

#include "pico.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/regs/addressmap.h"

#include "bk_split_flash.h"

#ifndef XIP_BASE
#    error "bk_split_flash_flash.c requires the RP2040 platform (missing XIP_BASE)."
#endif
#if defined(PICO_NO_FLASH)
#    error "bk_split_flash_flash.c must not be built for PICO_NO_FLASH configurations."
#endif

// The writer programs whole 256-byte pages and erases whole 4 KiB sectors.
#define BK_FLASH_PAGE_SIZE FLASH_PAGE_SIZE
#define BK_FLASH_SECTOR_SIZE FLASH_SECTOR_SIZE
// boot2 occupies the first 256 bytes of the image; it is committed last.
#define BK_FLASH_BOOT2_SIZE 256u

// Keep the staging slot clear of the wear-leveling EEPROM region. Fall back to
// the stock 8192-byte backing if the wear-leveling config header is not on the
// include path (e.g. a board using a different EEPROM_DRIVER).
#if defined(PICO_FLASH_SIZE_BYTES)
#    if defined(__has_include)
#        if __has_include("wear_leveling_rp2040_flash_config.h")
#            include "wear_leveling_rp2040_flash_config.h"
#            define BK_SPLIT_FLASH_EEPROM_BASE WEAR_LEVELING_RP2040_FLASH_BASE
#        else
#            define BK_SPLIT_FLASH_EEPROM_BASE ((PICO_FLASH_SIZE_BYTES) - 8192u)
#        endif
#    else
#        define BK_SPLIT_FLASH_EEPROM_BASE ((PICO_FLASH_SIZE_BYTES) - 8192u)
#    endif
_Static_assert((uint32_t)SPLIT_FLASH_STAGING_OFFSET + (uint32_t)SPLIT_FLASH_MAX_IMAGE_SIZE <= (uint32_t)BK_SPLIT_FLASH_EEPROM_BASE, "bk_split_flash: staging slot collides with the wear-leveling EEPROM region.");
_Static_assert((uint32_t)SPLIT_FLASH_STAGING_OFFSET % (uint32_t)BK_FLASH_SECTOR_SIZE == 0, "bk_split_flash: SPLIT_FLASH_STAGING_OFFSET must be sector aligned.");
_Static_assert((uint32_t)SPLIT_FLASH_MAX_IMAGE_SIZE % (uint32_t)BK_FLASH_SECTOR_SIZE == 0, "bk_split_flash: SPLIT_FLASH_MAX_IMAGE_SIZE must be a multiple of the flash sector size.");
#else
#    error "bk_split_flash_flash.c requires the RP2040 platform (missing PICO_FLASH_SIZE_BYTES)."
#endif

extern char __flash_binary_end;

// The one RAM buffer shared by the staging writer and the commit. Also used as
// the source buffer for flash_range_program (data must be in RAM while XIP is
// temporarily down). Must stay RAM-resident (.bss).
static uint8_t bk_flash_page_buf[BK_FLASH_PAGE_SIZE];

// Staging writer state (sequential append only).
static bool     bk_stage_active;
static uint32_t bk_stage_cursor;  // staging offset the next byte goes to
static uint32_t bk_stage_buf_len; // valid bytes currently held in bk_flash_page_buf
static uint32_t bk_stage_buf_off; // staging offset of bk_flash_page_buf[0]

#if defined(BK_SPLIT_FLASH_PROGRESS_LED)
// Direct SIO writes so the commit can signal progress with interrupts and XIP
// disabled. The pin is configured as a plain SIO output in keyboard_post_init.
#    define BK_FLASH_LED_OFF() (sio_hw->gpio_clr = (1u << (BK_SPLIT_FLASH_PROGRESS_LED)))
#    define BK_FLASH_LED_ON()  (sio_hw->gpio_set = (1u << (BK_SPLIT_FLASH_PROGRESS_LED)))
#else
#    define BK_FLASH_LED_OFF() ((void)0)
#    define BK_FLASH_LED_ON()  ((void)0)
#endif

uint32_t bk_split_flash_active_image_len(void) {
    return (uint32_t)((const char *)&__flash_binary_end - (const char *)XIP_BASE);
}

static uint8_t bk_split_flash_flash_program_page(uint32_t flash_offset) {
    // flash_range_program requires interrupts disabled and a RAM source buffer.
    uint32_t saved_irq = save_and_disable_interrupts();
    flash_range_program(flash_offset, bk_flash_page_buf, BK_FLASH_PAGE_SIZE);
    restore_interrupts(saved_irq);
    return BK_SPLIT_FLASH_STATUS_OK;
}

uint8_t bk_split_flash_stage_begin(void) {
    uint32_t saved_irq = save_and_disable_interrupts();
    flash_range_erase(SPLIT_FLASH_STAGING_OFFSET, SPLIT_FLASH_MAX_IMAGE_SIZE);
    restore_interrupts(saved_irq);
    bk_stage_active  = true;
    bk_stage_cursor  = 0;
    bk_stage_buf_len = 0;
    bk_stage_buf_off = 0;
    return BK_SPLIT_FLASH_STATUS_OK;
}

uint8_t bk_split_flash_stage_bytes(uint32_t offset, const void *data, uint32_t len) {
    if (!bk_stage_active) {
        return BK_SPLIT_FLASH_STATUS_ERR_RANGE;
    }
    if (offset != bk_stage_cursor) {
        return BK_SPLIT_FLASH_STATUS_ERR_RANGE;
    }
    uint32_t end = offset + len;
    if (end > (uint32_t)SPLIT_FLASH_MAX_IMAGE_SIZE) {
        return BK_SPLIT_FLASH_STATUS_ERR_SIZE;
    }
    const uint8_t *src = (const uint8_t *)data;
    while (len > 0) {
        if (bk_stage_buf_len == 0) {
            bk_stage_buf_off = bk_stage_cursor;
        }
        uint32_t room = BK_FLASH_PAGE_SIZE - bk_stage_buf_len;
        uint32_t n    = len < room ? len : room;
        for (uint32_t i = 0; i < n; ++i) {
            bk_flash_page_buf[bk_stage_buf_len + i] = src[i];
        }
        bk_stage_buf_len += n;
        bk_stage_cursor += n;
        src += n;
        len -= n;
        if (bk_stage_buf_len == BK_FLASH_PAGE_SIZE) {
            bk_split_flash_flash_program_page(SPLIT_FLASH_STAGING_OFFSET + bk_stage_buf_off);
            bk_stage_buf_len = 0;
        }
    }
    return BK_SPLIT_FLASH_STATUS_OK;
}

uint8_t bk_split_flash_stage_end(void) {
    if (!bk_stage_active) {
        return BK_SPLIT_FLASH_STATUS_ERR_RANGE;
    }
    if (bk_stage_buf_len > 0) {
        // Pad the partial page to erased-flash state (0xFF) and commit it.
        for (uint32_t i = bk_stage_buf_len; i < BK_FLASH_PAGE_SIZE; ++i) {
            bk_flash_page_buf[i] = 0xFF;
        }
        bk_split_flash_flash_program_page(SPLIT_FLASH_STAGING_OFFSET + bk_stage_buf_off);
        bk_stage_buf_len = 0;
    }
    bk_stage_active = false;
    return BK_SPLIT_FLASH_STATUS_OK;
}

uint8_t bk_split_flash_staged_verify(uint32_t expected_len, uint32_t expected_crc) {
    if (!bk_split_flash_len_in_range(expected_len)) {
        return BK_SPLIT_FLASH_STATUS_ERR_SIZE;
    }
    const uint8_t *stage = (const uint8_t *)(XIP_BASE + SPLIT_FLASH_STAGING_OFFSET);
    if (!bk_split_flash_image_plausible(stage, expected_len)) {
        return BK_SPLIT_FLASH_STATUS_ERR_MAGIC;
    }
    if (bk_split_flash_crc32(stage, expected_len) != expected_crc) {
        return BK_SPLIT_FLASH_STATUS_ERR_CRC;
    }
    return BK_SPLIT_FLASH_STATUS_OK;
}

// RAM-resident, interrupts-off commit. From the first sector-0 erase until
// boot2 lands last, any reset leaves the chip in ROM BOOTSEL. This function
// must not call anything flash-resident (the active region it erases holds the
// running image): no print, no memcpy, no callback — only pico-sdk's own
// RAM-resident flash primitives and inline register writes.
uint8_t __no_inline_not_in_flash_func(bk_split_flash_commit)(uint32_t image_len) {
    if (!bk_split_flash_len_in_range(image_len)) {
        return BK_SPLIT_FLASH_STATUS_ERR_SIZE;
    }
    uint32_t pages       = (image_len + BK_FLASH_PAGE_SIZE - 1) / BK_FLASH_PAGE_SIZE;
    uint32_t active_bytes = pages * BK_FLASH_PAGE_SIZE;
    if (active_bytes > (uint32_t)SPLIT_FLASH_MAX_IMAGE_SIZE || active_bytes > (uint32_t)SPLIT_FLASH_STAGING_OFFSET) {
        return BK_SPLIT_FLASH_STATUS_ERR_RANGE;
    }
    uint32_t erase_bytes = (image_len + BK_FLASH_SECTOR_SIZE - 1) & ~((uint32_t)BK_FLASH_SECTOR_SIZE - 1u);
    const uint8_t *stage = (const uint8_t *)(XIP_BASE + SPLIT_FLASH_STAGING_OFFSET);

    BK_FLASH_LED_OFF(); // commit in progress

    uint32_t saved_irq = save_and_disable_interrupts();

    // ARM + erase the active region (sector 0 including boot2 is erased first).
    flash_range_erase(0, erase_bytes);

    // Copy staging -> active, page by page, boot2 (page 0) written last.
    for (uint32_t off = BK_FLASH_BOOT2_SIZE; off < active_bytes; off += BK_FLASH_PAGE_SIZE) {
        for (uint32_t i = 0; i < BK_FLASH_PAGE_SIZE; ++i) {
            bk_flash_page_buf[i] = stage[off + i];
        }
        flash_range_program(off, bk_flash_page_buf, BK_FLASH_PAGE_SIZE);
    }
    for (uint32_t i = 0; i < BK_FLASH_BOOT2_SIZE; ++i) {
        bk_flash_page_buf[i] = stage[i];
    }
    flash_range_program(0, bk_flash_page_buf, BK_FLASH_PAGE_SIZE);

    restore_interrupts(saved_irq);

    BK_FLASH_LED_ON(); // boot2 valid: the image is in place
    return BK_SPLIT_FLASH_STATUS_OK;
}

// Progress LED init (call from keyboard_post_init, both halves).
void bk_split_flash_progress_led_init(void) {
#if defined(BK_SPLIT_FLASH_PROGRESS_LED)
    gpio_set_function(BK_SPLIT_FLASH_PROGRESS_LED, GPIO_FUNC_SIO);
    gpio_set_dir(BK_SPLIT_FLASH_PROGRESS_LED, GPIO_OUT);
    gpio_put(BK_SPLIT_FLASH_PROGRESS_LED, 0);
#endif
}