# QMK Knowledge Base

Researched findings about how this QMK tree works, so future work checks here **before** re-researching.
When you learn something new about QMK/this repo, add it here. Keep it to **how QMK works** — project
plan/status belongs in the plan documents, not here.

---

## Community modules

### Discovery and paths

- Loader: `lib/python/qmk/community_modules.py:find_available_module_paths()`.
- Search order: `<QMK_USERSPACE>/modules` **first**, then `<QMK_FIRMWARE>/modules`. Userspace shadows the
  firmware copy of the same module name.
- A module directory is anything under `modules/` containing `qmk_module.json`. Wire modules with a
  **plural** `modules/` folder name (a singular `module/` path is NOT searched).
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
  Every `config.h` in the chain is passed to the compiler as `-include <file>`
  (`builddefs/common_rules.mk:281-284`, `build_keyboard.mk:537`), so its macros are defined before any
  `#include` in every translation unit.
- `rules.mk` / `post_rules.mk` in the module folder are `-include`d into the build.
- A `.c` file with the **same basename as the module folder** is auto-added to `SRC`
  (`community_modules.py:_generate_modules_rules` → `SRC += $(wildcard {module_path}/{module_path.name}.c)`).
  With module folder `bk_split_flash`, that means `bk_split_flash.c` is compiled automatically.

### Split-transaction IDs

- A module declares its transaction ID in its `config.h`, e.g.
  ```c
  #define SPLIT_TRANSACTION_IDS_MODULE_BK_SPLIT_FLASH PUT_BK_SPLIT_FLASH
  ```
- `qmk generate-split-transaction-id-community-modules-inc` emits a `.inc` that is injected into
  `enum serial_transaction_id` in `quantum/split_common/transaction_id_define.h` (see the
  `/* community modules */` block). The macro name is derived from the module folder name:
  `SPLIT_TRANSACTION_IDS_MODULE_<FOLDER>.UPPER_SNAKE`. In code, reference the **define value**
  (`PUT_BK_SPLIT_FLASH`), same pattern as `modules/qmk/split_data_sync`.
- The `.inc` is generated per-keymap: check `.build/obj_*/src/split_transaction_id_community_modules.inc`.
- `NUM_TOTAL_TRANSACTIONS` is capped at 32 (`transaction_id_define.h` STATIC_ASSERT). The RPC adds 4 base
  IDs (`PUT_RPC_INFO`, `PUT_RPC_REQ_DATA`, `EXECUTE_RPC`, `GET_RPC_RESP_DATA`).
- A module does **not** get its own shared-memory buffer: the generator adds the ID and (below) the RPC, but
  no `split_shared_memory_t` member. A module that needs a buffer reuses the RPC buffers
  (`rpc_m2s_buffer` / `rpc_s2m_buffer`).

### RPC (split remote procedure call)

- API lives in `quantum/split_common/transactions.h` / `transactions.c`:
  - `void transaction_register_rpc(int8_t transaction_id, slave_callback_t callback);`
  - `bool transaction_rpc_exec(int8_t transaction_id, uint8_t m2s_size, const void *m2s,
    uint8_t s2m_size, void *s2m);`
  - `slave_callback_t = void (*)(uint8_t in_size, const void *in, uint8_t out_size, void *out);`
  - Convenience macros `transaction_rpc_send` / `transaction_rpc_recv` (one direction only).
- The RPC is a **4-phase exchange** (`transaction_rpc_exec`, `transactions.c:1081-1118`): `PUT_RPC_INFO`
  (writes the `rpc_sync_info_t` header), `PUT_RPC_REQ_DATA` (writes the m2s payload), `EXECUTE_RPC`
  (invokes the slave callback), `GET_RPC_RESP_DATA` (reads the s2m payload). The header carries the
  lengths, so the sizes are set dynamically per call; the slave reads them in `slave_rpc_info_callback`.
- RPC is **not enabled by default**; `SPLIT_TRANSACTION_RPC` is turned on when at least one module defines
  a split transaction ID (generated `community_post_config.h`).
