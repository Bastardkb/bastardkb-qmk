// SPDX-License-Identifier: GPL-2.0-or-later
#include <stddef.h>
#include <string.h>

#include "debug.h"
#include "timer.h"
#include "keyboard.h"
#include "transactions.h"
#include "crc.h"
#include "community_modules.h"

#include "bk_split_flash.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 0, 0);

// Transaction id value from the generated serial_transaction_id enum.
#define BK_SPLIT_FLASH_TRANSACTION PUT_BK_SPLIT_FLASH

// The image streamed to the secondary: [XIP_BASE, __flash_binary_end). In
// RP2040_FLASH_TIMECRIT.ld `__flash_binary_start` is flash1's origin (XIP_BASE,
// 0x10000000) and `__flash_binary_end` the end of the load region.
extern const uint8_t __flash_binary_start[];
extern const uint8_t __flash_binary_end[];

STATIC_ASSERT(sizeof(bk_split_flash_m2s_t) == BK_SPLIT_FLASH_M2S_SIZE, "m2s chunk must be full-size");
STATIC_ASSERT(sizeof(bk_split_flash_s2m_t) == BK_SPLIT_FLASH_S2M_SIZE, "s2m ack size mismatch");

// Reflected CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF). QMK's
// quantum/crc.c only offers a whole-buffer CRC-8, so both halves use this
// incremental CRC-32 instead; the identical code on both halves is what makes the
// master's and the slave's values directly comparable.
static uint32_t bk_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc;
}

// Runs in the slave's split protocol thread. The passed lengths are ignored: the
// sizes are fixed in the transaction table (the core passes the m2s size for the out
// size too, so they are not trustworthy here). Only a rolling CRC-32 and a little
// stream state are kept -- never a copy of the image.
typedef struct {
    bool     active;
    uint32_t total;
    uint32_t next_seq;
    uint32_t crc;
} bk_slave_stream_t;

static bk_slave_stream_t bk_slave;

static void bk_split_flash_slave_callback(uint8_t initiator2target_buffer_size, const void *initiator2target_buffer, uint8_t target2initiator_buffer_size, void *target2initiator_buffer) {
    (void)initiator2target_buffer_size;
    (void)target2initiator_buffer_size;

    const bk_split_flash_m2s_t *m2s = (const bk_split_flash_m2s_t *)initiator2target_buffer;
    bk_split_flash_s2m_t       *s2m = (bk_split_flash_s2m_t *)target2initiator_buffer;

    if (m2s->seq == BK_SPLIT_FLASH_SEQ_BEGIN) {
        bk_slave.active   = true;
        bk_slave.total    = m2s->offset;
        bk_slave.next_seq = 1;
        bk_slave.crc      = 0xFFFFFFFFu;
        s2m->ack          = BK_SPLIT_FLASH_ACK_BEGIN;
        return;
    }

    if (m2s->seq == BK_SPLIT_FLASH_SEQ_REPORT) {
        s2m->ack = bk_slave.crc ^ 0xFFFFFFFFu;
        return;
    }

    // Data chunk: in order, CRC-8 intact, and within the declared image length.
    if (!bk_slave.active || m2s->seq != bk_slave.next_seq || m2s->offset >= bk_slave.total) {
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
    bk_slave.next_seq++;
    s2m->ack = m2s->seq;
}

// Registered on both halves at post-init. Reuses the RPC shmem buffers (a module has
// no shared-memory buffer of its own) with the module's own fixed sizes.
static void bk_split_flash_register_transaction(void) {
    split_transaction_desc_t *trans = &split_transaction_table[BK_SPLIT_FLASH_TRANSACTION];

    trans->initiator2target_buffer_size = BK_SPLIT_FLASH_M2S_SIZE;
    trans->initiator2target_offset      = offsetof(split_shared_memory_t, rpc_m2s_buffer);
    trans->target2initiator_buffer_size = BK_SPLIT_FLASH_S2M_SIZE;
    trans->target2initiator_offset      = offsetof(split_shared_memory_t, rpc_s2m_buffer);
    trans->slave_callback               = bk_split_flash_slave_callback;
}

// --- Master: drive one stream, one transaction per housekeeping call -------------

typedef enum {
    BK_STREAM_IDLE = 0,
    BK_STREAM_BEGIN,
    BK_STREAM_DATA,
    BK_STREAM_REPORT,
} bk_stream_phase_t;

static bk_stream_phase_t bk_phase;
static uint32_t          bk_run_at_ms;   // absolute time of the next run start
static bool              bk_scheduled;
static uint32_t          bk_image_len;
static uint32_t          bk_chunk_count;
static uint32_t          bk_next_seq;
static uint32_t          bk_sent_crc;    // running CRC-32 of the bytes sent so far

static void bk_marker(bool ok, const char *reason, uint32_t at, uint32_t slave_crc) {
    uint32_t own_crc = bk_sent_crc ^ 0xFFFFFFFFu;

    if (ok) {
        xprintf("BKCLONE: step=2 chunks=%u bytes=%u crc=0x%08X slave=0x%08X ok\n", (unsigned)bk_chunk_count, (unsigned)bk_image_len, (unsigned)own_crc, (unsigned)slave_crc);
    } else {
        xprintf("BKCLONE: step=2 chunks=%u bytes=%u crc=0x%08X slave=0x%08X FAIL at=%u reason=%s\n", (unsigned)bk_chunk_count, (unsigned)bk_image_len, (unsigned)own_crc, (unsigned)slave_crc, (unsigned)at, reason);
    }

    bk_phase = BK_STREAM_IDLE;
}

static void bk_stream_start(void) {
    bk_image_len   = (uint32_t)(__flash_binary_end - __flash_binary_start);
    bk_chunk_count = (bk_image_len + BK_SPLIT_FLASH_CHUNK_DATA - 1) / BK_SPLIT_FLASH_CHUNK_DATA;
    bk_next_seq    = 1;
    bk_sent_crc    = 0xFFFFFFFFu;
    bk_run_at_ms   = timer_read32() + BK_SPLIT_FLASH_RUN_PERIOD_MS;
    bk_phase       = BK_STREAM_BEGIN;
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
            } else {
                bk_marker(false, ok ? "begin" : "tx", 0, s2m.ack);
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
                bk_marker(false, reason, m2s.seq, s2m.ack);
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
            uint32_t own_crc   = bk_sent_crc ^ 0xFFFFFFFFu;
            uint32_t slave_crc = ok ? s2m.ack : 0;
            if (ok && slave_crc == own_crc) {
                bk_marker(true, NULL, 0, slave_crc);
            } else {
                bk_marker(false, ok ? "crc" : "tx", bk_chunk_count, slave_crc);
            }
            break;
        }

        default:
            break;
    }
}

void keyboard_post_init_bk_split_flash(void) {
    bk_split_flash_register_transaction();
}

void housekeeping_task_bk_split_flash(void) {
    if (!is_keyboard_master()) {
        return;
    }

    if (!bk_scheduled) {
        bk_scheduled = true;
        bk_run_at_ms = timer_read32() + BK_SPLIT_FLASH_FIRST_RUN_DELAY_MS;
    }

    if (bk_phase == BK_STREAM_IDLE) {
        if ((int32_t)(timer_read32() - bk_run_at_ms) < 0) {
            return;
        }
        bk_stream_start();
    }

    bk_stream_step();
}
