// Copyright 2026 bastardkb
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bk_split_flash trigger + transport side: the capability/fingerprint handshake
// over the module's RPC transaction (the Phase 0 probe/echo is the transport
// seam). Each half computes a runtime CRC-32 over its own active image region
// [XIP_BASE, &__flash_binary_end). The master runs the handshake once per boot
// (bounded probing); the slave answers with its own protocol version, image
// length and image CRC. The master applies the plan's skip policy (no reply /
// version mismatch / identical fingerprint -> skip) and sets a one-shot per-boot
// guard, so the primary's boot is never blocked. A differing fingerprint records
// that a transfer would begin; the transfer itself lands in Phase 3.
#include "debug.h"
#include "timer.h"
#include "transactions.h"
#include "community_modules.h"
#include "split_util.h"
#include "hardware/regs/addressmap.h"

#include "bk_split_flash.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 2);

_Static_assert(sizeof(bk_split_flash_probe_t) <= RPC_M2S_BUFFER_SIZE, "bk_split_flash: probe does not fit in RPC_M2S_BUFFER_SIZE.");
_Static_assert(sizeof(bk_split_flash_echo_t) <= RPC_S2M_BUFFER_SIZE, "bk_split_flash: echo does not fit in RPC_S2M_BUFFER_SIZE.");

// Own image fingerprint, computed once per boot (flash is immutable while
// running). The active image never changes under us in this phase.
static bool     bk_split_flash_self_ready;
static uint32_t bk_split_flash_self_len;
static uint32_t bk_split_flash_self_crc;

static void bk_split_flash_ensure_self_fingerprint(void) {
    if (bk_split_flash_self_ready) {
        return;
    }
    bk_split_flash_self_len = bk_split_flash_active_image_len();
    bk_split_flash_self_crc = bk_split_flash_crc32((const uint8_t *)XIP_BASE, bk_split_flash_self_len);
    bk_split_flash_self_ready = true;
}

// Slave side of the RPC: answer the capability/fingerprint probe. Runs in the
// serial thread context; the CRC is computed at most once per boot.
static void bk_split_flash_slave_handler(uint8_t in_buflen, const void *in_data, uint8_t out_buflen, void *out_data) {
    const bk_split_flash_probe_t *probe = (const bk_split_flash_probe_t *)in_data;
    bk_split_flash_echo_t *echo         = (bk_split_flash_echo_t *)out_data;

    if (in_buflen < sizeof(*probe) || out_buflen < sizeof(*echo)) {
        print("bk_split_flash: slave probe buffer too small\r\n");
        return;
    }

    // Report our OWN capability honestly; the master decides. A probe without
    // our magic gets a 0xFF status (Phase 0 semantics).
    echo->magic   = BK_SPLIT_FLASH_MAGIC;
    echo->version = BK_SPLIT_FLASH_PROTOCOL_VERSION;
    echo->seq     = probe->seq;
    if (probe->magic != BK_SPLIT_FLASH_MAGIC) {
        echo->status    = 0xFF;
        echo->image_len = 0;
        echo->image_crc = 0;
        return;
    }
    echo->status = BK_SPLIT_FLASH_STATUS_OK;
    bk_split_flash_ensure_self_fingerprint();
    echo->image_len = bk_split_flash_self_len;
    echo->image_crc = bk_split_flash_self_crc;
}

// ---------------------------------------------------------------------------
// Master-side handshake: one decision per boot, bounded by
// SPLIT_FLASH_HANDSHAKE_TIMEOUT. Probing is gated on the board's own split
// connectivity (is_transport_connected): a peer that is not answering normal
// split syncs is mid-boot or gone, and reading from it would block the shared
// main loop for SERIAL_USART_TIMEOUT per failed read - at this board's ~3 s
// boot skew that made the primary appear dead for seconds (root-caused
// 2026-11-13). A late-booting secondary is still caught because the connected
// flag flips as soon as it answers a normal sync; the probe then settles
// quickly, so the primary's boot is never blocked.
// ---------------------------------------------------------------------------
static bool     bk_split_flash_handshake_done;
static bool     bk_split_flash_handshake_started;
static bool     bk_split_flash_probe_failed_printed;
static bool     bk_split_flash_unusable_reply_printed;
static uint8_t  bk_split_flash_probe_rejects;
static uint32_t bk_split_flash_handshake_started_at;
static uint32_t bk_split_flash_last_probe;
// Consumed by the Phase 3 transfer: whether the peer's fingerprint differs.
static bool     bk_split_flash_handshake_would_transfer;

// Consecutive transport-level probe failures before settling on a skip. A peer
// that answers the board's normal syncs but consistently rejects our RPC is
// running different firmware; retrying it only burns main-loop time.
#define BK_SPLIT_FLASH_PROBE_MAX_TRIES 3

