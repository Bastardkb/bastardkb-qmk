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
#include "transport.h"
#include "crc.h"
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

#ifdef BK_SPLIT_FLASH_DIAG
    // DIAG (TEMPORARY - 2026-11-13): capture ready-state and timing around the
    // fingerprint work. ready_before==0 means this probe reached the slave
    // BEFORE keyboard_post_init ran (i.e. the H2 inline-CRC path); a large dt_ms
    // then is the late echo that desyncs the half-duplex wire.
    static uint32_t bk_split_flash_slave_handler_calls;
    uint8_t         diag_ready_before = bk_split_flash_self_ready ? 1u : 0u;
    uint32_t        diag_t_start      = timer_read32();
    bk_split_flash_slave_handler_calls++;
#endif

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

#ifdef BK_SPLIT_FLASH_DIAG
    uint32_t diag_dt = timer_elapsed32(diag_t_start);
    echo->diag_self_ready    = diag_ready_before;
    echo->diag_handler_ms    = (diag_dt > 255u) ? 255u : (uint8_t)diag_dt;
    echo->diag_handler_calls = (bk_split_flash_slave_handler_calls > 255u) ? 255u : (uint8_t)bk_split_flash_slave_handler_calls;
    xprintf("bk_split_flash: DIAG slave handler ready_before=%u dt_ms=%lu calls=%u\r\n", diag_ready_before, (unsigned long)diag_dt, (unsigned)bk_split_flash_slave_handler_calls);
#endif
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

// BK_SPLIT_FLASH_PROBE_MAX_TRIES now lives in config.h (moved 2026-11-13 so the
// DIAG build can widen it to 200 without touching decision logic).

#ifdef BK_SPLIT_FLASH_DIAG
// DIAG (TEMPORARY - 2026-11-13): master-side wire observation. attempt counter
// and last transport-connected state, used only by the DIAG log lines.
static uint32_t bk_split_flash_diag_probe_attempts;
static bool     bk_split_flash_diag_last_conn;
#endif

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

#ifdef BK_SPLIT_FLASH_DIAG
    // DIAG (TEMPORARY - 2026-11-13): per-attempt wire observation. dt localizes
    // the failing phase: a ~timeout block is the read timeout, a fast dt (<5 ms)
    // signals a handshake/id mismatch rather than a timeout.
    bk_split_flash_diag_probe_attempts++;
    xprintf("bk_split_flash: DIAG attempt #%lu t=%lu conn=%u rejects=%u\r\n", (unsigned long)bk_split_flash_diag_probe_attempts, (unsigned long)timer_read32(), is_transport_connected() ? 1u : 0u, (unsigned)bk_split_flash_probe_rejects);
    uint32_t diag_t_start = timer_read32();

    // DIAG: replicate transaction_rpc_exec's 4 phases with per-phase logging
    // (mirrors transactions.c:1092-1117) so we can see WHICH phase fails:
    //   info  = PUT_RPC_INFO write      -> transport/XOR-handshake accepted?
    //   req   = PUT_RPC_REQ_DATA write  -> request payload accepted?
    //   exec  = EXECUTE_RPC write       -> callback invoked on the slave?
    //   read  = GET_RPC_RESP_DATA read  -> echo received within the window?
    rpc_sync_info_t info = {.payload = {.transaction_id = PUT_BK_SPLIT_FLASH, .m2s_length = sizeof(probe), .s2m_length = sizeof(echo)}};
    info.checksum        = crc8(&info.payload, sizeof(info.payload));
    split_transaction_table[PUT_RPC_REQ_DATA].initiator2target_buffer_size  = sizeof(probe);
    split_transaction_table[GET_RPC_RESP_DATA].target2initiator_buffer_size = sizeof(echo);
    int8_t rpc_id = PUT_BK_SPLIT_FLASH;

    uint32_t ph_t;
    ph_t         = timer_read32();
    bool     ph1 = transport_execute_transaction(PUT_RPC_INFO, &info, sizeof(info), NULL, 0);
    uint32_t d1  = timer_elapsed32(ph_t);
    ph_t         = timer_read32();
    bool     ph2 = ph1 && transport_execute_transaction(PUT_RPC_REQ_DATA, &probe, sizeof(probe), NULL, 0);
    uint32_t d2  = timer_elapsed32(ph_t);
    ph_t         = timer_read32();
    bool     ph3 = ph2 && transport_execute_transaction(EXECUTE_RPC, &rpc_id, sizeof(rpc_id), NULL, 0);
    uint32_t d3  = timer_elapsed32(ph_t);
    ph_t         = timer_read32();
    bool     ph4 = ph3 && transport_execute_transaction(GET_RPC_RESP_DATA, NULL, 0, &echo, sizeof(echo));
    uint32_t d4  = timer_elapsed32(ph_t);

    bool     ok    = ph1 && ph2 && ph3 && ph4;
    uint32_t diag_dt = timer_elapsed32(diag_t_start);
    xprintf("bk_split_flash: DIAG phases info=%u(%lu) req=%u(%lu) exec=%u(%lu) read=%u(%lu)\r\n", ph1, (unsigned long)d1, ph2, (unsigned long)d2, ph3, (unsigned long)d3, ph4, (unsigned long)d4);