- Buffers: `RPC_M2S_BUFFER_SIZE` / `RPC_S2M_BUFFER_SIZE` default 32 (`transport.h:27-33`). Global macros; a
  module can raise them via its `config.h`. Sizes are `uint8_t`. The buffers/state live inside
  `split_shared_memory_t` (`transport.h:239-243`).

### Community module execution hooks

- Hook names derive from the module folder name: `keyboard_post_init_<module>()` runs once per half at
  post-init; `housekeeping_task_<module>()` runs on **both halves** every main loop.
- API version gate: `ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 2);`
  (current tree hooks list: `data/constants/module_hooks/`).
  The versioned hook lists there run `0.1.0` → `1.1.3`; `keyboard_post_init_<module>` /
  `housekeeping_task_<module>` exist from `1.0.0`, so a module using only those may assert `(1, 0, 0)`
  (e.g. `modules/qmk/hello_world`); a higher assert is only needed for hooks added later (e.g. split data
  sync support is `1.1.2`, `modules/qmk/split_data_sync`). The assert is a *minimum*: any value ≤ the tree's
  provided version compiles.
- Generated wrapper `community_modules.c` provides weak `*_modules()` wrappers that call each enabled
  module's hook; `keyboard.c` calls `keyboard_post_init_modules()` / `housekeeping_task_modules()`.

### Split watchdog

- `quantum/split_common/split_util.h` declares only
  `void split_watchdog_update(bool done); void split_watchdog_task(void); bool split_watchdog_check(void);`.
- `split_watchdog_init()` is **NOT public**: it exists in `split_util.c:98` and is called internally from
  `split_post_init()` under `#if defined(SPLIT_WATCHDOG_ENABLE)` (`split_util.c:223-228`). A community module
  must NOT call it (no header declaration; implicit-declaration error under GCC 15).
- Only `split_watchdog_update(true)` is needed to pause the reset: `split_watchdog_task()` resets the slave
  only when `!split_watchdog_done` (`split_util.c:113-119`). So a module's housekeeping loop can call
  `split_watchdog_update(true)` while busy — **no core edit needed**, and `init` is not needed (it only
  seeds the startup timestamp, already done internally).
- `split_watchdog_task()` is not weak — cannot be overridden.

## Split transport (serial, half-duplex)

- `quantum/split_common/transport.c` (`transport_execute_transaction`, `transport.c:104-121`) is the one
  primitive: `transport_execute_transaction(id, m2s_buf, m2s_len, s2m_buf, s2m_len)`. It looks up
  `split_transaction_table[id]`, copies the master's m2s buffer into the shmem buffer at the entry's offset,
  runs the wire transaction, then copies the s2m result back out. Both copies are
  `min(entry_size, caller_length)`, so passing `sizeof(struct)` (== the entry size) copies it whole; these
  lengths themselves never travel on the wire.
- `split_transaction_table[]` is `extern` and **non-const** (`transactions.h:38`); its entries hold
  `{m2s_size, m2s_offset, s2m_size, s2m_offset, slave_callback}`. A module can write its own entry at
  runtime (as `transaction_register_rpc` does, `transactions.c:1071-1079`).
- **The slave dispatch is fully generic and table-driven** (`platforms/chibios/drivers/serial_protocol.c`
  `react_to_transaction`, `serial_protocol.c:51-94`): it reads one transaction-ID byte, sanity-checks
  `id < NUM_TOTAL_TRANSACTIONS`, then uses `split_transaction_table[id]` for the m2s length/offset, the
  callback, and the s2m length/offset. The **lengths come from the table, not the wire**, so any ID under
  the enum works — not just the core list serviced by `transactions_slave()`/`transactions_master()`
  (`transactions.c:1022-1067`).
- Master side: `initiate_transaction` (`serial_protocol.c:113-157`) sends the ID, reads back the handshake
  `id ^ NUM_TOTAL_TRANSACTIONS`, sends the m2s buffer, receives the s2m buffer.
- The slave's protocol runs in a dedicated thread (`SlaveThread`, HIGHPRIO, `serial_protocol.c:18-39`), so a
  slave callback that blocks (e.g. a flash write) blocks the whole link.
