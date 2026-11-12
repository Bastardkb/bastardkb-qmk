# Split Secondary Firmware Clone — Design

- **Date:** 2026-11-12
- **Status:** Approved by the human partner on 2026-11-12. Design doc only; nothing built from it yet. Extended on 2026-11-12 with a phased, human-in-the-loop build & validation workflow (Phase 0–5); still nothing built.
- **Size:** Project-sized (a new opt-in module with a safety-critical write path), not a small change.
- **Skill:** `skills/brainstorm.md` (approaches laid out and picked in earlier conversation; this is the written design).
- **Implementation plan:** `docs/superpowers/plans/2026-11-12-split-secondary-flash-implementation.md`
- **Module location (provisional):** `module/bastardkb/bk_split_flash` in this local folder; the human partner may move it (e.g. into external userspace) later.
- **Target board:** `bastardkb/dilemma/4x6_4/procyon` (all hardware validation happens here).
- **Build strategy:** a standalone QMK Community Module — enabled by listing it in `keymap.json`, inert otherwise — with no edits to QMK core (the plan maps every former core edit to a module hook).
- **Workflow:** phased; each phase emits exactly one UF2 and ends at a human validation gate. No phase's coding starts until the previous gate is validated by the human partner.

## Intent and why

Flashing a split RP2040 keyboard today means doing the reset-and-drop dance on each half separately. We want **one USB cable and one dropped UF2 file** to leave both halves running the new firmware. Because these boards have no `EE_HANDS` (handedness comes from `SPLIT_HAND_PIN`), a single image is valid for both halves, so the primary can copy its own image to the secondary.

The inferred reason is removing the second-half ritual, not adding a new flashing tool. Correct this if the why is different.

## Shape corrections made during the conversation

- **"Trigger boot mode in the secondary" was dropped** (human call: "oopsie, just go with the initial plan from D"). The secondary stays running its own firmware the whole time; that firmware receives the bytes and writes them. The un-erasable ROM fallback is only *armed* as a safety net, never entered in the normal path.
- **"Erase secondary memory" moves to last, not first.** The safe parking spot means the old firmware stays bootable until the new image is fully staged and verified; only then is there one brief commit.
- **"Use all available RAM / big packages" is reinterpreted.** The half-duplex wire is the bottleneck, not RAM, and the running firmware needs its RAM. Speed comes from the largest *reliable* chunk and, separately, a faster link setting.
- **"Clear local eeprom" became "wipe both halves"** (human call), recorded below.

## Approaches considered

| | What it is | Why not / why yes |
|---|---|---|
| A — trigger BOOTSEL on both halves, host writes both drives | Both halves become USB drives | Ruled out: with only the primary on USB, the secondary's drive is invisible |
| B — live courier (C) | PC streams the image through an awake primary | Ruled out: needs a custom host tool and a second phase to flash the primary |
| C — **chosen (D)** | Primary is flashed normally by the dropped UF2; on its next boot it copies its own image to the secondary over the split wire | No host changes, least data, one cable, stock tooling works |

Picked by the human partner across earlier rounds. Not reopened.

## Goals, non-goals, anti-goals

- **Goals:** one cable, one UF2 drop, both halves updated; no separate script; isolated module; capability-gated; unbrickable; danger window as close to zero as possible; as simple as possible.
- **Non-goals:** automatically converting old secondaries (they are skipped); a custom host tool; per-half different images; flashing the secondary over its own USB.
- **Anti-goals** (failure even if it technically works): a half stuck unrecoverably; wiping settings silently without the user knowing; a keyboard that appears to hang for half a minute; re-cloning on every boot; writing a truncated or garbage image.

## Decisions — the human partner's

1. The secondary never enters ROM boot mode; it stays in firmware and self-writes.
2. On a successful update, **both halves clear their settings area and reboot** (a full runtime-settings reset on every update; keymaps live in flash and are unaffected; handedness is pin-based and unaffected).
3. Existing boards with old secondary firmware get a **one-time manual seed flash** of the secondary; later updates are automatic.

## Decisions — the agent's, with reasons (override any of these)

