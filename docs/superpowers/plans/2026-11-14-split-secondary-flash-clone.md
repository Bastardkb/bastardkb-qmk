# Split-secondary flash clone — MVP plan

> **Working contract.** Read `AGENTS.md` first. The QMK facts this relies on live in
> `docs/superpowers/qmk-knowledge-base.md`. This document holds the **objective**, the **architecture**
> (how it works), and the **ordered work steps**. It deliberately contains **no implementation steps** —
> each step is implemented by a separate agent, one step at a time, and each step is validated on hardware
> before the next begins.

## Current status (read this + your step first; pull the rest only on demand)

- **Done: steps 1–5. In progress: step 6 (Harden). Steps 7 (visual indicator) and 8 (speed optimization)
  are scoped, not started.**
- **AMENDED (user, 2026-11-14): the trigger's fixed 5 s first-run delay is replaced by an adaptive
  readiness probe.** The master now prints the `step=5 trigger=1|0` marker immediately on its first
  housekeeping call, and a triggered clone streams as soon as the secondary is able to receive it: the
  `BEGIN` transaction doubles as the probe (the secondary only acks `BEGIN` once its split thread is up and
  its staging slot is erased), retried every `BK_SPLIT_FLASH_PROBE_RETRY_MS` (250 ms) and giving up with a
  `begin-tx` FAIL verdict after `BK_SPLIT_FLASH_PROBE_TIMEOUT_MS` (15 s) if the secondary never appears
  (e.g. a lone master). Each failed probe is a plain `SERIAL_USART_TIMEOUT` round-trip; nothing is logged
  during the wait, so the harness's "one verdict line" contract is unchanged.
- **AMENDED (user, 2026-11-14): step 7's failure state is specified.** After a **triggered** automatic flash
  attempt, if the keyboard detects the two halves still run different firmware versions (the automatic flash
  failed, or never reached the secondary), the master flashes **red** — RGB matrix mode `RGB_MATRIX_BREATHING`,
  speed 255, `HSV_RED`, brightness 150 — held until the mismatch is resolved. See step 7 for the exact
  arming rule (never on a successful commit/skip).
- **DECIDED (user, 2026-11-14): step 6 uses deferred staging preparation.** The master will declare the
  image length with a `prepare(length)` operation; the secondary callback records that length in O(1), its
  housekeeping task erases exactly the required rounded length, and the master polls a ready operation
  before sending `BEGIN`/data. This avoids erasing the full 256 KiB slot on every secondary boot while
  supporting a master image larger than the currently installed secondary image.
- **PASS (user, 2026-11-14): adaptive probe validated on hardware.** Image A produced the immediate
  `trigger=1` → transport → flash → `action=skip` path; Image B (a distinct 1 ms timeout variant used only
  to force a different fingerprint) cloned onto the Image-A secondary with `action=commit`, rebooted the
  secondary, and the keyboard remained usable. The normal 15-second Image A is the new baseline golden:
  `bk_split_flash_probeA.uf2`.
- Last PASS: adaptive-probe amendment, 2026-11-14 — golden `bk_split_flash_probeA.uf2` (in `$QMK_USERSPACE`).
- Next blockers, in order: step 6 must resolve the staging-erase sizing (erase a fixed
  `BK_SPLIT_FLASH_STAGING_MAX`, or a deferred `prepare(length)` + slave-housekeeping erase + master poll)
  before validating a master image newer/larger than the secondary's; step 7's remaining LED states
  (idle / in-progress / success) still need user confirmation before implementing (the failure /
  mismatched-versions state is now specified).
- Harness ground rules: one UF2 per step (`qmk compile -kb bastardkb/dilemma/4x6_4_procyon -km vendor`),
  flash **both** halves, connect only the primary's USB, full console from power-on, exactly one greppable
  `BKCLONE:` verdict line.

## Objective (the MVP)

When a new firmware is written to the **master** (the USB half), the master clones its **own** firmware
image `[XIP_BASE, __flash_binary_end)` to the secondary over the split link on its next boot. The
secondary writes it to flash, verifies it, commits it, and reboots into it. (Original: explicit trigger at
boot; superseded by the automatic EEPROM-fingerprint trigger, decided for work step 5.)

