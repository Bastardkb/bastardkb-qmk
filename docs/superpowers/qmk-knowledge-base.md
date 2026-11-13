# QMK Knowledge Base

Researched findings about how this QMK tree works, so future phases check here **before** re-researching.
When you learn something new about QMK/this repo, add it here.

Date: 2026-11-12 (Phase 0 of the split-flash clone project).

---

## Community modules

### Discovery and paths

- Loader: `lib/python/qmk/community_modules.py:find_available_module_paths()`.
- Search order: `<QMK_USERSPACE>/modules` **first**, then `<QMK_FIRMWARE>/modules`. Userspace shadows the
  firmware copy of the same module name.
- A module directory is anything under `modules/` containing `qmk_module.json`. Wire modules with a
  **plural** `modules/` folder name (the design doc typo wrote `module/` singular — that path is NOT searched).
- Enablement: list the module in the build's `keymap.json`:
  ```json
  { "modules": ["bastardkb/bk_split_flash"] }
  ```
  A module is inert until listed (`flag-off = stock build`). If the keymap is a C keymap (no `keymap.json`),
  the module can still be listed in the userspace `keymap.json`.

### Module file auto-inclusion

- `qmk_module.json` — required keys: `module_name`, `maintainer`; optional `license`, `url`, `keycodes`, `features`.
- `config.h` in the module folder is auto-appended to `CONFIG_H` BEFORE keyboard config.h
  (`builddefs/build_keyboard.mk` `config_h_community_module_appender`), so module macros beat keyboard defaults.
- `rules.mk` / `post_rules.mk` in the module folder are `-include`d into the build.
- A `.c` file with the **same basename as the module folder** is auto-added to `SRC`
  (`community_modules.py:_generate_modules_rules` → `SRC += $(wildcard {module_path}/{module_path.name}.c)`).
  With module folder `bk_split_flash`, that means `bk_split_flash.c` is compiled automatically.

### Split-transaction IDs

- Module declares its transaction ID in its `config.h`, e.g.
  ```c
  #define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH
  ```
- `qmk generate-split-transaction-id-community-modules-inc` emits a `.inc` that is injected into
  `enum serial_transaction_id` in `quantum/split_common/transaction_id_define.h` (see the
  `/* community modules */` block). The macro name is derived from the module folder name:
  `SPLIT_TRANSACTION_IDS_MODULE_<FOLDER>.UPPER_SNAKE`. In code, reference the **define value**
  (`PUT_BK_SPLIT_FLASH`), same pattern as `modules/qmk/split_data_sync`.
- The `.inc` is generated per-keymap: check `.build/obj_*/src/split_transaction_id_community_modules.inc`.
- `NUM_TOTAL_TRANSACTIONS` is capped at 32 (`transaction_id_define.h` STATIC_ASSERT); XQMK "budget".
  RPC adds 4 base IDs (`PUT_RPC_INFO`, `PUT_RPC_REQ_DATA`, `EXECUTE_RPC`, `GET_RPC_RESP_DATA`).

### RPC (split remote procedure call)

- API lives in `quantum/split_common/transactions.h` / `transactions.c`:
  - `void transaction_register_rpc(int8_t transaction_id, slave_callback_t callback);`
  - `bool transaction_rpc_exec(int8_t transaction_id, uint8_t m2s_size, const void *m2s,
    uint8_t s2m_size, void *s2m);`
  - `slave_callback_t = void (*)(uint8_t in_size, const void *in, uint8_t out_size, void *out);`
  - Convenience macros `transaction_rpc_send` / `transaction_rpc_recv` (one direction only).
- RPC is **not enabled by default**; `SPLIT_TRANSACTION_RPC` is turned on when at least one module
  defines a split transaction ID (generated `community_post_config.h`).
- Buffers: `RPC_M2S_BUFFER_SIZE` / `RPC_S2M_BUFFER_SIZE` default 32 (`transport.h:27-33`).
  Global macros; module can raise them via its `config.h` (e.g. 246/16). Sizes are `uint8_t`.
- The RPC buffers/state live inside `split_shared_memory_t` (`transport.h:239-243`).

### Community module execution hooks

