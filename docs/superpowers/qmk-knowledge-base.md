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

## Keyboards / build target

- Target board: `bastardkb/dilemma/4x6_4_procyon` (folder under `keyboards/bastardkb/dilemma/`,
  underscore form, NOT `4x6_4/procyon`).
- Board `keyboard.json` sets e.g. split protocols, `PICO_FLASH_SIZE_BYTES` etc; check it for
  flash-layout numbers. The MK measured stock image is `bastardkb_dilemma_4x6_4_procyon_vendor.uf2`.
- The board's real keymap is the userspace `vendor` keymap (see userspace); "default" keymap is a bare C
  keymap with no `keymap.json`. Phase UF2s are built with `-km vendor` (user-confirmed 2026-11-12) so the
  module is already listed + console is on.

## Misc

- `.build/` has per-target `obj_*` dirs and the generated `src/` intermediates (community modules, .inc files).
- Half-duplex vendor serial, 230400 baud on this board; SERIAL_DRIVER = vendor.
- Keep the CLI output clean: this repo builds with `qmk compile`; check `qmk env` for QMK_HOME/USERSPACE
  resolution (userspace = `/home/qlebastard/qmk_userspace_bstiq`).
- `qmk_module.json` `features` (e.g. `"features": {"console": true}`) flow into the generated
  `community_rules.mk` via `generate_features_rules`, so a module can self-require a feature (console,
  deferred_exec, ...). That is the clean way for the probe/echo module to guarantee console without
  editing the keymap.
