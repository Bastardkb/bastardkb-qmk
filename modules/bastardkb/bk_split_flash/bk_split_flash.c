// SPDX-License-Identifier: GPL-2.0-or-later
#include <stddef.h>

#include "debug.h"
#include "timer.h"
#include "keyboard.h"
#include "transactions.h"
#include "community_modules.h"

#include "bk_split_flash.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 0, 0);

// Transaction id value from the generated serial_transaction_id enum.
#define BK_SPLIT_FLASH_TRANSACTION PUT_BK_SPLIT_FLASH

#define BK_SPLIT_FLASH_PROBE_INTERVAL_MS 500

STATIC_ASSERT(sizeof(bk_split_flash_m2s_t) == BK_SPLIT_FLASH_M2S_SIZE, "m2s chunk must be full-size");
STATIC_ASSERT(sizeof(bk_split_flash_s2m_t) == BK_SPLIT_FLASH_S2M_SIZE, "s2m ack size mismatch");

// Runs in the slave's split protocol thread. The passed lengths are ignored: the
// sizes are fixed in the transaction table (the core passes the m2s size for the
// out size too, so they are not trustworthy here).
static void bk_split_flash_slave_callback(uint8_t initiator2target_buffer_size, const void *initiator2target_buffer, uint8_t target2initiator_buffer_size, void *target2initiator_buffer) {
    (void)initiator2target_buffer_size;
    (void)target2initiator_buffer_size;

    const bk_split_flash_m2s_t *m2s = (const bk_split_flash_m2s_t *)initiator2target_buffer;
    bk_split_flash_s2m_t       *s2m = (bk_split_flash_s2m_t *)target2initiator_buffer;

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

void keyboard_post_init_bk_split_flash(void) {
    bk_split_flash_register_transaction();
}

void housekeeping_task_bk_split_flash(void) {
    if (!is_keyboard_master()) {
        return;
    }

    static uint32_t last_probe = 0;
    static uint32_t seq        = 0;

    if (timer_elapsed32(last_probe) < BK_SPLIT_FLASH_PROBE_INTERVAL_MS) {
        return;
    }
    last_probe = timer_read32();

    bk_split_flash_m2s_t m2s = {0};
    bk_split_flash_s2m_t s2m = {0};

    m2s.seq = ++seq;
    for (size_t i = 0; i < sizeof(m2s.data); i++) {
        m2s.data[i] = (uint8_t)(m2s.seq + i);
    }

    bool ok = transport_execute_transaction(BK_SPLIT_FLASH_TRANSACTION, &m2s, sizeof(m2s), &s2m, sizeof(s2m));
    if (ok && s2m.ack == m2s.seq) {
        xprintf("BKCLONE: step=1 seq=%u m2s_len=%u ack=%u ok\n", (unsigned)m2s.seq, (unsigned)sizeof(m2s), (unsigned)s2m.ack);
    } else {
        xprintf("BKCLONE: step=1 seq=%u m2s_len=%u ack=%u FAIL\n", (unsigned)m2s.seq, (unsigned)sizeof(m2s), (unsigned)s2m.ack);
    }
}
