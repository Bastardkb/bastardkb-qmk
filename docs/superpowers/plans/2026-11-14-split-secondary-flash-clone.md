# Split-secondary flash clone — MVP plan

> **Working contract.** Read `AGENTS.md` first. The QMK facts this relies on live in
> `docs/superpowers/qmk-knowledge-base.md`. This document holds the **objective**, the **architecture**
> (how it works), and the **ordered work steps**. It deliberately contains **no implementation steps** —
> each step is implemented by a separate agent, one step at a time, and each step is validated on hardware
> before the next begins.

## Objective (the MVP)

On an **explicit trigger at boot on the master** (the USB half), the master copies its **own** firmware
image `[XIP_BASE, __flash_binary_end)` to the secondary over the split link. The secondary writes it to
flash, verifies it, commits it, and reboots into it.

Outside the trigger, the keyboard behaves exactly as stock: **no feature code runs and no boot-time
behaviour changes.**

## Non-goals (explicitly out of MVP scope)

- No automatic fingerprint / handshake / version negotiation — the trigger is explicit.
- No transferring a foreign image (only the master's own image).
- No progress UI beyond a minimal indicator.
- No EEPROM / handedness semantics changes.
- No baud tuning (stay at 230400).

## Architecture — how it works

### The link

- Half-duplex serial; only one side transmits at a time; every exchange is **master-initiated**.
- `transport_execute_transaction(id, m2s, m2s_len, s2m, s2m_len)` is the one round-trip primitive
  (`transport.c:104-121`).
- The slave dispatches **any** transaction id `< NUM_TOTAL_TRANSACTIONS` via `split_transaction_table[id]`
  — fixed m2s/s2m sizes, shmem offsets, and a callback — serviced by the split protocol thread
  (`serial_protocol.c:51-94`). **Lengths come from the table, not the wire.**
- Throughput is wire-bound: 230400 baud ≈ 23 KB/s, so a ~92 KiB image is ≈ **4 s of wire time** (≈ 5–7 s
  including per-chunk flash writes and the pre-erase).

### Transport choice: one transaction per chunk — NOT the 4-phase RPC

- The module declares a transaction id and, at init on **both** halves, registers a table entry with fixed
  m2s/s2m sizes and a slave callback, reusing the shmem RPC buffers (a module has no buffer of its own).
- Master: one `transport_execute_transaction` per chunk (`m2s = {seq, offset, crc8, data}`,
  `s2m = ack`). Slave: the callback writes the page to flash and fills the ack.
- Rationale: this is the **same machinery QMK's own matrix sync uses every scan** — proven, fixed sizes,
  no info block, no 4-phase timing. (The RPC failed; see Dead-ends.)
- The slave callback runs in the split thread, so a flash write **blocks the link**. Keep per-chunk writes
  fast (a 256 B page program is ~1–5 ms, well under the 20 ms read timeout) and handle **erase** specially
  (a 4 KiB sector erase ≈ 45 ms would blow the timeout — do it ahead of the stream, or as an op the master
  tolerates).

### Flash write (safety)

- **staging slot** (clear of the wear-leveling EEPROM region) → **verify** (whole-image CRC-32 + RP2040 boot2
  checksum + vector-table plausibility) → **commit** (erase active region, copy staging → active, write
  **boot2 last**) → **reboot**.
- The commit runs interrupts-off and RAM-resident; XIP is off while the active region is erased; sector 0
  is only erased inside the commit. (See the knowledge base's "RP2040 flash self-write" facts.)

### The trigger

- Explicit (a key held at boot, or Bootmagic) — **never** a boot-time auto-detect. Mind the dilemma
  Bootmagic L00 caveat in the knowledge base.

## Ordered work steps (riskiest / most important first)

Each step = **one build, one hardware assertion**, keyboard stays usable. Track status here.

1. **Transport round-trip at the final chunk size.** Prove a single custom transaction carries a full-size
   chunk (m2s) and returns an ack (s2m) between the two halves, repeatedly. *This is the step the previous
   attempt died on* — it must pass before anything is built on top.
2. **Chunk streaming.** Master sends many chunks in sequence, advancing only when the slave acks the
   matching sequence number; the slave acks each. No flash yet.
3. **Whole-image transfer into RAM.** Master streams the entire image; the slave holds it in a RAM buffer;
   the master reads back a checksum over the whole thing. Proves bulk integrity end to end.
4. **Write to staging flash.** Slave writes each chunk into the staging slot (erase staging first) and the
   result is verified by read-back / CRC.
5. **Commit + reboot.** Verify, then commit (erase active, copy, boot2 last), then reboot the secondary
   into the new image.
6. **Trigger.** Wire the explicit trigger to start the whole flow.
7. **Harden.** Retries, timeout/disconnect handling, erase/busy handling, minimal progress indication.

## Harness / test protocol

- One UF2 per step: `qmk compile -kb bastardkb/dilemma/4x6_4_procyon -km vendor`.
- Each step prints **exactly one machine-greppable marker line** (e.g. `BKCLONE: step=1 chunks=1 crc=0x… ok`).
- The human flashes **both** halves, connects **only the primary's** USB, power-cycles, captures the full
  console from power-on for the needed window, and pastes it back.
- Keep a **golden UF2** (last known-good). After every flash, confirm the keyboard still boots and types.
- If a step regresses an earlier one, **bisect before adding anything** (see `AGENTS.md`).

## Dead-ends (do not repeat)

- **The 4-phase RPC for bulk/handshake.** `transaction_rpc_exec`'s response phase (`GET_RPC_RESP_DATA`)
  stopped being delivered as the payload grew and desynced the half-duplex link within the window. The
  request phases succeeded; the response never came back even with a 200 ms read window; delaying the first
  probe to 6 s (after the secondary's post_init) did not help; the same *minimal* round-trip worked. Do not
  build the transfer on the RPC.
- **Boot-time auto-handshake / fingerprint detection.** Probing on every boot risks the link and the boot
  path; use an explicit trigger.
- **Changing boot behaviour and the transport in the same step.** The previous failure bundled a payload
  change, a CRC, a policy, and boot-time behaviour — impossible to bisect.
