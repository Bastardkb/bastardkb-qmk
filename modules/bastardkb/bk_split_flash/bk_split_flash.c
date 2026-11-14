// SPDX-License-Identifier: GPL-2.0-or-later
#include <stddef.h>
#include <string.h>

#include "debug.h"
#include "timer.h"
#include "keyboard.h"
#include "transactions.h"
#include "crc.h"
#include "community_modules.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/m0plus.h"
#include "wear_leveling/wear_leveling_rp2040_flash_config.h"

#include "bk_split_flash.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 2);

// Transaction id value from the generated serial_transaction_id enum.
#define BK_SPLIT_FLASH_TRANSACTION PUT_BK_SPLIT_FLASH

// The image streamed to the secondary: [XIP_BASE, __flash_binary_end). In
// RP2040_FLASH_TIMECRIT.ld `__flash_binary_start` is flash1's origin (XIP_BASE,
// 0x10000000) and `__flash_binary_end` the end of the load region.
extern const uint8_t __flash_binary_start[];
extern const uint8_t __flash_binary_end[];

STATIC_ASSERT(sizeof(bk_split_flash_m2s_t) == BK_SPLIT_FLASH_M2S_SIZE, "m2s chunk must be full-size");
STATIC_ASSERT(sizeof(bk_split_flash_s2m_t) == BK_SPLIT_FLASH_S2M_SIZE, "s2m ack size mismatch");
// Keep the staging slot clear of the wear-leveling EEPROM region (base 0x1FE000
// on this board; see the knowledge base's flash-size facts).
STATIC_ASSERT(BK_SPLIT_FLASH_STAGING_OFFSET + BK_SPLIT_FLASH_STAGING_MAX <= WEAR_LEVELING_RP2040_FLASH_BASE, "staging overlaps the wear-leveling EEPROM");
// The commit erases the whole wear-leveling region, so it must be sector-aligned.
STATIC_ASSERT((WEAR_LEVELING_RP2040_FLASH_BASE % FLASH_SECTOR_SIZE) == 0 && (WEAR_LEVELING_BACKING_SIZE % FLASH_SECTOR_SIZE) == 0, "wear-leveling region not sector-aligned");
// BEGIN rejects an image larger than the erased staging slot, so the active
// region a commit erases is at most STAGING_MAX and can never reach staging.
STATIC_ASSERT(BK_SPLIT_FLASH_STAGING_OFFSET >= BK_SPLIT_FLASH_STAGING_MAX, "active region could overlap staging");

// Reflected CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF). QMK's
// quantum/crc.c only offers a whole-buffer CRC-8, so both halves use this table
// driven CRC-32: the master over the bytes it sends, and the slave over the bytes
// it receives, over the staging read-back (folded in one 256 B page at a time, so
// no callback ever reads the whole ~90 KiB slot), and over the active image for
// the re-clone guard.
static uint32_t bk_crc32_table[256];
static bool     bk_crc32_table_ready;

static void bk_crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (uint8_t bit = 0; bit < 8; bit++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        bk_crc32_table[i] = c;
    }
    bk_crc32_table_ready = true;
}