- Half-duplex vendor serial; `SERIAL_DRIVER = vendor`. The **current `-km vendor` build runs at 625000 baud**
  — the userspace `users/vendor/config.h` sets `SERIAL_USART_SPEED 625000` and the build includes it
  (`-include …/users/vendor/config.h`, confirmed in the build's `cflags.txt`). That is ~62.5 KB/s, ~4 ms per
  246-byte chunk, ~1.6 s of wire time for the 93692 B step-4 image. The earlier 230400 figure (≈23 KB/s,
  ~11 ms/chunk) matches the steps 1–3 measurements taken before that override; the 2026-11-14 plan's
  "stay at 230400" non-goal is stale. The PIO driver derives its clock from `SERIAL_USART_SPEED`
  (`serial_vendor.c:358`), so the baud remains a config change.
- Each failed `serial_transport_receive` blocks its caller for up to `SERIAL_USART_TIMEOUT` (default 20 ms,
  `serial_usart.h:43-45`).
- `connection_errors` (hence `is_transport_connected()`, `split_util.c:230-231`) is only updated by the base
  split-sync path (`transport_master`), **not** by raw transactions. `connection_errors` starts at 0
  (= "connected") and resets to 0 on every successful base sync; it increments on a failed one and trips
  "disconnected" at `SPLIT_MAX_CONNECTION_ERRORS` (default 10, `split_util.c:46-47`).
- Split sync is serviced from the main loop: `quantum/matrix_common.c:112` calls `transport_slave(...)` on
  the slave half. A blocking hook therefore also kills the split link.
- **Slave callback argument quirk:** `serial_protocol.c` `react_to_transaction` passes the m2s size as the
  *third* argument too — `transaction->slave_callback(initiator2target_buffer_size, in, initiator2target_buffer_size, out)`.
  So a module callback must **not** trust the size arguments; use its own fixed sizes (compile-time struct sizes).
- **Custom transaction proven at 240 B (hardware):** on the dilemma 4x6_4 procyon (230400 baud, vendor serial),
  a single module transaction carrying a 240-byte m2s and a 4-byte s2m — registered on both halves via a
  `split_transaction_table` entry reusing `rpc_m2s_buffer`/`rpc_s2m_buffer` (with `RPC_M2S_BUFFER_SIZE 240`) and
  driven from master `housekeeping_task` every 500 ms with one `transport_execute_transaction` — round-trips
  reliably and repeatedly (26+ consecutive). Register the table entry in `keyboard_post_init_<module>` (runs on
  both halves; the slave thread starts in `split_post_init`, just before it — a probe in that tiny window fails
  once and self-recovers via the thread's clear-on-error).
- **Bulk streaming proven (hardware, 2026-11-14):** the same transaction streams the master's whole image
  (398 chunks × 240 B m2s, offset-carrying header + per-chunk CRC-8) at one transaction per master
  `housekeeping_task` call (~4.5 s for 91820 B, ~11 ms/chunk), with the slave accumulating an incremental
  CRC-32 and returning it in the 4 B s2m at end of stream; master and slave values matched. The keyboard
  stayed usable during the burst (one transaction per main-loop iteration).

## Build-environment facts (observed in this workspace)

### ChibiOS HAL config version skew

- This workspace's `platforms/chibios/boards/common/configs/halconf.h:32` defines `_CHIBIOS_HAL_CONF_VER_9_1_`,
  but `lib/chibios/os/hal/include/hal.h:135` requires `_CHIBIOS_HAL_CONF_VER_8_4_` → any compile aborts with
  `hal.h:136: #error "obsolete or unknown configuration file"`. This hit even `-km default`, i.e. it is
  **not** module-related. A workspace cleanup / submodule refresh is needed before compiling this board.

### Userspace `vendor` keymap depends on a userspace compat shim

- The userspace `vendor` keymap for `4x6_4_procyon` lists `bastardkb/bk_pointing_device`, `bastardkb/argos`,
  etc. Those modules call `eeconfig_read_<mod>_datablock(...)` / `eeconfig_update_<mod>_datablock(...)`.
- Those APIs ARE generated by *this* tree's `community_modules.py` (`_render_eeconfig_declarations` /
  `_render_eeconfig_implementation`, guarded on `EECONFIG_MODULE_<slug>_DATA_SIZE > 0`) — but only for
  modules that declare such a datablock. The userspace argos / bk_pointing_device / bk_led_matrix modules do
  not, so on this tree the functions were previously provided by a (now deleted) userspace `bk_split_flash`
  module's `post_config.h` + `bk_split_flash_compat.c` shim (weak fallbacks returning zeroed defaults).
- Consequence: deleting the userspace `bk_split_flash` module breaks `-km vendor` (GCC 15: "implicit
  declaration of function" errors in `bk_pointing_device.c`). Building `-km vendor` needs either (a) the shim
  restored/kept in the userspace, or (b) `-km default`, or (c) the userspace modules dropped.
- In-tree module search: userspace `modules/` searched first, then firmware `modules/` — a same-named module
  in userspace shadows the firmware copy (`community_modules.py:find_available_module_paths`).

## Routing / console output

- `CONSOLE_ENABLE` (feature `console` in `info.json`/keyboard features / `rules.mk`) gates HID console output.
- `console: false` in a board's `keyboard.json` disables console; a keymap can re-enable via `rules.mk`
  or the keymap's feature override.
- `dprintf` only prints when `debug_config.enable` is set (`quantum/logging/debug.h`); use it for debug,
  `xprintf`/`print` for unconditional logging. `print`/`xprintf` output when `CONSOLE_ENABLE` is on,
  regardless of the debug flag. `debug.h` already `#include`s `print.h`, so both are available from one include.
- Slave-half console output reaching the master's `qmk console` depends on console relay and is not
  guaranteed. When slave-side evidence is needed, have the master print content the slave returned over the
  transport (e.g. an echo/ack buffer) rather than relying on the slave's own print lines.
- `print`/`xprintf` are safe to call from a slave callback (it runs in the serial thread context, not an ISR).

## Keyboards / build target

- Target board: `bastardkb/dilemma/4x6_4_procyon` (folder under `keyboards/bastardkb/dilemma/`, underscore
  form, NOT `4x6_4/procyon`).
- Board `keyboard.json` sets split protocols, `PICO_FLASH_SIZE_BYTES` etc; check it for flash-layout numbers.
- The board's real keymap is the userspace `vendor` keymap (see userspace); "default" keymap is a bare C
  keymap with no `keymap.json`. Build with `-km vendor` so modules are listed and console is on.
- **Halves boot ~3 s apart on the dilemma 4x6_4 procyon (measured).** When powered via the split cable, the
  secondary comes up ~3 s after the primary starts. Any time-bound master behaviour must tolerate at least
  this skew or it acts before the secondary can answer.
- **Master/slave role:** `split_util.c:181` `is_keyboard_master_impl()` = `usb_bus_detected()`; the
  USB-connected half is master. A standalone half (its own USB, no secondary) is master and legitimately has
  no peer.

## RP2040 flash self-write

- **pico-sdk primitives:** `flash_range_erase` / `flash_range_program`
  (`lib/pico-sdk/.../hardware_flash/flash.c`) are already `__no_inline_not_in_flash_func` (RAM), copy boot2
  to RAM before exiting XIP and re-enter XIP via that copy. `hardware_flash/flash.c` is in `PICOSDKSRC`
  (RP2040.mk), so module code can call them directly — no need to re-implement the boot2/XIP dance (the
  `wear_leveling_rp2040_flash.c` pattern is exactly what these primitives do).
- **Headers are on the module include path:** `RP2040.mk` `PICOSDKINC` puts `hardware_flash/include`,
  `hardware_sync/include`, and `rp2040/hardware_regs/include` on the include path, so module code can
  `#include "hardware/flash.h"` (`flash_range_erase`/`_program`, `FLASH_PAGE_SIZE`=256, `FLASH_SECTOR_SIZE`=4096),
  `hardware/sync.h` (`save_and_disable_interrupts`/`restore_interrupts`), and `hardware/regs/addressmap.h`
  (`XIP_BASE`=0x10000000) directly.
- **Requirements when calling them:** interrupts disabled (`save_and_disable_interrupts` /
  `restore_interrupts` are static inline in `hardware/sync.h`); the `data` buffer passed to
  `flash_range_program` must be in RAM; `flash_range_program` offset must be 256-byte aligned and `count` a
  multiple of 256; `flash_range_erase` offset/count must be 4 KiB sector aligned (64 KiB block rounding is
  handled by the bootrom — procyon is single-core `CRT0_EXTRA_CORES_NUMBER=0`, so no core 1 concern).
- **Callers may stay in flash** (XIP is re-enabled between each op); only code executing *during* an op must
  be RAM-resident. Exception: if the active region is being erased/rewritten, the loop between ops must ALSO
  be RAM (its own code is in the erased region), so such a routine must be `__no_inline_not_in_flash_func`
  (→ `.time_critical.*` → RAM `.data`) and must not call memcpy/print/etc. **Beware the linker's veneers:** a
  direct call from such a RAM routine into another RAM function (e.g. `flash_range_erase`) is a cross-region
  call, and the linker resolves it with a `*_veneer` stub placed in **flash** (`__flash_range_erase_veneer` /
  `__flash_range_program_veneer` were at 0x100139b0.. in the step-4 map). Once sector 0 is erased that stub is
  gone, so the *next* call faults. Call the RAM flash primitives through a **`volatile` function pointer**
  instead: the indirect `blx` needs no veneer and survives the erase (verified in the step-4 disassembly — the
  pointer words hold RAM addresses 0x200003xx and the commit routine uses `blx`). The flash→RAM call *into*
  the commit routine may use a flash veneer; that call happens with XIP up, before the erase.
- **Sector-0 erase order vs boot2 copy-out:** `flash_range_erase`/`_program` call
  `flash_init_boot2_copyout()` on their first XIP exit, which snapshots the 256-byte boot2 from the *current*
  sector 0 into RAM and re-enters XIP via that copy. So the commit's first op must be the erase of sector 0
  (the snapshot happens before it disappears), and boot2 (page 0) must be programmed last so the image only
  becomes bootable at the end. In normal operation the snapshot was already taken by the earlier staging
  erase, and the boot2 blob is chip-specific rather than image-specific.
- **Erase must run outside any transaction:** `flash_range_erase`/`_program` require interrupts disabled, and
  with IRQs off the ChibiOS scheduler is stopped, so the slave's `SlaveThread` cannot service the link for the
  duration of the op. A 256 B page program (~1–5 ms) fits inside the 20 ms transport read timeout from a
  slave callback; a 4 KiB sector erase (tens of ms) does not. So an erase is best done *before* the master
  initiates (e.g. at slave `keyboard_post_init`), and the master must not transact until it has completed.
- **A whole-slot read-back does not fit one callback:** reading the whole ~90 KiB staging slot over XIP and
  CRC-32ing it in a single slave callback exceeds the 20 ms timeout (observed on hardware: the end-of-stream
  VERIFY transaction returned `FAIL reason=tx` while the stream itself was fine). Read the slot back a 256 B
  page at a time as each page is programmed and fold it into an incremental CRC, so the verify callback is
  O(1) and can never blow the timeout.
- **Re-programming identical data is idempotent:** NOR program only clears bits (1→0) and never sets them, so
  writing the same bytes again over an already-programmed page leaves it unchanged (no 0→1 transition is ever
  needed). Streaming the *same* image repeatedly to an already-erased staging slot is therefore safe without
  re-erasing; re-writing a *different* image without erasing would corrupt (result = old AND new).
- **Vector-table hazard:** `crt0_v6m.S` sets VTOR to `_vectors` in **flash** (sector 0). Erasing sector 0
  while running = next IRQ fetches `0xFF` → hard fault. So sector 0 must only be erased inside an
  interrupts-off commit.
- **Linker symbols:** `__flash_binary_start` / `__flash_binary_end` / `__boot2_start__` from
  `RP2040_FLASH_TIMECRIT.ld`. Image length = `&__flash_binary_end - XIP_BASE`. flash1's origin is
  `0x10000000` = `XIP_BASE` (`addressmap.h:18`), so `__flash_binary_start` == `XIP_BASE`;
  the step-1 `-km vendor` map showed `__flash_binary_start=0x10000000` and
  `__flash_binary_end=0x10016414` (~89 KiB). No header declares these symbols, so a module declares them
  itself: `extern const uint8_t __flash_binary_start[];` (likewise `…_end`). Reading `[start, end)` over XIP
  while running is fine.
- **Flash-size reality:** `PICO_FLASH_SIZE_BYTES` stays 2 MiB even on BKB 4 MiB chips
  (`EXTERNAL_FLASH_SIZE 4MiB` in board config.h does NOT feed pico-sdk; linker `flash1` defaults to 2048k
  since `FLASH_LEN` is undefined). Wear-leveling EEPROM base = `PICO_FLASH_SIZE_BYTES - 8192` = `0x1FE000`;
  a staging slot at `0x100000` (1 MiB) + 256 KiB stays clear of it.
- **RP2040 boot2 checksum (verified empirically against all six prebuilt blobs in
  `stage2_bootloaders.c`):** it is NOT a word-sum.
  `checksum = bitrev32(crc_reg(bitrev8(each of boot2[0:252])))` where `crc_reg` is the raw CRC-32 register
  (init 0xFFFFFFFF, NO final complement), and `crc_reg` is what a `crc32` helper returns *before* the final
  `~`. Stored checksum word = little-endian at boot2[252:256].
- **Vector table offset:** in a QMK/ChibiOS RP2040 image, boot2 occupies image[0,256) and the ARM vector
  table starts at 0x100 — the initial SP word is at image+0x100 and the reset vector at image+0x104 (a real
  image: sp=0x20040400, pc=0x100002c5).
- **Vector-table plausibility gate:** SRAM is `[0x20000000, 0x20042000)` (264 KiB). A staged image is
  treated as plausibly bootable when the initial SP is word-aligned and inside that range, and the reset
  vector is odd (Thumb), inside the image, and past the vector table. Both are O(1) reads, so the COMMIT
  callback can run them inside the 20 ms window.
- **Step-4 verified values (same build on both halves):** image len 93692 B (406 chunks), boot2 checksum
  `0xD58F0B07`, sp `0x20040400`, reset `0x100002C5`; rounded staging erase = 94208 B (23 sectors). The boot2
  checksum was re-derived with `pad_checksum`'s exact expression and matches the raw-table formula in the
  module. The **whole-image CRC-32 is build-specific** — `via.c` embeds `QMK_BUILDDATE` (build date/time) in
  the image, so it changes on every rebuild; the runtime check is that the master/slave/readback CRCs match
  each other, not a fixed value.
- **The commit runs the secondary's *currently running* firmware, not the image being written.** So any
  side-effect added to the commit routine (e.g. erasing the EEPROM) only takes effect once the secondary
  already runs a firmware that contains it. A first clone of a new firmware onto an old-firmware secondary
  runs the OLD commit routine (no new side-effects); the feature applies from the next clone onward.
- **Commit must run from housekeeping, not the callback:** an erase+copy takes seconds with IRQs off, so the
  COMMIT callback only records the request (and runs the O(1) bootability/read-back checks); the slave's
  `housekeeping_task` runs the RAM-resident commit after a short delay, long enough for the master to receive
  the acknowledgment. Freezing the link while the secondary resets is expected; the master's base sync
  retries and reconnects when the secondary is back.
- **GP17** is the BKB status LED (`RP2040_BOOTLOADER_DOUBLE_TAP_RESET_LED`), not used by matrix/RGB, free
  after boot; QMK chibios `GP17` is literally the numeral `17`, so `sio_hw->gpio_set/clr = 1u<<17` works to
  pulse it from RAM-resident code (configure SIO output beforehand via pico `gpio_set_function` /
  `gpio_set_dir`). Observed on the dilemma 4x6_4 procyon: driving GP17 produced **no visible output**, so do
  not rely on it as a user-facing indicator.

## Dilemma Bootmagic

- `keyboards/bastardkb/dilemma/4x6_4_procyon/keyboard.json` enables full bootmagic with
  `split.bootmagic.matrix = [5, 0]`. That value generates **only** `BOOTMAGIC_ROW_RIGHT 5` /
  `BOOTMAGIC_COLUMN_RIGHT 0` in `info_config.h`; the left half falls back to
  `quantum/bootmagic/bootmagic.h` defaults `BOOTMAGIC_ROW 0` / `BOOTMAGIC_COLUMN 0`. `bootmagic.c:46-56`
  checks `matrix_get_row(row) & (1<<col)` on boot and calls `bootloader_jump()` — so **holding L00 (left
  top-left) at boot puts that half into RP2040 ROM BOOTSEL before QMK runs** (no RGB/keys/transport).
  Confirmed identical on the stock image. Corollary: hold L00 while plugging a half's own USB to enter
  BOOTSEL for flashing without double-tap. (The right half's bootmagic key is R50.)

## Misc

- `.build/` has per-target `obj_*` dirs and the generated `src/` intermediates (community modules, `.inc` files).
- CRC: `quantum/crc.c` provides only a **weak, whole-buffer** `crc8()` (`crc.h`; poly 0x31, init 0xff, no
  incremental variant); the tree has no CRC-32 helper, so a module wanting an incremental CRC-32 implements
  its own. `crc.o` **is** compiled into the vendor build (present under `.build/obj_*/quantum/crc.o`), and
  `-Werror` is on by default (`builddefs/common_rules.mk:81-83`; nothing in tree/keymaps/userspace sets
  `ALLOW_WARNINGS=yes`), so a successful build already proves a warning-free compile.
- `qmk compile` copies the produced UF2 over `<QMK_HOME>/<name>.uf2` **and**
  `<QMK_USERSPACE>/<name>.uf2` ("Copying … to qmk_firmware/userspace folder"), silently overwriting any
  golden kept at those two paths — keep goldens under a distinct filename.
- Keep the CLI output clean: this repo builds with `qmk compile`; check `qmk env` for QMK_HOME/USERSPACE
  resolution (userspace = `/home/qlebastard/qmk_userspace_bstiq`).
- `qmk_module.json` `features` (e.g. `"features": {"console": true}`) flow into the generated
  `community_rules.mk` via `generate_features_rules`, so a module can self-require a feature (console,
  deferred_exec, ...) without editing the keymap.

## Module EEPROM datablocks

- Declare in the module `config.h`: `#define EECONFIG_MODULE_<FOLDER>_DATA_SIZE <n>` (optional
  `..._DATA_VERSION`, default = DATA_SIZE). Generator: `lib/python/qmk/cli/generate/community_modules.py`
  `_render_eeconfig_declarations` (into generated `community_modules.h`, already included by modules) and
  `_render_eeconfig_implementation` (into `community_modules.c`). Example: `modules/qmk/nvm_test`.
- API: `eeconfig_read_<folder>_datablock(void *data, uint32_t off, uint32_t len)`,
  `eeconfig_update_<folder>_datablock(const void *, off, len)`, `eeconfig_is_<folder>_datablock_valid()`,
  weak `eeconfig_init_<folder>_datablock()` (default zero-fills + writes the version word). Storage =
  4-byte version word + data.
- Init order (`keyboard.c` `quantum_init`): `eeconfig_init()` if EEPROM not enabled (calls every module's init),
  then `bootmagic()`, then `eeconfig_prepare_datablocks()` → re-inits any invalid module datablock. So by
  `keyboard_post_init`/`housekeeping_task` a datablock is **always valid**; "fresh" shows up as zeroed data,
  not as invalid. Detect "new" by comparing stored data against an expected value.
- Wear-leveling region macros: `platforms/chibios/drivers/wear_leveling/wear_leveling_rp2040_flash_config.h`
  (`WEAR_LEVELING_RP2040_FLASH_BASE` = `PICO_FLASH_SIZE_BYTES - WEAR_LEVELING_BACKING_SIZE`, backing 8192);
  includable from a module as `"wear_leveling/wear_leveling_rp2040_flash_config.h"` (same path
  `quantum/wear_leveling/wear_leveling_drivers.h` uses). Erasing that region with `flash_range_erase` makes the
  next boot see an un-enabled EEPROM → full `eeconfig_init()`.
- Unverified: whether an RP2040 UF2 drag-and-drop erases the EEPROM region (bootrom likely only erases sectors
  the UF2 covers). Don't rely on it either way.