- `keyboard_post_init_bk_split_flash()` runs once per half at post-init (register the RPC here).
- `housekeeping_task_bk_split_flash()` runs on **both halves** every main loop (probe master→slave here).
- Hook names derive from the module folder name (`<hook>_<module folder name>`).
- API version gate: `ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 2);`
  (current tree hooks list: `data/constants/module_hooks/`).
- Generated wrapper `community_modules.c` provides weak `*_modules()` wrappers that call each enabled
  module's hook; `keyboard.c` calls `keyboard_post_init_modules()` / `housekeeping_task_modules()`.

### Split watchdog

- `quantum/split_common/split_util.h` declares only
  `void split_watchdog_update(bool done); void split_watchdog_task(void); bool split_watchdog_check(void);`.
- `split_watchdog_init()` is **NOT public**: it exists in `split_util.c:98` and is called internally from
  `split_post_init()` under `#if defined(SPLIT_WATCHDOG_ENABLE)` (`split_util.c:223-228`). A community module
  must NOT call it (no header declaration; implicit-declaration error under GCC 15). The plan's
  "public `split_watchdog_update()` / `split_watchdog_init()` (split_util.h:14-15)" is wrong for `init`.
- Important detail: only `split_watchdog_update(true)` is enough to pause the reset. `split_watchdog_task()`
  resets the slave only when `!split_watchdog_done` (`split_util.c:113-119`). So the module's housekeeping
  loop calls `split_watchdog_update(true)` while busy — **no core edit needed**, and `init` is not needed
  (it only seeds the startup timestamp, already done internally).
- `split_watchdog_task()` is not weak — cannot be overridden.

## Build-environment facts (observed on 2026-11-12)

### ChibiOS HAL config version skew in this workspace

- This workspace's `platforms/chibios/boards/common/configs/halconf.h:32` defines `_CHIBIOS_HAL_CONF_VER_9_1_`,
  but `lib/chibios/os/hal/include/hal.h:135` requires `_CHIBIOS_HAL_CONF_VER_8_4_` → any compile aborts with
  `hal.h:136: error: #error "obsolete or unknown configuration file"`. This hit even `-km default`, i.e. it is
  **not** module-related. A workspace cleanup / submodule refresh is needed before compiling this board.
- (Note QMK upstream bump order in git log: `dd74b2326f ChibiOS, ChibiOS-Contrib updates (redux)` — the
  platform halconf templates and the `lib/chibios` submodule are expected to move in lockstep.)

### Userspace `vendor` keymap depends on a deleted userspace `bk_split_flash` compat shim

- The userspace `vendor` keymap for `4x6_4_procyon` lists `bastardkb/bk_pointing_device`, `bastardkb/argos`,
  etc. in its `keymap.json`/`config.h`. Those modules call `eeconfig_read_<mod>_datablock(...)` /
  `eeconfig_update_<mod>_datablock(...)` etc.
- Those APIs ARE generated by *this* tree's `community_modules.py` (`_render_eeconfig_declarations` /
  `_render_eeconfig_implementation`, guarded on `EECONFIG_MODULE_<slug>_DATA_SIZE > 0`) — but only for
  modules that declare such a datablock. The userspace argos / bk_pointing_device / bk_led_matrix modules do
  not, so on this tree the functions were previously provided by the (now deleted) userspace `bk_split_flash`
  module's `post_config.h` + `bk_split_flash_compat.c` shim (weak fallbacks returning zeroed defaults).
- Consequence: **deleting the userspace `bk_split_flash` module broke `-km vendor`** (GCC 15:
  "implicit declaration of function" errors in `bk_pointing_device.c`). Our in-tree module does NOT provide
  that shim (out of Phase-0 scope), so building `-km vendor` needs either (a) the shim restored/kept in the
  userspace, or (b) `-km default`, or (c) the userspace modules dropped. This is a workspace-state issue
  agreed with the user to resolve during their cleanup.
- In-tree module search: userspace `modules/` searched first, then firmware `modules/` — a same-named module
  in userspace shadows the firmware copy (`community_modules.py:find_available_module_paths`).

## Routing / console output

- `CONSOLE_ENABLE` (feature `console` in `info.json`/keyboard features / `rules.mk`) gates HID console output.
- `console: false` in a board's `keyboard.json` disables console; a keymap can re-enable via `rules.mk`
  or the keymap's feature override.