static uint32_t bk_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    if (!bk_crc32_table_ready) {
        bk_crc32_build_table();
    }
    for (size_t i = 0; i < len; i++) {
        crc = bk_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

// Final XOR-out of the reflected CRC-32. Both halves keep the raw accumulator
// (init 0xFFFFFFFF, no final xor) while streaming and apply this once at the end.
static inline uint32_t bk_crc32_finalize(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

// The RP2040 bootrom checks boot2 with a bit-reversed CRC-32 (see the knowledge
// base's boot2-checksum facts): feed each byte bit-reversed into the same
// reflected CRC-32, then bit-reverse the raw result. Verified against a real
// built image before implementing.
static inline uint8_t bk_bitrev8(uint8_t value) {
    value = (uint8_t)(((value & 0xF0u) >> 4) | ((value & 0x0Fu) << 4));
    value = (uint8_t)(((value & 0xCCu) >> 2) | ((value & 0x33u) << 2));
    value = (uint8_t)(((value & 0xAAu) >> 1) | ((value & 0x55u) << 1));
    return value;
}

static inline uint32_t bk_bitrev32(uint32_t value) {
    value = ((value & 0xFFFF0000u) >> 16) | ((value & 0x0000FFFFu) << 16);
    value = ((value & 0xFF00FF00u) >> 8) | ((value & 0x00FF00FFu) << 8);
    value = ((value & 0xF0F0F0F0u) >> 4) | ((value & 0x0F0F0F0Fu) << 4);
    value = ((value & 0xCCCCCCCCu) >> 2) | ((value & 0x33333333u) << 2);
    value = ((value & 0xAAAAAAAAu) >> 1) | ((value & 0x55555555u) << 1);
    return value;
}

// Read a little-endian word from flash without assuming aligned access.
static inline uint32_t bk_read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t bk_crc32_update_reversed(uint32_t crc, const uint8_t *data, uint32_t len) {
    if (!bk_crc32_table_ready) {
        bk_crc32_build_table();
    }
    for (uint32_t i = 0; i < len; i++) {
        crc = bk_crc32_table[(crc ^ bk_bitrev8(data[i])) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

// The RP2040 bootrom checksum for a boot2 blob (the first 256 bytes of an image):
// the bit-reversed raw CRC-32 over the first 252 bytes, compared against the
// little-endian word at offset 252.
static uint32_t bk_boot2_checksum(const uint8_t *boot2) {
    return bk_bitrev32(bk_crc32_update_reversed(0xFFFFFFFFu, boot2, 252u));
}

// --- Staging slot -------------------------------------------------------------

// Staging is programmed one 256-byte flash page at a time. Chunks arrive 231
// bytes wide, so received bytes are buffered until a page is complete; only a
// single page lives in RAM at once. `bk_staged` counts bytes appended so far, so
// the page being filled always starts at the 256-aligned offset
// `bk_staged - bk_page_len`.
static uint8_t  bk_page[FLASH_PAGE_SIZE];
static uint32_t bk_page_len;
static uint32_t bk_staged;
static uint32_t bk_readback_crc; // running CRC-32 of the bytes read back from staging
static uint32_t bk_active_crc;   // running CRC-32 of the bytes currently in the active region

// Program one 256-byte page, then read it back and fold its `valid_len` data
// bytes into the read-back CRC. Verifying a page at a time (rather than reading
// the whole ~90 KiB slot in one callback) keeps each read-back cheap enough to run
// during the stream, so the final VERIFY is O(1) and cannot blow the 20 ms
// transport read timeout.
//
// The same valid bytes of the *active* image are folded into a second CRC. Both
// are accumulated page by page, so the re-clone guard (staged == active) is also
// O(1) at commit time instead of a whole-region read in one callback.
static void bk_page_program(uint32_t staging_offset, uint32_t valid_len) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(BK_SPLIT_FLASH_STAGING_OFFSET + staging_offset, bk_page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    const uint8_t *readback = (const uint8_t *)(XIP_BASE + BK_SPLIT_FLASH_STAGING_OFFSET + staging_offset);
    bk_readback_crc         = bk_crc32_update(bk_readback_crc, readback, valid_len);

    const uint8_t *active = (const uint8_t *)(XIP_BASE + staging_offset);
    bk_active_crc         = bk_crc32_update(bk_active_crc, active, valid_len);
}

static void bk_stage_flush_full_page(void) {
    bk_page_program(bk_staged - FLASH_PAGE_SIZE, FLASH_PAGE_SIZE);
    bk_page_len = 0;
}

static void bk_stage_flush_partial_page(void) {
    if (bk_page_len == 0) {
        return;
    }
    uint32_t valid = bk_page_len; // data bytes in the last, short page
    // Pad the final short page with erased-flash (0xFF) fill.
    memset(bk_page + bk_page_len, 0xFF, FLASH_PAGE_SIZE - bk_page_len);
    bk_page_program(bk_staged - bk_page_len, valid);
    bk_page_len = 0;
}

static void bk_stage_append(const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        bk_page[bk_page_len++] = data[i];
        bk_staged++;
        if (bk_page_len == FLASH_PAGE_SIZE) {
            bk_stage_flush_full_page();
        }
    }
}

// --- Slave --------------------------------------------------------------------

// Runs in the slave's split protocol thread. The passed lengths are ignored (the
// core passes the m2s size for the out size too, so they are not trustworthy
// here); sizes are fixed in the transaction table. Only the rolling CRC-32, the
// page-assembly buffer, and a few counters are kept -- never a whole-image copy.
typedef struct {
    bool     active;
    uint32_t total;
    uint32_t next_seq;
    uint32_t crc;
} bk_slave_stream_t;

static bk_slave_stream_t bk_slave;

// Bytes erased at post-init (0 = staging unavailable; begin is then rejected).
static uint32_t bk_staging_erased;

// Commit scheduling. The COMMIT callback runs in the split thread, so it only
// records the request; the slave's housekeeping task performs the long,
// interrupts-off commit (doing it in the callback would freeze the link).
static volatile bool     bk_commit_pending;
static volatile bool     bk_commit_timed;
static volatile uint32_t bk_commit_at_ms;
static volatile uint32_t bk_commit_len;

// Bootability gate, run in the COMMIT callback before anything is erased: the
// staged image must carry a valid RP2040 boot2 checksum and a plausible ARM
// vector table (initial SP in SRAM, odd reset vector inside the image).
static bool bk_image_bootable(uint32_t image_len) {
    if (image_len < 0x108u) {
        return false;
    }

    const uint8_t *image = (const uint8_t *)(XIP_BASE + BK_SPLIT_FLASH_STAGING_OFFSET);

    if (bk_boot2_checksum(image) != bk_read_u32(image + 252u)) {
        return false;
    }

    // SRAM is [0x20000000, 0x20042000); the initial stack pointer must be a
    // word-aligned address inside it.
    uint32_t initial_sp = bk_read_u32(image + 0x100u);
    if (initial_sp < 0x20000000u || initial_sp > 0x20042000u || (initial_sp & 0x3u) != 0) {
        return false;
    }

    // The reset vector must be an odd (Thumb) address inside the staged image.
    uint32_t reset_vector = bk_read_u32(image + 0x104u);
    if (reset_vector < XIP_BASE + 0x108u || reset_vector >= XIP_BASE + image_len || (reset_vector & 0x1u) == 0) {
        return false;
    }

    return true;
}

static void bk_split_flash_slave_callback(uint8_t initiator2target_buffer_size, const void *initiator2target_buffer, uint8_t target2initiator_buffer_size, void *target2initiator_buffer) {
    (void)initiator2target_buffer_size;
    (void)target2initiator_buffer_size;

    const bk_split_flash_m2s_t *m2s = (const bk_split_flash_m2s_t *)initiator2target_buffer;
    bk_split_flash_s2m_t       *s2m = (bk_split_flash_s2m_t *)target2initiator_buffer;

    if (m2s->seq == BK_SPLIT_FLASH_SEQ_BEGIN) {
        // (Re)initialise the stream even when the request is rejected, so no state
        // from an earlier run lingers. A pending commit is cancelled by a new run.
        bk_slave.active   = false;
        bk_slave.total    = 0;
        bk_slave.next_seq = 0;
        bk_slave.crc      = 0xFFFFFFFFu;
        bk_staged         = 0;
        bk_page_len       = 0;
        bk_readback_crc   = 0xFFFFFFFFu;
        bk_active_crc     = 0xFFFFFFFFu;
        bk_commit_pending = false;
        bk_commit_timed   = false;
        bk_commit_len     = 0;

        if (bk_staging_erased == 0 || m2s->offset == 0 || m2s->offset > bk_staging_erased) {
            s2m->ack = BK_SPLIT_FLASH_ACK_LEN_ERR;
            return;
        }
        bk_slave.active   = true;
        bk_slave.total    = m2s->offset;
        bk_slave.next_seq = 1;
        s2m->ack          = BK_SPLIT_FLASH_ACK_BEGIN;
        return;
    }

    if (m2s->seq == BK_SPLIT_FLASH_SEQ_REPORT) {
        s2m->ack = bk_crc32_finalize(bk_slave.crc); // rolling CRC-32 over the received bytes
        return;
    }

    if (m2s->seq == BK_SPLIT_FLASH_SEQ_VERIFY) {
        s2m->ack = bk_crc32_finalize(bk_readback_crc); // CRC-32 of what the staging read-back returned
        return;
    }

    if (m2s->seq == BK_SPLIT_FLASH_SEQ_COMMIT) {
        // A completed stream whose staged read-back still matches what was
        // received, a bootable image, and (unless forced) staged != active.
        if (bk_commit_pending || !bk_slave.active || bk_staged != bk_slave.total || bk_page_len != 0) {
            s2m->ack = BK_SPLIT_FLASH_ACK_SEQ_ERR;
            return;
        }

        uint32_t received = bk_crc32_finalize(bk_slave.crc);
        uint32_t staged   = bk_crc32_finalize(bk_readback_crc);
        if (received != staged) {
            s2m->ack = BK_SPLIT_FLASH_ACK_FLASH_ERR;
            return;
        }
        if (!bk_image_bootable(bk_slave.total)) {
            s2m->ack = BK_SPLIT_FLASH_ACK_BOOT_ERR;
            return;
        }
        // Re-clone guard: identical bytes mean there is nothing to install. The
        // step-4 validation one-shot sets non-zero `offset` to force one commit.
        if (m2s->offset == 0 && bk_crc32_finalize(bk_active_crc) == staged) {
            s2m->ack = BK_SPLIT_FLASH_ACK_MATCH;
            return;
        }

        bk_commit_len     = bk_slave.total;
        bk_commit_pending = true;
        bk_commit_timed   = false;
        s2m->ack          = BK_SPLIT_FLASH_ACK_COMMIT;
        return;
    }

    // Data chunk: in order, contiguous, CRC-8 intact, within the declared length.
    if (bk_commit_pending || !bk_slave.active || m2s->seq != bk_slave.next_seq || m2s->offset != bk_staged) {
        s2m->ack = BK_SPLIT_FLASH_ACK_SEQ_ERR;
        return;
    }

    uint32_t valid = bk_slave.total - m2s->offset;
    if (valid > BK_SPLIT_FLASH_CHUNK_DATA) {
        valid = BK_SPLIT_FLASH_CHUNK_DATA;
    }

    if (crc8(m2s->data, valid) != m2s->crc8) {
        s2m->ack = BK_SPLIT_FLASH_ACK_CRC8_ERR;
        return;
    }

    bk_slave.crc = bk_crc32_update(bk_slave.crc, m2s->data, valid);
    bk_stage_append(m2s->data, valid);
    if (bk_staged == bk_slave.total) {
        bk_stage_flush_partial_page(); // last, possibly short, page
    }
    bk_slave.next_seq++;
    s2m->ack = m2s->seq;
}

// Registered on both halves at post-init. Reuses the RPC shmem buffers (a module
// has no shared-memory buffer of its own) with the module's own fixed sizes.
static void bk_split_flash_register_transaction(void) {
    split_transaction_desc_t *trans = &split_transaction_table[BK_SPLIT_FLASH_TRANSACTION];

    trans->initiator2target_buffer_size = BK_SPLIT_FLASH_M2S_SIZE;
    trans->initiator2target_offset      = offsetof(split_shared_memory_t, rpc_m2s_buffer);
    trans->target2initiator_buffer_size = BK_SPLIT_FLASH_S2M_SIZE;
    trans->target2initiator_offset      = offsetof(split_shared_memory_t, rpc_s2m_buffer);
    trans->slave_callback               = bk_split_flash_slave_callback;
}

// Erase the staging slot on the secondary, ahead of the stream: a 4 KiB sector
// erase takes tens of ms and would blow the 20 ms transport read timeout if done
// inside a transaction, so it runs once here instead. Interrupts are disabled for
// the erase (XIP is off while it runs).
static void bk_split_flash_prepare_staging(void) {
    uint32_t len     = (uint32_t)(__flash_binary_end - __flash_binary_start);
    uint32_t rounded = (len + FLASH_SECTOR_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);

    if (len == 0 || rounded > BK_SPLIT_FLASH_STAGING_MAX) {
        bk_staging_erased = 0;
        return;
    }

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(BK_SPLIT_FLASH_STAGING_OFFSET, rounded);
    restore_interrupts(ints);
    bk_staging_erased = rounded;
}

// The flash primitives live in RAM, but a direct call from a RAM-resident
// routine needs a cross-region long-branch veneer, and the linker places those
// in the region being erased. Call through a volatile function pointer instead:
// the indirect `blx` needs no veneer and therefore survives sector 0 being gone.
typedef void (*bk_flash_erase_fn_t)(uint32_t flash_offs, size_t count);
typedef void (*bk_flash_program_fn_t)(uint32_t flash_offs, const uint8_t *data, size_t count);

static bk_flash_erase_fn_t volatile   bk_flash_erase   = flash_range_erase;
static bk_flash_program_fn_t volatile bk_flash_program = flash_range_program;

// Erase the active region and the wear-leveling EEPROM, copy staging over the
// active region, then reset. Runs with interrupts
// off and every instruction and byte of state in RAM: VTOR points at flash sector
// 0, which is erased here, so nothing may be fetched from the active region until
// after the reset. boot2 (page 0) is programmed last, so the image only becomes
// bootable once every other page is in place.
static void __no_inline_not_in_flash_func(bk_split_flash_commit)(void) {
    uint32_t image_len = bk_commit_len;
    uint32_t erase_len = (image_len + FLASH_SECTOR_SIZE - 1u) & ~(uint32_t)(FLASH_SECTOR_SIZE - 1u);
    uint32_t pages     = (image_len + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE;

    (void)save_and_disable_interrupts();

    bk_flash_erase(0, erase_len);

    // Reset the secondary's EEPROM like a fresh flash: erase the whole
    // wear-leveling region, so the new image boots with eeconfig re-initialised
    // (done before the copy, so once the image is bootable its EEPROM is clean).
    bk_flash_erase(WEAR_LEVELING_RP2040_FLASH_BASE, WEAR_LEVELING_BACKING_SIZE);

    for (uint32_t page = 1; page < pages; page++) {
        const volatile uint8_t *src = (const volatile uint8_t *)(XIP_BASE + BK_SPLIT_FLASH_STAGING_OFFSET + page * FLASH_PAGE_SIZE);
        // Volatile source prevents the compiler from turning this into a
        // memcpy call into flash while the active region is erased.
        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
            bk_page[i] = src[i];
        }
        bk_flash_program(page * FLASH_PAGE_SIZE, bk_page, FLASH_PAGE_SIZE);
    }

    {
        const volatile uint8_t *src = (const volatile uint8_t *)(XIP_BASE + BK_SPLIT_FLASH_STAGING_OFFSET);
        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++) {
            bk_page[i] = src[i];
        }
        bk_flash_program(0, bk_page, FLASH_PAGE_SIZE);
    }

    // Clean system reset; the new vector table is only used after the reset.
    __asm__ volatile("dsb 0xf" ::: "memory");
    *(volatile uint32_t *)(PPB_BASE + M0PLUS_AIRCR_OFFSET) = (0x05FAu << 16) | (1u << 2);
    for (;;) {
    }
}

void keyboard_post_init_bk_split_flash(void) {
    bk_split_flash_register_transaction();

    // The master streams its own image to the secondary, so only the secondary
    // needs a staging slot.
    if (!is_keyboard_master()) {
        bk_split_flash_prepare_staging();
    }
}

// --- Master: drive one stream, one transaction per housekeeping call -----------

typedef enum {
    BK_STREAM_IDLE = 0,
    BK_STREAM_BEGIN,
    BK_STREAM_DATA,
    BK_STREAM_REPORT,
    BK_STREAM_VERIFY,
    BK_STREAM_COMMIT,
} bk_stream_phase_t;

static bk_stream_phase_t bk_phase;
static uint32_t          bk_run_at_ms;   // absolute time of the first run start
static bool              bk_scheduled;
static uint32_t          bk_image_len;
static uint32_t          bk_image_boot2; // boot2 checksum of this image (step-4 marker)
static uint32_t          bk_chunk_count;
static uint32_t          bk_next_seq;
static uint32_t          bk_sent_crc;    // running CRC-32 of the bytes sent so far

// Step-5 trigger: the master clones its image to the secondary once, when it
// detects a new firmware was flashed to it. The fingerprint (whole-image CRC-32)
// is persisted in the module's EEPROM datablock; a mismatch means "new
// firmware". Outside the trigger
// no stream runs and no re-flash happens.
static bool     bk_triggered; // new firmware detected at boot
static bool     bk_done;      // clone attempt finished (success or failure)
static uint32_t bk_image_crc; // fingerprint of this image

// Whole-image CRC-32, used as the firmware fingerprint. QMK embeds the build
// date/time in the image, so this changes on every rebuild and reliably
// distinguishes a newly-flashed firmware from the one already recorded.
static uint32_t bk_image_crc_compute(void) {
    uint32_t len = (uint32_t)(__flash_binary_end - __flash_binary_start);
    return bk_crc32_finalize(bk_crc32_update(0xFFFFFFFFu, __flash_binary_start, len));
}

// Detect a new firmware: compare the persisted fingerprint with this image's.
// Whether or not a reflash erases the EEPROM region, a new image has a new CRC
// (and a fresh datablock is zeroed by eeconfig init), so either way a mismatch
// means the master was just updated.
static bool bk_trigger_detect(void) {
    bk_image_crc = bk_image_crc_compute();
    uint32_t stored = 0;
    eeconfig_read_bk_split_flash_datablock(&stored, 0, sizeof(stored));
    return stored != bk_image_crc;
}

// Persist the fingerprint after a successful clone, so the next boot does not
// re-trigger. Deliberately not called on failure, so a failed clone re-triggers
// on the next boot.
static void bk_trigger_persist(void) {
    eeconfig_update_bk_split_flash_datablock(&bk_image_crc, 0, sizeof(bk_image_crc));
}

static void bk_trigger_marker(bool triggered) {
    if (triggered) {
        xprintf("BKCLONE: step=5 trigger=1 crc=0x%08X ok\n", (unsigned)bk_image_crc);
    } else {
        xprintf("BKCLONE: step=5 trigger=0 ok\n");
    }
}

// Terminal state of the one-shot stream: stop and never re-run.
static void bk_stream_finish(void) {
    bk_phase = BK_STREAM_IDLE;
    bk_done  = true;
}

// Three distinct markers per run. `transport` covers the link (the slave's
// rolling CRC-32 of the bytes it received vs. the master's CRC-32 of the bytes it
// sent), `flash` covers the staging slot (the CRC-32 of the staging read-back vs.
// that same expected value), and `commit` is the step-4 verdict. A failing
// `flash` marker with a passing `transport` marker is unambiguously a flash-write
// bug; a failing `commit` marker with the first two passing isolates the
// bootability/commit path.
static void bk_transport_marker(bool ok, const char *reason, uint32_t slave_crc) {
    uint32_t own_crc = bk_crc32_finalize(bk_sent_crc);

    if (ok) {
        xprintf("BKCLONE: step=3 transport chunks=%u bytes=%u crc=0x%08X slave=0x%08X ok\n", (unsigned)bk_chunk_count, (unsigned)bk_image_len, (unsigned)own_crc, (unsigned)slave_crc);
    } else {
        xprintf("BKCLONE: step=3 transport chunks=%u bytes=%u crc=0x%08X slave=0x%08X FAIL reason=%s\n", (unsigned)bk_chunk_count, (unsigned)bk_image_len, (unsigned)own_crc, (unsigned)slave_crc, reason);
    }
}

static void bk_flash_marker(bool ok, const char *reason, uint32_t readback_crc) {
    uint32_t own_crc = bk_crc32_finalize(bk_sent_crc);

    if (ok) {
        xprintf("BKCLONE: step=3 flash bytes=%u crc=0x%08X readback=0x%08X ok\n", (unsigned)bk_image_len, (unsigned)own_crc, (unsigned)readback_crc);
    } else {
        xprintf("BKCLONE: step=3 flash bytes=%u crc=0x%08X readback=0x%08X FAIL reason=%s\n", (unsigned)bk_image_len, (unsigned)own_crc, (unsigned)readback_crc, reason);
    }
}

// Step-4 verdict: the secondary's answer to the COMMIT op. `action` is `commit`
// when the secondary will erase/copy/reset, or `skip` when the staged image
// already matches its active image.
static void bk_commit_marker(bool ok, const char *action, const char *reason) {
    if (ok) {
        xprintf("BKCLONE: step=4 commit bytes=%u boot2=0x%08X action=%s ok\n", (unsigned)bk_image_len, (unsigned)bk_image_boot2, action);
    } else {
        xprintf("BKCLONE: step=4 commit bytes=%u boot2=0x%08X FAIL reason=%s\n", (unsigned)bk_image_len, (unsigned)bk_image_boot2, reason);
    }
}

static void bk_stream_start(void) {
    bk_image_len   = (uint32_t)(__flash_binary_end - __flash_binary_start);
    bk_image_boot2 = bk_boot2_checksum(__flash_binary_start);
    bk_chunk_count = (bk_image_len + BK_SPLIT_FLASH_CHUNK_DATA - 1) / BK_SPLIT_FLASH_CHUNK_DATA;
    bk_next_seq    = 1;
    bk_sent_crc    = 0xFFFFFFFFu;

    if (bk_image_len == 0) {
        bk_flash_marker(false, "empty", 0);
        bk_stream_finish();
    } else {
        bk_phase = BK_STREAM_BEGIN;
    }
}

static void bk_stream_step(void) {
    bk_split_flash_m2s_t m2s = {0};
    bk_split_flash_s2m_t s2m = {0};

    switch (bk_phase) {
        case BK_STREAM_BEGIN: {
            m2s.seq    = BK_SPLIT_FLASH_SEQ_BEGIN;
            m2s.offset = bk_image_len;

            bool ok = transport_execute_transaction(BK_SPLIT_FLASH_TRANSACTION, &m2s, sizeof(m2s), &s2m, sizeof(s2m));
            if (ok && s2m.ack == BK_SPLIT_FLASH_ACK_BEGIN) {
                bk_phase = BK_STREAM_DATA;
            } else if (!ok) {
                bk_transport_marker(false, "begin-tx", 0);
                bk_stream_finish();
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_LEN_ERR) {
                bk_flash_marker(false, "len", 0);
                bk_stream_finish();
            } else {
                bk_transport_marker(false, "begin-ack", s2m.ack);
                bk_stream_finish();
            }
            break;
        }

        case BK_STREAM_DATA: {
            uint32_t offset = (bk_next_seq - 1) * BK_SPLIT_FLASH_CHUNK_DATA;
            uint32_t valid  = bk_image_len - offset;
            if (valid > BK_SPLIT_FLASH_CHUNK_DATA) {
                valid = BK_SPLIT_FLASH_CHUNK_DATA;
            }

            m2s.seq    = bk_next_seq;
            m2s.offset = offset;
            memcpy(m2s.data, &__flash_binary_start[offset], valid);
            m2s.crc8 = crc8(m2s.data, valid);

            bool        ok     = transport_execute_transaction(BK_SPLIT_FLASH_TRANSACTION, &m2s, sizeof(m2s), &s2m, sizeof(s2m));
            const char *reason = NULL;
            if (!ok) {
                reason = "tx";
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_SEQ_ERR) {
                reason = "seq";
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_CRC8_ERR) {
                reason = "crc8";
            } else if (s2m.ack != m2s.seq) {
                reason = "ack";
            }

            if (reason != NULL) {
                bk_transport_marker(false, reason, s2m.ack);
                bk_stream_finish();
                break;
            }

            bk_sent_crc = bk_crc32_update(bk_sent_crc, m2s.data, valid);
            if (++bk_next_seq > bk_chunk_count) {
                bk_phase = BK_STREAM_REPORT;
            }
            break;
        }

        case BK_STREAM_REPORT: {
            m2s.seq = BK_SPLIT_FLASH_SEQ_REPORT;

            bool     ok        = transport_execute_transaction(BK_SPLIT_FLASH_TRANSACTION, &m2s, sizeof(m2s), &s2m, sizeof(s2m));
            uint32_t own_crc   = bk_crc32_finalize(bk_sent_crc);
            uint32_t slave_crc = ok ? s2m.ack : 0;
            if (ok && slave_crc == own_crc) {
                bk_transport_marker(true, NULL, slave_crc);
                bk_phase = BK_STREAM_VERIFY;
            } else {
                bk_transport_marker(false, ok ? "crc" : "tx", slave_crc);
                bk_stream_finish();
            }
            break;
        }

        case BK_STREAM_VERIFY: {
            m2s.seq = BK_SPLIT_FLASH_SEQ_VERIFY;

            bool     ok       = transport_execute_transaction(BK_SPLIT_FLASH_TRANSACTION, &m2s, sizeof(m2s), &s2m, sizeof(s2m));
            uint32_t own_crc  = bk_crc32_finalize(bk_sent_crc);
            uint32_t readback = ok ? s2m.ack : 0;
            if (ok && readback == own_crc) {
                bk_flash_marker(true, NULL, readback);
                bk_phase = BK_STREAM_COMMIT;
            } else {
                bk_flash_marker(false, ok ? "crc" : "tx", readback);
                bk_stream_finish();
            }
            break;
        }

        case BK_STREAM_COMMIT: {
            m2s.seq = BK_SPLIT_FLASH_SEQ_COMMIT;

            bool ok = transport_execute_transaction(BK_SPLIT_FLASH_TRANSACTION, &m2s, sizeof(m2s), &s2m, sizeof(s2m));
            if (!ok) {
                bk_commit_marker(false, NULL, "tx");
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_COMMIT) {
                bk_trigger_persist();
                bk_commit_marker(true, "commit", NULL);
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_MATCH) {
                bk_trigger_persist();
                bk_commit_marker(true, "skip", NULL);
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_BOOT_ERR) {
                bk_commit_marker(false, NULL, "boot");
            } else if (s2m.ack == BK_SPLIT_FLASH_ACK_FLASH_ERR) {
                bk_commit_marker(false, NULL, "flash");
            } else {
                bk_commit_marker(false, NULL, "ack");
            }

            bk_stream_finish();
            break;
        }

        default:
            break;
    }
}

void housekeeping_task_bk_split_flash(void) {
    if (!is_keyboard_master()) {
        // The commit was requested from the split thread. Give the master time to
        // receive the acknowledgment, then run the long interrupts-off commit
        // here, where freezing the link is expected (the secondary is resetting).
        if (bk_commit_pending) {
            if (!bk_commit_timed) {
                bk_commit_timed = true;
                bk_commit_at_ms = timer_read32() + BK_SPLIT_FLASH_COMMIT_DELAY_MS;
            }
            if ((int32_t)(timer_read32() - bk_commit_at_ms) >= 0) {
                bk_commit_pending = false;
                bk_split_flash_commit(); // does not return
            }
        }
        return;
    }

    if (!bk_scheduled) {
        bk_scheduled = true;
        bk_triggered = bk_trigger_detect();
        bk_run_at_ms = timer_read32() + BK_SPLIT_FLASH_FIRST_RUN_DELAY_MS;
    }

    if (bk_done) {
        return;
    }

    if (bk_phase == BK_STREAM_IDLE) {
        if ((int32_t)(timer_read32() - bk_run_at_ms) < 0) {
            return;
        }
        bk_trigger_marker(bk_triggered);
        if (bk_triggered) {
            bk_stream_start();
        } else {
            bk_done = true;
        }
        return;
    }

    bk_stream_step();
}