Outside the trigger, the keyboard behaves exactly as stock: **no feature code runs and no boot-time
behaviour changes.**

## Non-goals (explicitly out of MVP scope)

- No automatic fingerprint / handshake / version negotiation with the secondary — the trigger is the master's
  own "new firmware" detection (EEPROM-persisted fingerprint, decided for work step 5).
- No transferring a foreign image (only the master's own image).
- No progress UI beyond a minimal indicator (the LED indicator defined in work step 7).
- No EEPROM / handedness semantics changes.
- No speed tuning in MVP steps 1–7 (the `-km vendor` userspace config currently sets
  `SERIAL_USART_SPEED 625000`; the link is wire-bound at ~62.5 KB/s). Performance work is isolated in
  step 8 and must preserve the transaction timeout and keyboard usability constraints.

## Architecture — how it works

### The link

- Half-duplex serial; only one side transmits at a time; every exchange is **master-initiated**.
- `transport_execute_transaction(id, m2s, m2s_len, s2m, s2m_len)` is the one round-trip primitive
  (`transport.c:104-121`).
- The slave dispatches **any** transaction id `< NUM_TOTAL_TRANSACTIONS` via `split_transaction_table[id]`
  — fixed m2s/s2m sizes, shmem offsets, and a callback — serviced by the split protocol thread
  (`serial_protocol.c:51-94`). **Lengths come from the table, not the wire.**
- Throughput is wire-bound. The current `-km vendor` build runs at **625000 baud** (userspace
  `users/vendor/config.h`, confirmed in the build's `cflags.txt`): ~62.5 KB/s, ~4 ms per 246 B chunk, so a
  ~92 KiB image is ≈ **1.6 s of wire time** (≈ 2–3.5 s including per-chunk flash writes; the pre-erase runs
  at slave boot). The earlier 230400-based ≈4 s estimate applied before the userspace override.

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

- **Automatic on new master firmware (user decision, 2026-11-14; implemented in step 5; supersedes the earlier
  Bootmagic decision).** The master compares its own image fingerprint with the one persisted in its module
  EEPROM datablock; a mismatch starts one clone. No probing of the secondary at boot — the decision is purely
  local to the master.

## Ordered work steps (riskiest / most important first)

Each step = **one build, one hardware assertion**, keyboard stays usable. Track status here.

1. **Transport round-trip at the final chunk size.** Prove a single custom transaction carries a full-size
   chunk (m2s) and returns an ack (s2m) between the two halves, repeatedly. *This is the step the previous
   attempt died on* — it must pass before anything is built on top.
   - **STATUS: DONE (2026-11-14).** Hardware PASS on `bastardkb/dilemma/4x6_4_procyon` (`-km vendor`): 26+
     consecutive `BKCLONE: step=1 seq=<n> m2s_len=240 ack=<n> ok` round-trips, `ack == seq`, `seq`
     incrementing. This UF2 is the **golden known-good**.
   - Confirmed knobs: custom split transaction id `PUT_BK_SPLIT_FLASH` (module `bastardkb/bk_split_flash`),
     registered on **both** halves at `keyboard_post_init`; m2s = 240 B, s2m = 4 B; lengths fixed in the
     `split_transaction_table` entry; reuses the RPC shmem buffers (`RPC_M2S_BUFFER_SIZE 240`); driven from
     the master's `housekeeping_task_bk_split_flash` every 500 ms via one `transport_execute_transaction`
     (no RPC, no flash, no trigger).
2. **Chunk streaming + transport integrity.** Master sends many chunks in sequence, advancing only when the
   slave acks the matching sequence number. The slave accumulates a **rolling CRC over the received bytes**;
   at end of stream the master compares it against its own whole-image CRC. No flash, no RAM buffer — this is
   the old "whole image" checkpoint without the throwaway buffer (bulk transport proven end to end).
   - **STATUS: DONE (2026-11-14).** Hardware PASS on `bastardkb/dilemma/4x6_4_procyon` (`-km vendor`): two
     consecutive `BKCLONE: step=2 chunks=398 bytes=91820 crc=0x48643FE0 slave=0x48643FE0 ok` lines — the
     master's CRC-32 over the bytes it sent equals the slave's rolling CRC-32 over the bytes it received.
     This UF2 (`bk_split_flash_step2.uf2`) is the **golden known-good**.
   - Confirmed format (user decision, 2026-11-14): keep Step 1's 240 B m2s / 4 B s2m; `m2s =
     {seq, offset, crc8, data[231]}`, `s2m = ack`. Control ops use reserved `seq` values: `seq=0` "begin"
     carries the image length in `offset` and resets slave state; a final `seq=0xFFFFFFFF` "report" makes the
     slave return its finalized CRC-32 in `ack`. Ack sentinels: begin `0xFFFFFFFF`, seq error `0`, CRC-8 error
     `0xFFFFFFFE`; a good data chunk is acked with its own `seq`.
   - Cadence (user decision, 2026-11-14): the master streams the whole image **once ~5 s after boot and then
     every 20 s** (each run prints one marker), so the ~3 s split-boot skew is cleared. No trigger, no flash,
     no RAM image buffer; the slave keeps only a rolling CRC-32 plus a few words of state. One transaction per
     `housekeeping_task` call, so the keyboard stays usable.
3. **Write to staging flash.** Slave writes each chunk into the staging slot (erase staging first) and the
   result is verified by read-back / CRC. Transport vs. flash integrity are reported by **distinct markers**,
   so a failure here is unambiguously a flash bug.
   - **STATUS: DONE (2026-11-14).** Hardware PASS on `bastardkb/dilemma/4x6_4_procyon` (`-km vendor`): two
     consecutive runs each printed `BKCLONE: step=3 transport chunks=402 bytes=92648 crc=0x289E6CF8
     slave=0x289E6CF8 ok` **and** `BKCLONE: step=3 flash bytes=92648 crc=0x289E6CF8 readback=0x289E6CF8 ok`.
     This UF2 (`bk_split_flash_step3.uf2`) is the **golden known-good**.
   - Confirmed knobs: staging slot at flash offset `0x100000` (1 MiB), erased once on the secondary at
     `keyboard_post_init` (a 4 KiB-sector erase can't run inside a transaction — IRQs off stalls the split
     thread past the 20 ms read timeout, so it runs ahead of the stream). New `seq` control ops on top of
     step 2: `VERIFY=0xFFFFFFFE` returns a CRC-32 of the staging **read-back**; `ack LEN_ERR=0xFFFFFFFD`
     rejects a begin whose length exceeds the erased slot. The slot is verified **per 256 B page as it is
     written** (read each page back, fold into a running CRC-32) so the end-of-stream VERIFY op is O(1) and
     cannot time out — a whole-slot read-back in one callback exceeded the 20 ms budget. No commit, no
     reboot, no trigger; keyboard stayed usable. Each run prints **two** distinct markers (transport + flash),
     which is the step-3 intent even though the harness's "exactly one marker line" rule is written for the
     single-verdict steps.
4. **Commit + reboot.** Verify, then commit (erase active, copy, boot2 last), then reboot the secondary
   into the new image.
   - **Verify must include bootability, not just CRC-32.** Step 3 proved the staged bytes equal the sent
     bytes; committing them erases the active region and sector 0, so a CRC-valid but **unbootable** image
     would brick the secondary. Before erasing anything, also check the **RP2040 boot2 checksum**
     (`bitrev32(crc_reg(bitrev8(boot2[0:252])))`, see the KB) and **vector-table plausibility** (initial `sp`
     at image+0x100, reset vector at image+0x104). Optional strengthening: fold the boot2 checksum into
     step 3's `flash` marker so "flash ok" already means byte-exact **and** bootable.
   - **Commit mechanics:** the whole erase+copy is one interrupts-off, **RAM-resident** routine
     (`.time_critical.*`, no calls into flash — no `memcpy`/print) because VTOR points at flash sector 0;
     erase **sector 0 only inside the commit**; write **boot2 last**. Never erase the wear-leveling EEPROM
     (`0x1FE000`) or the staging slot.
   - **Re-clone guard (required for "keyboard stays usable"):** skip the commit when the staged image already
     matches the slave's active image — otherwise the current 20 s cadence would commit+reboot every 20 s and
     the secondary would never be usable. Needs the slave to CRC its **active** region for the comparison.
   - **Step-4 validation one-shot (user decision, 2026-11-14).** With both halves flashed from the same UF2 the
     staged image equals the active image, so the guard would skip forever and the commit path would never run
     on hardware. For this step only, the master forces **exactly one** commit on its first run after boot:
     `COMMIT.offset != 0` bypasses the equality guard, while the read-back and bootability checks still apply.
     Afterwards the guard takes over. This is temporary — step 5 ties the force to the Bootmagic trigger
     instead of "first run after boot", so outside the trigger no re-flash happens.
   - **Confirmed knobs:** the guard's active CRC is accumulated **per 256 B page while staging** (alongside
     the read-back CRC), so COMMIT stays O(1). The COMMIT callback runs the boot2/vector checks and the guard
     and only **schedules**; the slave's `housekeeping_task` runs the RAM-resident commit after a 250 ms delay
     (enough for the ack to leave first). The commit calls `flash_range_erase/program` through **volatile
     function pointers**: a direct RAM→RAM call needs a long-branch veneer that the linker places in **flash**,
     and erasing sector 0 would destroy it (see the KB). The commit erases `round_up(image_len, 4096)`
     (93692 B → 94208 B, 23 sectors) at `0..0x17000`, clear of staging (0x100000) and the EEPROM (0x1FE000);
     it programs pages 1..N-1, then page 0 (boot2) last, then resets via AIRCR `SYSRESETREQ`.
   - **Reboot + recovery:** clean system reset after the commit; the master must tolerate the secondary
     dropping off the link and reappearing (and must not immediately re-trigger): after the COMMIT verdict the
     master re-arms its next run 20 s later. A mid-commit power loss leaves no valid vectors — the handoff
     must document the BOOTSEL recovery (hold L00 / double-tap).
   - **Assertion:** both halves boot the new image, the link recovers, the keyboard types, and the secondary
     does **not** re-commit on the next cadence (first run `action=commit`, later runs `action=skip`).
   - **STATUS: DONE (2026-11-14).** Hardware PASS on `bastardkb/dilemma/4x6_4_procyon` (`-km vendor`). Run 1:
     `BKCLONE: step=3 transport chunks=406 bytes=93692 crc=0x02C06F09 slave=0x02C06F09 ok`,
     `BKCLONE: step=3 flash bytes=93692 crc=0x02C06F09 readback=0x02C06F09 ok`,
     `BKCLONE: step=4 commit bytes=93692 boot2=0xD58F0B07 action=commit ok` — the secondary erased, copied,
     wrote boot2 last and reset. Run 2 (next cadence, after the secondary reappeared):
     `step=3 transport … ok`, `step=3 flash … ok`, `step=4 commit bytes=93692 boot2=0xD58F0B07 action=skip ok`
     — the link recovered and the re-clone guard skipped. This UF2 (`bk_split_flash_step4.uf2`) is the **golden
     known-good**. (The whole-image CRC is build-specific because `QMK_BUILDDATE` is embedded; the check is
     that master/slave/readback agree.)
5. **Trigger.** Wire the trigger to start the whole flow.
   - **STATUS: DONE (2026-11-14).** Hardware PASS on `bastardkb/dilemma/4x6_4_procyon` (`-km vendor`), two
     runs, golden `bk_split_flash_step5.uf2`:
     - Run 1 (both halves on the same step-5 build): `BKCLONE: step=5 trigger=1 crc=0x80B467E2 ok` →
       `step=3 transport chunks=408 bytes=94096` → `step=3 flash … readback=0x80B467E2 ok` →
       `step=4 commit bytes=94096 … action=skip ok`. The trigger fired on a fresh master (datablock mismatch),
       the guard correctly skipped the identical image, and the one-shot stream never re-ran.
     - Run 2 (crossing builds: secondary on step-5 build A, master on step-5 build B — same source, different
       build timestamp ⇒ different CRC): `action=commit` — the secondary committed, erased its whole EEPROM
       inside the commit, and rebooted into the master's build. Keyboard typed after reboot.
   - **Post-commit red flashing (follow-up, not a clone bug):** both halves flashed red after the crossing
     build commit. Cause: the userspace **Argos** module's build-id sync (`argos.c`), which compares the two
     halves' build IDs (`__DATE__`/`__TIME__` compile constants) and drives a red breathing RGB error state
     whenever they differ — expected in the crossing-build test by construction. It self-resolves on reboot
     (the sync re-runs on each boot). The user then decided to move that mismatch indication out of argos
     into bk_split_flash (see step 7) — this also removes a UX trap: a build-id red flash after a *clone*
     would make the user think the clone failed.
   - **SUPERSEDED (user, 2026-11-14): automatic "new firmware on master" detection, not Bootmagic.** The
     earlier Bootmagic-key decision below is replaced: whenever a new firmware is written to the master, the
     master clones it to the secondary on its next boot, with no separate key/mechanism.
   - **Implemented knobs:** the module declares a 4-byte EEPROM datablock
     (`EECONFIG_MODULE_BK_SPLIT_FLASH_DATA_SIZE 4`) holding the master's fingerprint = whole-image CRC-32 of
     `[XIP_BASE, __flash_binary_end)` (build-specific, since `QMK_BUILDDATE` is embedded). On the master's first
     `housekeeping_task` it compares the stored fingerprint with its own; mismatch (new image, or a fresh zeroed
      datablock) ⇒ trigger. It prints exactly one `BKCLONE: step=5 trigger=1|0`
      marker on the first master housekeeping call; only `trigger=1` runs the stream, **once** (no 20 s
      cadence; `BK_SPLIT_FLASH_RUN_PERIOD_MS` removed), **starting as soon as the secondary is online**: the
      `BEGIN` transaction doubles as the readiness probe (the secondary only acks `BEGIN` once booted with
      its staging slot erased), retried every 250 ms and given up after 15 s if the secondary never appears
      (`BK_SPLIT_FLASH_FIRST_RUN_DELAY_MS` removed; see the AMENDED note in the status header). The step-4
      commit force is removed: the re-clone guard decides (new image ⇒ `commit`, identical ⇒
     `skip`). The fingerprint is persisted only on a `commit`/`skip` verdict, so a failed clone re-triggers on
     the next boot.
   - **Secondary EEPROM reset (user, 2026-11-14):** the commit also erases the secondary's whole wear-leveling
     EEPROM region (`WEAR_LEVELING_RP2040_FLASH_BASE`, `WEAR_LEVELING_BACKING_SIZE` = 0x1FE000, 8 KiB) inside the
     RAM-resident commit, right after erasing the active region and before the copy, so the cloned secondary
     boots like a freshly flashed half (eeconfig re-initialised).
   - **Bootstrap caveat:** the commit is executed by the secondary's *currently running* firmware, so the EEPROM
     wipe only takes effect once the secondary already runs a build that contains it. A first clone onto an
     old-firmware secondary runs the old commit (no wipe). Validated by cloning between two builds of the same
     step-5 source (differ only in build timestamp ⇒ different CRC): secondary runs build A (erase-capable),
     master runs build B ⇒ `action=commit` and the wipe runs.
   - **Staging-size note:** step-5 image is 94096 B (408 chunks); a step-4 secondary erased
     `round_up(93692, 4096)` = 94208 B,
     so cloning step-5 onto a step-4 secondary fits by luck of sector rounding. The general fix remains step 6.
   - (Original, superseded) **DECIDED (user, 2026-11-14): Bootmagic on the master.** A key held at master boot starts the clone and
     **auto-forces** the commit (replacing step 4's "first run after boot" force). The 20 s cadence and the
     unconditional first-run force exist only for step-4 validation; here they are gated by the trigger, so
     outside it no re-flash happens. Open detail: L00 already enters BOOTSEL on this board (KB), so choose a
     non-conflicting trigger key/combo or run the check after Bootmagic handling. A deferred wait is still
     needed for the ~3 s master→secondary boot skew.
6. **Harden.** Retries, timeout/disconnect handling, erase/busy handling, minimal progress indication through console.
   - **Staging-erase sizing (gates step 5).** Step 3 erases the staging slot from the **secondary's own**
     image length and rejects a longer `BEGIN`. That only holds while both halves run the same UF2 — but the
     trigger's whole point is "master pushes its image **after the master is updated**", so the master's image
     can be **newer/larger** than the secondary's, giving `LEN_ERR` and no clone. **Decision:** size the
     staging region from the **master's** declared length with a `prepare(length)` op whose callback only
     records the length (O(1)); erase the rounded length in the slave `housekeeping_task`, and have the master
     poll a ready op before `BEGIN`/data (the async pattern `AGENTS.md` mandates). This avoids the fixed
     `BK_SPLIT_FLASH_STAGING_MAX` boot erase (~256 KiB ≈ 2.9 s) and must be resolved before the automatic
     trigger (step 5) is validated with a master image that differs from the secondary's.

7. **Visual indicator (LED).** Give the user an on-keyboard indication of a running master→secondary clone,
   using the keyboard's addressable LEDs (RGB matrix / underglow) — this is the "minimal indicator" carved
   out in Non-goals. Today the only feedback is the console marker line, which needs `qmk console` attached.
   - **Where: master side only.** The secondary must not drive LEDs from the transfer path: its flash write
     runs in the split protocol thread, and it reboots at commit (its LEDs are wiped mid-flow). The master
     drives the flow from `housekeeping_task_bk_split_flash`, so the indication lives there.
   - **Hardware:** the procyon has `rgb_matrix` (ws2812 on GP10, `split_count: [52, 52]`) — see
     `keyboards/bastardkb/dilemma/4x6_4_procyon/keyboard.json`. Use the `rgb_matrix` indicator API rather
     than poking the driver buffers directly.
   - **States:** idle = unchanged stock animations (the Objective still holds: no behaviour outside the
     trigger); in progress = a distinct animated colour for the stream duration (~5–7 s — **still to be
     confirmed**); success = brief confirmation (**still to be confirmed**); **failure / mismatched
     versions (user spec, 2026-11-14)** = after a **triggered** clone attempt, when the two halves still
     run different firmware versions — the automatic flash failed (transport/flash/boot/commit fail, or
     the 15 s probe give-up: secondary never online) — the master shows **red breathing**: RGB matrix mode
     `RGB_MATRIX_BREATHING`, speed **255**, colour **`HSV_RED`**, brightness **150**, held until the
     mismatch is gone (a later successful clone, or both halves re-flashed).
   - **Assertion:** flash both halves, trigger the flow, and confirm the keyboard shows the in-progress
      indication for the duration, then the success indication at completion, then returns to normal.
      Print one marker (`BKCLONE: step=7 …`) alongside the visual state. **Failure assertion:** flash a new
      build onto **only the primary** (secondary off/detached so the probe gives up at 15 s), and confirm
      the master then shows the red breathing state (mode `RGB_MATRIX_BREATHING`, speed 255, `HSV_RED`,
      brightness 150) and keeps it until a successful clone or re-flash — and confirm a successful
      `action=commit`/`skip` run shows **no** red.
   - **User decision (2026-11-14, from step 5): move the red "firmware id desync" flashing out of argos into
     bk_split_flash.** The userspace argos module (`argos.c`) currently drives a red breathing RGB error
     state whenever the two halves' build IDs differ (`__DATE__`/`__TIME__` compile constants, not EEPROM
     content), gated by `rgb_matrix_indicators_advanced_argos` → `argos_sides_have_different_firmware_id()`.
      That code moves into bk_split_flash (it owns the clone's semantics), and argos loses it. The build-id
      sync itself (RPC_ID_BUILD_ID_SYNC, key blocking) stays in argos. This also removes the step-5 UX trap:
      after a successful clone the argos red flash looked like a clone failure.
   - **Arming rule — red only when a clone attempt left the halves mismatched (user, 2026-11-14).** The
     moved indicator is gated by the **clone outcome**, not by any build-id difference. A **triggered**
     clone attempt that ends in failure — any `FAIL reason=…` verdict, or the 15 s probe give-up (secondary
     never online) — arms the state; the firmware-version comparison (the check that moves out of argos; the
     sync itself stays in argos) then decides whether the halves actually still differ ("secondary never
     responded" counts as a failed flash). A **successful** attempt — `action=commit` (secondary reboots into
     the master's image) or `action=skip` (already identical) — never shows red, so the few seconds of
     mismatch while the secondary reboots after a commit cannot re-create the step-5 UX trap. Driven from
     the master's `housekeeping_task_bk_split_flash` via the `rgb_matrix` indicator API, like the other
     states, with the exact parameters from the States bullet.

8. **Speed optimization.** Measure and reduce the end-to-end clone duration and time-to-first-transfer after
   the secondary becomes available, without changing the verified clone safety model or making the keyboard
   unusable.
   - **Scope:** optimize only after step 6 reliability and step 7 indication are validated; keep the
     adaptive readiness probe's immediate-start behaviour as the startup baseline.
   - **Candidates:** reduce avoidable per-transaction overhead, improve safe chunk/housekeeping cadence,
     minimize staging/page-program overhead where measurements justify it, and evaluate a higher serial baud
     rate only if both halves remain reliable at that setting.
   - **Constraints:** every slave callback remains O(1) and comfortably inside `SERIAL_USART_TIMEOUT`; no
     long erase/read-back work moves into the transaction callback; preserve packet/read-back/bootability
     checks, link recovery, and normal keyboard responsiveness.
   - **Measurement:** report image size, chunk count, time from power-on to first accepted `BEGIN`, total
     transfer time, total clone time, and keyboard responsiveness. Compare against the validated step-6/7
     baseline and keep one firmware artefact per optimization step.
   - **Assertion:** the optimized build is faster by a measured amount, still passes the complete clone and
     recovery flow, and does not regress the readiness-probe or safety verdicts.

## Harness / test protocol

- One UF2 per step: `qmk compile -kb bastardkb/dilemma/4x6_4_procyon -km vendor`.
- Each step prints **exactly one machine-greppable verdict line** (e.g. `BKCLONE: step=1 chunks=1 crc=0x… ok`).
  Steps 3–4 also print the prerequisite `step=3 transport` / `step=3 flash` lines as evidence; the new verdict
  is the `step=4 commit … action=commit|skip` line.
- The human flashes **both** halves, connects **only the primary's** USB, power-cycles, captures the full
  console from power-on for the needed window, and pastes it back.
- Keep a **golden UF2** (last known-good). After every flash, confirm the keyboard still boots and types.
  `qmk compile` copies its output over `<QMK_HOME>/<name>.uf2` **and** `<QMK_USERSPACE>/<name>.uf2`, so an
  earlier golden kept at either of those paths is overwritten by the next build — save goldens under a
  distinct filename (e.g. `bk_split_flash_step<N>.uf2`); that is also why the Step 1 golden copy at the
  repo root / userspace root no longer exists. Goldens live in `$QMK_USERSPACE`
  (`/home/qlebastard/qmk_userspace_bstiq/`). Current golden: `bk_split_flash_probeA.uf2` (adaptive-probe
  hardware PASS, 2026-11-14). `bk_split_flash_probeB.uf2` was a temporary 15.001-second timeout variant used
  only to force a distinct fingerprint for the commit test; it is not the baseline. Earlier goldens remain
  `bk_split_flash_step5.uf2` and `bk_split_flash_step4.uf2`.
- If a step regresses an earlier one, **bisect before adding anything** (see `AGENTS.md`).

## Dead-ends (do not repeat)

- **The 4-phase RPC for bulk/handshake.** `transaction_rpc_exec`'s response phase (`GET_RPC_RESP_DATA`)
  stopped being delivered as the payload grew and desynced the half-duplex link within the window. The
  request phases succeeded; the response never came back even with a 200 ms read window; delaying the first
  probe to 6 s (after the secondary's post_init) did not help; the same *minimal* round-trip worked. Do not
  build the transfer on the RPC.
- **Boot-time auto-handshake / fingerprint detection over the link.** Probing the secondary on every boot
  risks the link and the boot path. The shipped trigger is different: the master compares its **local**
  EEPROM-stored fingerprint with its own image CRC — no link probing, no boot-time behaviour change outside
  the trigger — so it is safe. Keep the trigger local; do not reintroduce boot-time probing.
- **Changing boot behaviour and the transport in the same step.** The previous failure bundled a payload
  change, a CRC, a policy, and boot-time behaviour — impossible to bisect.
ct.