- Enable on the target vendor keymap already (`CONSOLE_ENABLE = yes` in userspace `vendor/rules.mk`).
- `dprintf` only prints when `debug_config.enable` is set (`quantum/logging/debug.h`); use it for debug,
  `xprintf`/`print` for unconditional logging. `print`/`xprintf` output when `CONSOLE_ENABLE` is on,
  regardless of the debug flag.
- For gate evidence that must appear without toggling the device debug flag, use `print`/`xprintf` (Phase 0
  probe/echo did, 2026-11-12). `debug.h` already `#include`s `print.h`, so both are available from one include.
- Slave-half console output reaching the master's `qmk console` depends on console relay and is not
  guaranteed. When slave-side evidence is needed, have the master print the content the slave returned
  over RPC (e.g. the echo buffer) rather than relying on the slave's own print lines.
- `print`/`xprintf` are safe to call from the RPC slave callback (it runs in the serial thread context,
  not an ISR).

## Split-flash handshake DIAG build (2026-11-13, TEMPORARY — revert after diagnosis)

- The rev 3 Phase 2 gate still showed `bk_split_flash: DECISION skip (no current-protocol reply from the
  secondary)` — three consecutive transport-level RPC failures (a wire failure of the 4-phase RPC), not a
  fingerprint/version decision. A **diagnostic-only** build discriminates the candidates.
- **Temporary instrumentation (module-only, no QMK core edits).** `#define BK_SPLIT_FLASH_DIAG 1` in the
  module `config.h` switches it on. It changes no decision logic; it only widens the observation window
  (`BK_SPLIT_FLASH_PROBE_MAX_TRIES` 3→200, `SPLIT_FLASH_HANDSHAKE_TIMEOUT` 4000→8000) and surfaces
  slave-side state over the echo. **All of it is reverted after the failure is identified.**
- **Config `-include` mechanism (why the header can see `BK_SPLIT_FLASH_DIAG`):** every module/keyboard
  `config.h` is passed to the compiler as `-include <file>` (`builddefs/common_rules.mk:281-284`,
  `build_keyboard.mk:537`), so the macro is defined before any `#include` in every translation unit,
  including `bk_split_flash.h`. The native host harness (`host/`) does not include `config.h`, so the
  `diag_*` echo fields are absent there — harmless (it only checks `sizeof(echo_t) <= 16`).
- **`bk_split_flash_echo_t` size:** without DIAG 12 B, with DIAG 16 B (4×u8 + 2×u32 + 3×u8, padded to 16)
  = exactly `RPC_S2M_BUFFER_SIZE`; the `_Static_assert` in `bk_split_flash.c` still holds.
- **How to read the DIAG log (which hypothesis):** FAIL with `dt≈20 ms` clustered right after the first
  `DIAG conn=1`, then a later attempt SUCCEEDS → H1 (early settle only). First `diag_ready=0` /
  `ready_before=0` with a large `dt_ms` and failures across the whole window → H2. FAIL with `dt<5 ms` /
  wrong shake / `probe answered but reply is not current protocol` → H3. No `DIAG attempt` line at all →
  H5. Fast FAILs with a handshake-constant mismatch → H6. `DIAG conn=0` the whole window → peer absent /
  timeout (different symptom).
- Diagnostic artifact: `bk_split_flash_phase2_diag.uf2` from
  `qmk compile -kb bastardkb/dilemma/4x6_4_procyon -km vendor`.

### DIAG run result (2026-11-13)

- Both halves on the diag image, primary USB only, power-cycle, full console. Raw sequence:
  `DIAG conn=1 t=2202` → `DIAG probing t0=2202 own_len=93328 own_crc=0x78ff6f7b conn=1` → attempts #1..#10,
  each `DIAG attempt #N t=… conn=1 rejects=N-1` then `DIAG attempt #N FAIL … dt=20 rejects=N conn=1`,
  cadence ~226 ms → `DIAG conn=0 t=4468` → `DECISION skip (no reply within SPLIT_FLASH_HANDSHAKE_TIMEOUT)
  t=10203 rejects=10`. No usable echo, no `probe answered` line, no relayed slave-handler line.