static void bk_split_flash_attempt_probe(void) {
    bk_split_flash_ensure_self_fingerprint();

    bk_split_flash_probe_t probe = {
        .magic     = BK_SPLIT_FLASH_MAGIC,
        .version   = BK_SPLIT_FLASH_PROTOCOL_VERSION,
        .seq       = (uint8_t)(timer_read32() & 0xFF),
        .image_len = bk_split_flash_self_len,
        .image_crc = bk_split_flash_self_crc,
    };
    bk_split_flash_echo_t echo = {0};

    if (!transaction_rpc_exec(PUT_BK_SPLIT_FLASH, sizeof(probe), &probe, sizeof(echo), &echo)) {
        // No RPC reply. A couple of retries cover transient glitches; a peer
        // that talks split sync but not this RPC (older/other firmware) is
        // settled as a skip instead of hammered for the whole window.
        if (!bk_split_flash_probe_failed_printed) {
            bk_split_flash_probe_failed_printed = true;
            print("bk_split_flash: probe failed (secondary not connected?)\r\n");
        }
        if (++bk_split_flash_probe_rejects >= BK_SPLIT_FLASH_PROBE_MAX_TRIES) {
            print("bk_split_flash: DECISION skip (no current-protocol reply from the secondary)\r\n");
            bk_split_flash_handshake_done = true;
        }
        return;
    }
    bk_split_flash_probe_rejects = 0;

    // Only a reply that matches the current protocol is read further; anything
    // else means the peer is running different firmware. There is no
    // cross-version handling - settling on a skip.
    if (!bk_split_flash_reply_usable(echo.magic == BK_SPLIT_FLASH_MAGIC, echo.seq == probe.seq, echo.image_len)) {
        if (!bk_split_flash_unusable_reply_printed) {
            bk_split_flash_unusable_reply_printed = true;
            xprintf("bk_split_flash: probe answered but reply is not current protocol (magic=0x%02x seq=%u len=%lu)\r\n", echo.magic, echo.seq, (unsigned long)echo.image_len);
        }
        print("bk_split_flash: DECISION skip (secondary not running a compatible protocol)\r\n");
        bk_split_flash_handshake_done = true;
        return;
    }

    xprintf("bk_split_flash: probe seq=%u -> echo seq=%u magic=0x%02x version=%u status=%u len=%lu crc=0x%08lx\r\n", probe.seq, echo.seq, echo.magic, echo.version, echo.status, (unsigned long)echo.image_len, (unsigned long)echo.image_crc);

    uint8_t decision = bk_split_flash_handshake_decide(echo.version, echo.image_len, echo.image_crc, bk_split_flash_self_len, bk_split_flash_self_crc);
    if (decision == BK_SPLIT_FLASH_DECISION_SKIP_IDENTICAL) {
        xprintf("bk_split_flash: DECISION skip (identical fingerprint len=%lu crc=0x%08lx, nothing to clone)\r\n", (unsigned long)echo.image_len, (unsigned long)echo.image_crc);
    } else if (decision == BK_SPLIT_FLASH_DECISION_SKIP_VERSION) {
        xprintf("bk_split_flash: DECISION skip (peer protocol version %u != local %u)\r\n", echo.version, BK_SPLIT_FLASH_PROTOCOL_VERSION);
    } else {
        bk_split_flash_handshake_would_transfer = true;
        xprintf("bk_split_flash: DECISION begin transfer (peer fingerprint differs: len %lu vs %lu, crc 0x%08lx vs 0x%08lx) [transfer lands in Phase 3]\r\n", (unsigned long)echo.image_len, (unsigned long)bk_split_flash_self_len, (unsigned long)echo.image_crc, (unsigned long)bk_split_flash_self_crc);
    }
    bk_split_flash_handshake_done = true;
}

void keyboard_post_init_bk_split_flash(void) {
    transaction_register_rpc(PUT_BK_SPLIT_FLASH, bk_split_flash_slave_handler);
    xprintf("bk_split_flash: post_init on %s half, RPC registered\r\n", is_keyboard_master() ? "master" : "slave");
}

void housekeeping_task_bk_split_flash(void) {
    // The handshake lives only on the master; the slave answers via the RPC.
    if (!is_keyboard_master()) {
        return;
    }
    if (bk_split_flash_handshake_done) {
        return;
    }
    if (!bk_split_flash_handshake_started) {
        bk_split_flash_handshake_started    = true;
        bk_split_flash_handshake_started_at = timer_read32();
        bk_split_flash_last_probe           = 0;
        print("bk_split_flash: housekeeping on master half, probing\r\n");
    }
    if (timer_elapsed32(bk_split_flash_handshake_started_at) > SPLIT_FLASH_HANDSHAKE_TIMEOUT) {
        print("bk_split_flash: DECISION skip (no reply within SPLIT_FLASH_HANDSHAKE_TIMEOUT)\r\n");
        bk_split_flash_handshake_done = true;
        return;
    }
    // Never run serial transactions against a peer that is not currently
    // answering the board's own split sync (root cause of the primary boot
    // stall found 2026-11-13: each failed read blocks the main loop up to
    // SERIAL_USART_TIMEOUT).
    if (!is_transport_connected()) {
        return;
    }
    if (timer_elapsed32(bk_split_flash_last_probe) >= BK_SPLIT_FLASH_PROBE_INTERVAL_MS) {
        bk_split_flash_last_probe = timer_read32();
        bk_split_flash_attempt_probe();
    }
}