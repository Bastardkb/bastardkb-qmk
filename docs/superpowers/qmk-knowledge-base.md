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
  runs the wire transaction, then copies the s2m result back out.
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
- Half-duplex vendor serial, 230400 baud on this board (`SERIAL_USART_SPEED`); `SERIAL_DRIVER = vendor`.
  Throughput is wire-bound: ~23 KB/s. The PIO driver derives its clock from `SERIAL_USART_SPEED`
  (`serial_vendor.c:358`), so raising the baud is a config change.
- Each failed `serial_transport_receive` blocks its caller for up to `SERIAL_USART_TIMEOUT` (default 20 ms,
  `serial_usart.h:43-45`).
- `connection_errors` (hence `is_transport_connected()`, `split_util.c:230-231`) is only updated by the base
  split-sync path (`transport_master`), **not** by raw transactions. `connection_errors` starts at 0
  (= "connected") and resets to 0 on every successful base sync; it increments on a failed one and trips
  "disconnected" at `SPLIT_MAX_CONNECTION_ERRORS` (default 10, `split_util.c:46-47`).
- Split sync is serviced from the main loop: `quantum/matrix_common.c:112` calls `transport_slave(...)` on
  the slave half. A blocking hook therefore also kills the split link.

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
- **Requirements when calling them:** interrupts disabled (`save_and_disable_interrupts` /
  `restore_interrupts` are static inline in `hardware/sync.h`); the `data` buffer passed to
  `flash_range_program` must be in RAM; `flash_range_program` offset must be 256-byte aligned and `count` a
  multiple of 256; `flash_range_erase` offset/count must be 4 KiB sector aligned (64 KiB block rounding is
  handled by the bootrom — procyon is single-core `CRT0_EXTRA_CORES_NUMBER=0`, so no core 1 concern).
- **Callers may stay in flash** (XIP is re-enabled between each op); only code executing *during* an op must
  be RAM-resident. Exception: if the active region is being erased/rewritten, the loop between ops must ALSO
  be RAM (its own code is in the erased region), so such a routine must be `__no_inline_not_in_flash_func`
  (→ `.time_critical.*` → RAM `.data`) and must not call memcpy/print/etc. The linker may emit a flash
  `*_veneer` stub for cross-TU calls into it — harmless (runs with XIP up).
- **Vector-table hazard:** `crt0_v6m.S` sets VTOR to `_vectors` in **flash** (sector 0). Erasing sector 0
  while running = next IRQ fetches `0xFF` → hard fault. So sector 0 must only be erased inside an
  interrupts-off commit.
- **Linker symbols:** `__flash_binary_start` / `__flash_binary_end` / `__boot2_start__` from
  `RP2040_FLASH_TIMECRIT.ld`. Image length = `&__flash_binary_end - XIP_BASE`.
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
- Keep the CLI output clean: this repo builds with `qmk compile`; check `qmk env` for QMK_HOME/USERSPACE
  resolution (userspace = `/home/qlebastard/qmk_userspace_bstiq`).
- `qmk_module.json` `features` (e.g. `"features": {"console": true}`) flow into the generated
  `community_rules.mk` via `generate_features_rules`, so a module can self-require a feature (console,
  deferred_exec, ...) without editing the keymap.