- Reading: **every attempt fails with `dt=20 ms` = the full `SERIAL_USART_TIMEOUT` read** (the echo never
  arrives), and the board's own split link then collapses (`conn` 1→0) and never recovers. The probe traffic
  itself is what kills the half-duplex link → **H2** (late/no echo → wire desync), not a fingerprint/version
  decision.
- Ruled out: **H1** (cap raised to 200, window to 8 s; still 10 consecutive fails, no later success, no early
  settle — settle-on-3 was not hiding a later success), **H5** (attempts ran → master role detected),
  **H6** (`dt=20 ms`, not a fast handshake-constant mismatch).
- Not separable in this log: **H3** (secondary on an older image whose handler also computes the CRC inline →
  identical wire-desync signature) vs **H2**. The field that would disambiguate (`ready_before`) travels in
  the echo, which never arrived because the failure is upstream of the echo. **H4** (peer absent) is unlikely
  but not fully excluded: `conn=1` at t=2202 can be the `connection_errors` initial default
  (`split_util.c:231`, `SPLIT_MAX_CONNECTION_ERRORS` default 10), though the ~2.3 s to reach `conn=0` fits a
  peer that was answering base syncs until probing broke the link.
- Init-order fact that makes H2 reachable on the slave: in `keyboard_init()` the slave's `split_post_init()`
  (transport up, `keyboard.c:546`) runs **before** `keyboard_post_init_quantum()` →
  `keyboard_post_init_modules()` (`keyboard.c:570`) where the module computes its fingerprint — so a probe
  landing in that window still hits the inline-CRC path in the handler (`bk_split_flash.c:76`).

### DIAG build 2 (2026-11-13) — H2 vs H3 discriminator

- **Why:** DIAG build 1 could not separate H2 (slow-but-present handler) from H3 (secondary on an older
  image), because the disambiguating field rode the echo that never arrived.
- **What it adds (all `#ifdef BK_SPLIT_FLASH_DIAG`, temporary):**
  - `config.h`: `SERIAL_USART_TIMEOUT` 20 → 200 ms, so a late echo (inline-CRC handler) can still be read
    instead of timing out and desyncing the wire. (No board/keymap overrides it — checked 2026-11-13.)
  - `bk_split_flash.c` master: `transaction_rpc_exec` is replaced by an explicit 4-phase replay
    (`transport_execute_transaction` for `PUT_RPC_INFO` / `PUT_RPC_REQ_DATA` / `EXECUTE_RPC` /
    `GET_RPC_RESP_DATA`, mirroring `transactions.c:1092-1117`) that logs each phase's ok/fail and duration:
    `DIAG phases info=..(..) req=..(..) exec=..(..) read=..(..)`.
  - the "reply is not current protocol" line now also prints `version` and `status`.
- **How to read it:** `info` fails (dt≈200) → the slave's transport rejects/does not ack → handshake
  mismatch (H3) or absent (H4). `info..exec` OK but `read` fails (dt≈200) → the slave accepted the request
  but no echo → slow/absent handler (H2 or H3-inline). Any usable reply → `version`=2 ⇒ H2, `version`=1 ⇒ H3.
- Artifact: `bk_split_flash_phase2_diag2.uf2`.
- **Run result (2026-11-13, both halves on diag2):** attempt #1 (t=563, before the secondary's boot)
  `info=1(0) req=1(0) exec=1(0) read=0(200)`; attempts #2–#4 `info=0(200)`; `conn=1` throughout; ends in
  the timeout decision. **Reading: the failing stage is the slave→master echo (the response read), not the
  request writes** — the request path completes in 0 ms, the echo never arrives even at 200 ms, and the
  half-duplex link desyncs after the first probe. With both halves on the same build the transport is
  compatible (H3/H6 excluded by construction), which leaves **H2** (late/absent echo → desync) as the
  family. Caveat: the raised 200 ms timeout perturbed the link (probes ~2.2 s apart, later writes timing
  out), so it was not a clean read against an up slave and the peer's `version` was never captured; H2 vs a
  transport-level response failure is still open.

### DIAG build 3 (2026-11-13) — H2 test (delayed first probe)