1. **Module boundary:** a standalone QMK Community Module (opt-in by listing it in `keymap.json`; completely inert unless enabled) with three internal seams — trigger, transport, action — and a HAL boundary between transport and action so the two can be built and tested in parallel (the action is the only hardware-bound part). Zero edits to QMK core: this fork's Community Module hooks (`SPLIT_TRANSACTION_IDS_MODULE_*`, RPC registration, `housekeeping_task_<module>`, auto-included `config.h`/`rules.mk`) already cover the integration points. The normal flash path is untouched. *Reason:* isolation was an explicit objective; a standalone module additionally keeps this fork divergenceless from upstream QMK and lets any other split board opt in.
2. **Trigger fires on the primary's first boot after it was updated, not during bootmagic.** *Reason:* during bootmagic the primary is already in ROM boot mode and cannot run our code.
3. **Capability + fingerprint handshake, no sticky "just flashed" note.** The primary asks the secondary whether it supports the protocol and whether its image differs; no answer, wrong version, or identical fingerprint → skip. *Reason:* addresses trigger fragility, the self-replication loop con, and the "only if supported" objective in one mechanism; it also self-heals when a previous attempt was interrupted.
4. **Reuse existing QMK split messaging and the existing QMK boot-load function** rather than inventing a new protocol or USB interface. *Reason:* "SIMPLE" and "use QMK mechanisms you mentioned" were explicit; this is also why no separate script is needed.
5. **Chunked, numbered, acknowledged transfer; whole image verified before commit; bounded timeout and clean abort.** *Reason:* the reliability objective; bounded behaviour also addresses the "appears to hang" anti-goal.
6. **Speed policy:** largest chunk the existing messaging comfortably carries; treat the serial link as the limit; never buffer the whole image in RAM. A faster link setting is a separate, later, optional decision. *Reason:* the wire dominates; this is the only speed lever that does not fight reliability or simplicity.
7. **Copy only the firmware region, stopping at the image boundary — not the whole flash — and keep each half's settings area out of the copy.** *Reason:* addresses "must know exactly what to copy"; with one shared image this also keeps both halves valid.
8. **Safety sequence (unbrickable, danger window ~0):** arm the ROM fallback → stage the new image in a safe parking spot → verify the whole image → commit in one brief step → write a "valid" marker last → reboot. *Reason:* any interruption before the commit leaves the old firmware bootable; any interruption after leaves the chip in its un-erasable recoverable state.
9. **Pause the split watchdog during the transfer and commit.** *Reason:* otherwise it would reset the secondary mid-write.
10. **Recovery path documented:** double-tap / manual UF2 on the secondary remains the escape hatch. It needs a USB cable to that half once — an accepted consequence of the one-cable topology, not a hidden failure.
11. **No host-side tooling.** *Reason:* the clone triggers on the *result* of a flash, not on the tool, so stock flash commands and a manual UF2 drag both work.
12. **The primary tolerates the secondary briefly disappearing** at the final reboot, and the module never blocks the primary's own boot.

## How each of D's cons is addressed

| Con | High-level mitigation |
|---|---|
| One-time seed flash needed | Accepted by the human partner; capability handshake skips unsupported secondaries gracefully |
| "Just flashed" trigger fragility | Fingerprint/version comparison on every boot, no RAM sticky note |
| Knowing exactly what to copy | Copy only the firmware region; stop at the image boundary; exclude settings area |
| Identical images / version coupling | Desired here — one image valid for both halves, no `EE_HANDS` |
| Re-cloning forever | One-shot guard plus "skip when identical" |
| Bricking / power loss | Arm fallback → stage → verify → brief commit → valid marker last |
| Silent 10–30 s freeze | Bounded window, visible progress indication, never block the primary's boot |
| Watchdog resetting mid-write | Paused for the duration |
| UX of the secondary rebooting | Primary tolerates the brief disappearance |

## Phases (order only, high level)

0. **Walking skeleton / verify the foundation first** (added 2026-11-12). A barebones Community Module image that compiles, registers one RPC transaction ID, and round-trips a capability probe + echo between the halves on the dilemma 4x6 procyon — with no flash writes. Its job is to retire the architectural caveats (transaction-ID budget, RPC buffer sizing, module discovery/enablement, housekeeping on both halves, watchdog hooks, Community Module API version) before any safety-critical code exists.
1. **Safety core first** — staging, verification, brief commit, recovery — behind a HAL so its logic is host-testable; proven on one board (power-cut matrix) before anything else exists.
2. **Capability + fingerprint handshake and skip logic.**
3. **Chunked acknowledged transfer** (plus the watchdog pause and bounded progress).
4. **Module packaging:** finalize knobs/asserts, README, seed-flash and recovery docs, and the flag-off = stock check.
5. **Optional speed tuning last**, only after correctness is established.

## Build & validation workflow (added 2026-11-12)

- The implementation is a standalone QMK Community Module, so "packaging" is not a single late phase: a minimal scaffold (`qmk_module.json`, `config.h`, `rules.mk`) exists from Phase 0, and Phase 4 finalizes it. The seams (trigger / transport / action) let the hardware-bound flash writer and the host-testable protocol/state-machine be developed in parallel.
- **One UF2 per phase.** Every phase produces exactly one self-contained UF2 for `bastardkb/dilemma/4x6_4/procyon` (any temporary test harness — a debug keycode or forced trigger — lives inside that image), so validation needs a single image and a single flash.
- **A validation gate ends every phase.** Because flashing and power-cut testing need the physical board, the agent hands the human partner the UF2 plus an exact checklist (cable setup, seed state, the flash command, the console/LED/BOOTSEL observations to make, and what to report back) and does **not** begin the next phase until the human partner reports validation. The precise per-gate contents live in the implementation plan.
- **No coding yet.** These docs are planning only; implementation starts only after the revised plan is approved and then proceeds gate-by-gate.

Read back as a builder would; the only items that genuinely needed the human partner were the three questions, answered in the conversation. Everything above marked "the agent's" is a call, not something agreed to.

## Left to the builder

Chunk size policy, where the parking spot lives, how the fingerprint is derived, how long to wait before skipping, whether to expose a "force re-clone" escape, and which board enables the flag first — the board is now fixed to the dilemma 4x6 procyon. The architectural caveats (transaction-ID budget, RPC buffer sizing, module discovery, watchdog hooks) are retired by the Phase 0 walking skeleton rather than left open.

## Approval gate

Per `skills/brainstorm.md`, nothing gets built until this design doc is approved. It was approved on 2026-11-12. The next step is the separate detailed implementation plan in `docs/superpowers/plans/`, now structured as Phase 0–5 with one UF2 and one human validation gate per phase. Approval of the revised plan is required before Phase 0 coding; each subsequent phase waits on its validation gate. For now these documents are planning only — no code is written.