#else
    bool ok = transaction_rpc_exec(PUT_BK_SPLIT_FLASH, sizeof(probe), &probe, sizeof(echo), &echo);
#endif

    if (!ok) {
        // No RPC reply. A couple of retries cover transient glitches; a peer
        // that talks split sync but not this RPC (older/other firmware) is
        // settled as a skip instead of hammered for the whole window.
        if (!bk_split_flash_probe_failed_printed) {
            bk_split_flash_probe_failed_printed = true;
            print("bk_split_flash: probe failed (secondary not connected?)\r\n");
        }
        bk_split_flash_probe_rejects++;
#ifdef BK_SPLIT_FLASH_DIAG
        xprintf("bk_split_flash: DIAG attempt #%lu FAIL t=%lu dt=%lu rejects=%u conn=%u\r\n", (unsigned long)bk_split_flash_diag_probe_attempts, (unsigned long)timer_read32(), (unsigned long)diag_dt, (unsigned)bk_split_flash_probe_rejects, is_transport_connected() ? 1u : 0u);
#endif
        if (bk_split_flash_probe_rejects >= BK_SPLIT_FLASH_PROBE_MAX_TRIES) {
            xprintf("bk_split_flash: DECISION skip (no current-protocol reply from the secondary) t=%lu rejects=%u\r\n", (unsigned long)timer_read32(), (unsigned)bk_split_flash_probe_rejects);
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
            xprintf("bk_split_flash: probe answered but reply is not current protocol (magic=0x%02x version=%u seq=%u status=%u len=%lu)\r\n", echo.magic, echo.version, echo.seq, echo.status, (unsigned long)echo.image_len);
        }
        xprintf("bk_split_flash: DECISION skip (secondary not running a compatible protocol) t=%lu rejects=%u\r\n", (unsigned long)timer_read32(), (unsigned)bk_split_flash_probe_rejects);
        bk_split_flash_handshake_done = true;
        return;
    }

#ifdef BK_SPLIT_FLASH_DIAG
    xprintf("bk_split_flash: probe seq=%u -> echo seq=%u magic=0x%02x version=%u status=%u len=%lu crc=0x%08lx diag_ready=%u diag_hms=%u diag_calls=%u\r\n", probe.seq, echo.seq, echo.magic, echo.version, echo.status, (unsigned long)echo.image_len, (unsigned long)echo.image_crc, echo.diag_self_ready, echo.diag_handler_ms, echo.diag_handler_calls);
#else
    xprintf("bk_split_flash: probe seq=%u -> echo seq=%u magic=0x%02x version=%u status=%u len=%lu crc=0x%08lx\r\n", probe.seq, echo.seq, echo.magic, echo.version, echo.status, (unsigned long)echo.image_len, (unsigned long)echo.image_crc);