- **Why:** diag2 localized the failure to the RPC response (echo), but the raised 200 ms timeout perturbed
  the link. diag3 reverts `SERIAL_USART_TIMEOUT` to the default 20 ms and instead holds the master's first
  probe until `BK_SPLIT_FLASH_DIAG_PROBE_DELAY_MS` (6000 ms) after boot, so the first probe cannot land in
  the slave's transport-up -> post_init window. Phase logging is kept.
- **Decision:** a probe against a fully-initialized slave that SUCCEEDS → **H2 confirmed** (the early probe
  was the cause). A probe that still FAILS with the same `read` timeout → **H2 excluded**; it is a
  transport/response-level problem, not the inline-CRC window.
- Artifact: `bk_split_flash_phase2_diag3.uf2`.
- **Run result (2026-11-13, both halves on diag3):** `DIAG probing t0=6000` (first probe well after the
  secondary's boot/post_init), then attempt #1 `info=1(0) req=1(0) exec=1(0) read=0(20)`; attempts #2–#10
  `info=0(20)`; `conn=1` until t=8276 then 0; ends in the timeout decision. **The post-init probe still
  fails → H2 is EXCLUDED** (the transport-up → post_init inline-CRC window is not the cause). With H1/H5
  (diag1), H3/H6 (same build), and H4 (base link up, `conn=1`) also excluded, **no candidate hypothesis
  fits**: the RPC **response** (`GET_RPC_RESP_DATA`) is never delivered and the exchange desyncs the
  half-duplex link, reproducibly, between two identical connected builds. Since the Phase 0 probe/echo
  round-tripped cleanly, this is a Phase-2 regression at the RPC response/transport level (candidates: the
  probe/echo grew vs Phase 0, `version` 1→2, or the `post_init` CRC work) — to be bisected.
- **Phase 0 comparison (2026-11-13):** the existing `bk_split_flash_phase0.uf2` (both halves) round-trips
  cleanly and continuously (`probe seq=N -> echo seq=N magic=0x5f version=1 status=0`, seq mirrored).
  ⇒ the cable/baud/topology/serial transport are fine; the failure is a **Phase-2 code regression on the
  RPC response path**, not environment and not H1–H6. Phase 0's echo print showed only
  seq/magic/version/status (no `image_len`/`image_crc`), so Phase 0's probe/echo were minimal (~3/4 bytes)
  vs Phase 2's 12/16.

## Phase 2 handshake — FAILED: dead-ends and findings (2026-11-13)

Phase 2's capability/fingerprint handshake **does not work on hardware** and has been reverted. A fresh
approach is needed; what follows is what we established so a retry does not repeat it.

**Symptom.** The primary prints `DECISION skip (no current-protocol reply from the secondary)` — three
consecutive transport-level failures of the 4-phase RPC (`transaction_rpc_exec`).

**Proven with temporary DIAG builds 1-3 (now reverted):**
- The failing stage is the RPC **response** (`GET_RPC_RESP_DATA`). The request writes (`PUT_RPC_INFO`,
  `PUT_RPC_REQ_DATA`, `EXECUTE_RPC`) complete in ~0 ms; the response read times out at the full
  `SERIAL_USART_TIMEOUT`, and the exchange **desyncs the half-duplex link** (later transactions, then the
  base split sync, fail). Observed as `DIAG phases info=1(0) req=1(0) exec=1(0) read=0(20)`.
- Excluded: H1 early 3-strike settle (cap 200 + 8 s window — still no success), H2 pre-`post_init`
  inline-CRC window (delaying the first probe to 6 s did not help), H3 wrong image and H6 build skew (both
  halves on the same build ⇒ same `NUM_TOTAL_TRANSACTIONS`), H4 never-connected (base link up, `conn=1`),
  H5 role mis-detect (probes ran on the master).
- **The Phase 0 probe/echo round-trips cleanly** on the same hardware (`probe seq=N -> echo seq=N
  magic=0x5f version=1 status=0`, seq mirrored) ⇒ cable/baud/topology/serial transport are fine. So Phase 2
  is a **code regression on the RPC response path**, not environment.

**Current Phase 2 approach (as implemented, now reverted):** `bk_split_flash.c` runs a one-shot master
handshake — probe (`magic`/`version=2`/`seq`/`image_len`/`image_crc`) via `transaction_rpc_exec`, gated on
`is_transport_connected()`, re-probed every 200 ms inside a 4 s (DIAG 8 s) window, settling after 3 (DIAG
200) transport failures; the slave handler echoes its own `version`/`image_len`/`image_crc`; the master
applies the skip policy. Both halves compute a CRC-32 over `[XIP_BASE, __flash_binary_end)` at
`keyboard_post_init`.

**Prime suspects for a retry (unresolved — the bisect was not run):**
- the probe/echo **payload grew** vs Phase 0 (Phase 0's echo print showed only seq/magic/version/status, so
  ~3/4 bytes; Phase 2's are 12/16) — and the response direction is exactly what fails;
- the `post_init` CRC work;
- `version` 1→2.

**Recommendation for a fresh start:** reproduce the Phase 0 round-trip success with the *final* payload
shape **before** adding any handshake policy, then layer the fingerprint/skip logic on top. Treat "the RPC
response does not come back" as the first-class failure to design against, because it desyncs the link and
is not recoverable inside the window.

## Keyboards / build target

- Target board: `bastardkb/dilemma/4x6_4_procyon` (folder under `keyboards/bastardkb/dilemma/`,
  underscore form, NOT `4x6_4/procyon`).
- Board `keyboard.json` sets e.g. split protocols, `PICO_FLASH_SIZE_BYTES` etc; check it for
  flash-layout numbers. The MK measured stock image is `bastardkb_dilemma_4x6_4_procyon_vendor.uf2`.
- The board's real keymap is the userspace `vendor` keymap (see userspace); "default" keymap is a bare C
  keymap with no `keymap.json`. Phase UF2s are built with `-km vendor` (user-confirmed 2026-11-12) so the
  module is already listed + console is on.
- **Halves boot ~3 s apart on the dilemma 4x6_4 procyon (measured 2026-11-13).** When powered via the
  split cable, the secondary comes up ~3 s after the primary starts. Any time-bound handshake/probe on the
  primary must tolerate at least this skew or it decides before the secondary can answer; the Phase 2
  handshake window is therefore 4000 ms with a 200 ms probe cadence (knobs
  `SPLIT_FLASH_HANDSHAKE_TIMEOUT` / `BK_SPLIT_FLASH_PROBE_INTERVAL_MS`). Non-blocking housekeeping polling
  keeps the primary fully usable during the wait.
- **Split serial reads can block the main loop; RPC failures do not fool `is_transport_connected`.**
  Each failed `serial_transport_receive` blocks its caller for up to `SERIAL_USART_TIMEOUT` (default
  20 ms). QMK's `connection_errors` (and hence `is_transport_connected()`, split_util.c:230) is only
  updated by the base split-sync path (`transport_master`), NOT by raw RPC (`transaction_rpc_exec`), so
  probing a peer that is mid-boot or on different firmware repeatedly blocks the main loop for ~20 ms
  without ever flipping "disconnected" — a module that probes on a fixed cadence can stall the primary's
  boot for seconds (root-caused 2026-11-13 via a handshake-disabled A/B build). Rule: only run split-RPC
  from housekeeping code when `is_transport_connected()` is already true, and settle (skip/fail) after a
  small number of consecutive RPC failures instead of hammering the whole window.
- **The slave RPC handler must be effectively instantaneous.** EXECUTE_RPC returns to the master as soon
  as the request is written; the master immediately opens GET_RPC_RESP_DATA and waits ≤ `SERIAL_USART_TIMEOUT`
  for the echo. Any work the handler does on the slave's main-loop path (i.e. the one-time CRC-32 of the
  92 KiB image, ~tens of ms) delays the echo past the master's 20 ms read → `transaction_rpc_exec` fails
  AND the stray echo desynchronizes the half-duplex wire (every later transaction times out) until the
  connection re-syncs. Root-caused 2026-11-13 with a stage-logging probe (stage1-3 OK in 2 ms, stage4/read
  timed out at 20 ms, then all stages timed out). Fix: compute fingerprints at `keyboard_post_init`
  (both halves), never inside the RPC callback.

## RP2040 flash self-write (learned 2026-11-13, Phase 1 safety core)

- **pico-sdk primitives:** `flash_range_erase` / `flash_range_program` (`lib/pico-sdk/.../hardware_flash/flash.c`)
  are already `__no_inline_not_in_flash_func` (RAM), copy boot2 to RAM before exiting XIP and re-enter XIP
  via that copy. `hardware_flash/flash.c` is in `PICOSDKSRC` (RP2040.mk), so the module can call them
  directly — no need to re-implement the boot2/XIP dance (the plan's "pattern in
  `wear_leveling_rp2040_flash.c`" is exactly what these primitives do).
- **Requirements when calling them:** interrupts disabled (`save_and_disable_interrupts`/`restore_interrupts`
  are static inline in `hardware/sync.h` — no link dependency though `hardware_sync/sync.c` is NOT in
  PICOSDKSRC); the `data` buffer passed to `flash_range_program` must be in RAM; `flash_range_program`
  offset must be 256-byte aligned and `count` a multiple of 256; `flash_range_erase` offset/count must be
  4 KiB sector aligned (64 KiB block rounding is handled by the bootrom — procyon is single-core
  `CRT0_EXTRA_CORES_NUMBER=0`, so no core 1 concern).
- **Callers may stay in flash** (XIP is re-enabled between each op); only code executing *during* an op
  must be RAM-resident. Exception: if the active region is being erased/rewritten, the loop between ops
  must ALSO be RAM (its own code is in the erased region). `bk_split_flash_commit` is therefore
  `__no_inline_not_in_flash_func` (→ `.time_critical.*` → RAM `.data`) and must not call memcpy/print/etc.
  The linker may emit a flash `*_veneer` stub for cross-TU calls into it — harmless (runs with XIP up).
- **Vector-table hazard:** `crt0_v6m.S` sets VTOR to `_vectors` in **flash** (sector 0). Erasing sector 0
  while running = next IRQ fetches `0xFF` → hard fault. Hence the module erases sector 0 only inside the
  interrupts-off commit (the approved §4 arm-at-commit-start change).
- **Linker symbols:** `__flash_binary_start`/`__flash_binary_end`/`__boot2_start__` from
  `RP2040_FLASH_TIMECRIT.ld`. Image length = `&__flash_binary_end - XIP_BASE`.
- **Flash-size reality:** `PICO_FLASH_SIZE_BYTES` stays 2 MiB even on BKB 4 MiB chips
  (`EXTERNAL_FLASH_SIZE 4MiB` in board config.h does NOT feed pico-sdk; linker `flash1` defaults to 2048k
  since `FLASH_LEN` is undefined). Wear-leveling EEPROM base = `PICO_FLASH_SIZE_BYTES - 8192` = `0x1FE000`;
  the module's staging slot at `0x100000` (1 MiB) + 256 KiB stays clear of it.
- **GP17** is the BKB status LED (`RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED`), not used by matrix/RGB, free
  after boot; QMK chibios `GP17` is literally the numeral `17`, so `sio_hw->gpio_set/clr = 1u<<17` works
  to pulse it from the RAM-resident commit (configure SIO output beforehand via pico `gpio_set_function`/
  `gpio_set_dir`).
- **Phase 1 build facts:** `qmk compile -kb bastardkb/dilemma/4x6_4_procyon -km vendor` builds the module
  (`bk_split_flash_flash.c` is added with `SRC +=` in the module `rules.mk`). Phase 1 image: len 93280,
  crc32 `0x48dd14d1` (~91 KiB binary, UF2 186880 B — comfortably under `SPLIT_FLASH_MAX_IMAGE_SIZE`).
  `bk_flash_page_buf` + `.time_critical` commit cost ~0.6 KiB RAM; `.bss` was already ~256 KiB (RGB/VIA
  vendor keymap) — RAM is tight, watch it in later phases.
- **RP2040 boot2 checksum (verified 2026-11-13 empirically against all six prebuilt blobs in
  `stage2_bootloaders.c`):** it is NOT a word-sum. `checksum = bitrev32(crc_reg(bitrev8(each of boot2[0:252])))`
  where `crc_reg` is the raw CRC-32 register (init 0xFFFFFFFF, NO final complement), and `crc_reg` is what
  `bk_split_flash_crc32` returns *before* the final `~`. Stored checksum word = little-endian at boot2[252:256].
  `bk_split_flash_boot2_checksum_valid()` in the module header implements it; the native host harness covers
  it with the real blob (0xd58f0b07).
- **Vector table offset:** in a QMK/ChibiOS RP2040 image, boot2 occupies image[0,256) and the ARM vector
  table starts at 0x100 — the initial SP word is at image+0x100 and the reset vector at image+0x104 (a
  real Phase-1 image: sp=0x20040400, pc=0x100002c5). The module's `bk_split_flash_image_plausible()`
  reads those offsets (build independent), plus the boot2 checksum.
- **Dilemma Bootmagic (root cause of the "dead secondary" gate symptom, found 2026-11-13):**
  `keyboards/bastardkb/dilemma/4x6_4_procyon/keyboard.json` enables full bootmagic with
  `split.bootmagic.matrix = [5, 0]`. That value generates **only** `BOOTMAGIC_ROW_RIGHT 5` /
  `BOOTMAGIC_COLUMN_RIGHT 0` in `info_config.h`; the left half falls back to `quantum/bootmagic/bootmagic.h`
  defaults `BOOTMAGIC_ROW 0` / `BOOTMAGIC_COLUMN 0`. `bootmagic.c:46-56` checks
  `matrix_get_row(row) & (1<<col)` on boot and calls `bootloader_jump()` — so **holding L00 (left top-left)
  at boot puts that half into RP2040 ROM BOOTSEL before QMK runs** (no RGB/keys/transport; `probe failed`
  forever on the primary; clears only on a power cycle without that key). Confirmed identical on the stock
  image — nothing to do with `bk_split_flash`. Hence the Phase 1 test trigger was moved to `(1,0)` (the key
  directly below the top-left). Useful corollary: hold L00 while plugging that half's own USB to enter
  BOOTSEL for flashing without double-tap.
- **Split transport is serviced from the main loop:** `quantum/matrix_common.c:112` calls
  `transport_slave(...)` on the slave half. A test hook that blocks housekeeping therefore also kills
  the split link — the master prints `probe failed (secondary not connected?)` forever, the slave's
  RGB/keys freeze, and the state clears only on a power cycle (fresh boot, no key held); it never
  self-recovers because `mcu_reset()` is never reached. **Phase 1 hook was reworked (2026-11-13) into a
  non-blocking state machine**: one flash page per housekeeping tick, and its state is reported through
  the probe echo `status=` field (0 idle, 1 armed, 2 staging, 3 verified, 4 commit, 5 done,
  241..245 = fail stage). If the trigger key is held through a reboot, the hook re-triggers — release
  once `status=1` appears. User observation: a left half that is dead for a full minute and only
  recovers via unplug/replug (then enumerates as a normal keyboard standalone, NOT `RPI-RP2`) rules
  out a BOOTSEL/erased-boot2 state; the stuck state was a firmware-level loop.
- **GP17 status LED:** `RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED GP17` is defined for BKB RP2040 boards,
  but during Phase 1 gate testing on the dilemma 4x6_4_procyon, driving GP17 produced **no visible
  output** (the module's LED phase patterns were never seen). Do not rely on it as user-facing
  progress. The working Phase 1 indicator is the console `status=` in the probe echo; the user approved
  an **RGB progress effect on the primary for Phase 3** as the product-facing indicator (plan §7).
- **Master/slave role:** `split_util.c:181` `is_keyboard_master_impl()` = `usb_bus_detected()`; the
  USB-connected half is master. Standalone left half (its own USB) is master and legitimately prints
  `probe failed` since no secondary is connected.

## Misc

- `.build/` has per-target `obj_*` dirs and the generated `src/` intermediates (community modules, .inc files).
- Half-duplex vendor serial, 230400 baud on this board; SERIAL_DRIVER = vendor.
- Keep the CLI output clean: this repo builds with `qmk compile`; check `qmk env` for QMK_HOME/USERSPACE
  resolution (userspace = `/home/qlebastard/qmk_userspace_bstiq`).
- `qmk_module.json` `features` (e.g. `"features": {"console": true}`) flow into the generated
  `community_rules.mk` via `generate_features_rules`, so a module can self-require a feature (console,
  deferred_exec, ...). That is the clean way for the probe/echo module to guarantee console without
  editing the keymap.