#endif

    uint8_t decision = bk_split_flash_handshake_decide(echo.version, echo.image_len, echo.image_crc, bk_split_flash_self_len, bk_split_flash_self_crc);
    if (decision == BK_SPLIT_FLASH_DECISION_SKIP_IDENTICAL) {
        xprintf("bk_split_flash: DECISION skip (identical fingerprint len=%lu crc=0x%08lx, nothing to clone) t=%lu rejects=%u\r\n", (unsigned long)echo.image_len, (unsigned long)echo.image_crc, (unsigned long)timer_read32(), (unsigned)bk_split_flash_probe_rejects);
    } else if (decision == BK_SPLIT_FLASH_DECISION_SKIP_VERSION) {
        xprintf("bk_split_flash: DECISION skip (peer protocol version %u != local %u) t=%lu rejects=%u\r\n", echo.version, BK_SPLIT_FLASH_PROTOCOL_VERSION, (unsigned long)timer_read32(), (unsigned)bk_split_flash_probe_rejects);
    } else {
        bk_split_flash_handshake_would_transfer = true;
        xprintf("bk_split_flash: DECISION begin transfer (peer fingerprint differs: len %lu vs %lu, crc 0x%08lx vs 0x%08lx) t=%lu rejects=%u [transfer lands in Phase 3]\r\n", (unsigned long)echo.image_len, (unsigned long)bk_split_flash_self_len, (unsigned long)echo.image_crc, (unsigned long)bk_split_flash_self_crc, (unsigned long)timer_read32(), (unsigned)bk_split_flash_probe_rejects);
    }
    bk_split_flash_handshake_done = true;
}

void keyboard_post_init_bk_split_flash(void) {
    // Compute the own-image fingerprint once at init on both halves: doing it
    // inside the RPC handler on the slave would delay the echo past the
    // master's 20 ms serial timeout and desync the wire (root-caused
    // 2026-11-13 via stage-logging probe).
    bk_split_flash_ensure_self_fingerprint();
    transaction_register_rpc(PUT_BK_SPLIT_FLASH, bk_split_flash_slave_handler);
    xprintf("bk_split_flash: post_init on %s half len=%lu crc=0x%08lx rpc_id=%d\r\n", is_keyboard_master() ? "master" : "slave", (unsigned long)bk_split_flash_self_len, (unsigned long)bk_split_flash_self_crc, (int)PUT_BK_SPLIT_FLASH);
}

void housekeeping_task_bk_split_flash(void) {
#ifdef BK_SPLIT_FLASH_DIAG
    // DIAG (TEMPORARY - 2026-11-13): log transport-connected edges each tick.
    // conn=0 -> conn=1 marks when the peer starts answering normal split syncs,
    // i.e. the end of the ~3 s boot skew.
    bool diag_conn = is_transport_connected();
    if (diag_conn != bk_split_flash_diag_last_conn) {
        bk_split_flash_diag_last_conn = diag_conn;
        xprintf("bk_split_flash: DIAG conn=%u t=%lu\r\n", diag_conn ? 1u : 0u, (unsigned long)timer_read32());
    }
#endif

    // The handshake lives only on the master; the slave answers via the RPC.
    if (!is_keyboard_master()) {
        return;
    }
    if (bk_split_flash_handshake_done) {
        return;
    }
#ifdef BK_SPLIT_FLASH_DIAG
    // DIAG (TEMPORARY - 2026-11-13): H2 test - do not start probing until the
    // secondary has booted and run its post_init (fingerprint), so the first
    // probe cannot land in the slave's transport-up -> post_init window.
    if (!bk_split_flash_handshake_started && timer_read32() < BK_SPLIT_FLASH_DIAG_PROBE_DELAY_MS) {
        return;
    }
#endif
    if (!bk_split_flash_handshake_started) {
        bk_split_flash_handshake_started    = true;
        bk_split_flash_handshake_started_at = timer_read32();
        bk_split_flash_last_probe           = 0;
#ifdef BK_SPLIT_FLASH_DIAG
        xprintf("bk_split_flash: DIAG probing t0=%lu own_len=%lu own_crc=0x%08lx conn=%u\r\n", (unsigned long)bk_split_flash_handshake_started_at, (unsigned long)bk_split_flash_self_len, (unsigned long)bk_split_flash_self_crc, is_transport_connected() ? 1u : 0u);
#endif
        print("bk_split_flash: housekeeping on master half, probing\r\n");
    }
    if (timer_elapsed32(bk_split_flash_handshake_started_at) > SPLIT_FLASH_HANDSHAKE_TIMEOUT) {
        xprintf("bk_split_flash: DECISION skip (no reply within SPLIT_FLASH_HANDSHAKE_TIMEOUT) t=%lu rejects=%u\r\n", (unsigned long)timer_read32(), (unsigned)bk_split_flash_probe_rejects);
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