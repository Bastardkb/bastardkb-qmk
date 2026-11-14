# `bk_split_flash.c` High and Medium Risk Analysis

## Scope

This document evaluates the high- and medium-priority risks identified during
the code review of:

`modules/bastardkb/bk_split_flash/bk_split_flash.c`

The analysis follows the actual QMK and RP2040 implementations in this tree.
It does not compare the implementation with the plan, add features, run a
build, run a simulation, or make firmware changes.

Power-loss recovery is explicitly out of scope for this assessment. The
secondary is assumed to remain powered throughout the commit operation.

The risks covered are:

1. Commit acknowledged before the commit operation completes.
2. Flash page programming performed inside the split transaction callback.
3. Unsynchronized module state shared by the split thread and housekeeping.
4. Lazy CRC-table initialization from multiple execution contexts.
5. No integrity verification after copying staging into the active image.

## Findings Summary

| Risk | Actual threat | Current verdict |
| --- | --- | --- |
| Early commit acknowledgment | Mainly a power-loss recovery gap | Out of scope; no fix required for the current failure model |
| Page programming in callback | Real timeout margin issue, but currently supported by measured timing | No immediate fix for this board; keep timing margin under review |
| Module state synchronization | Real absence of formal synchronization, but normal state ordering prevents the important overlaps | No immediate fix; document the serialization assumptions if code changes later |
| Lazy CRC-table initialization | Theoretical race with identical deterministic writes | No fix currently required |
| No post-copy verification | Theoretical active-write failure is not detected before reset | Low-priority limitation; current implementation is acceptable |

## 1. Commit Acknowledged Before Completion

### Module code

The slave callback validates the stream and then only schedules the commit:

- `bk_split_flash.c:341-369`
  - Checks stream completion, received CRC, read-back CRC, and bootability.
  - Sets `bk_commit_len`.
  - Sets `bk_commit_pending = true`.
  - Returns `BK_SPLIT_FLASH_ACK_COMMIT`.

The actual erase, copy, marker write, and reset happen later:

- `bk_split_flash.c:948-966`
  - `housekeeping_task_bk_split_flash()` waits for the commit delay.
  - It then calls `bk_split_flash_commit()`.

The master treats the acknowledgment as success immediately:

- `bk_split_flash.c:916-940`
  - On `BK_SPLIT_FLASH_ACK_COMMIT`, it calls `bk_trigger_persist()`.
  - It prints the successful commit marker.
  - It enters the success indicator state.

### QMK call-site behavior

The serial protocol invokes the callback and sends its response immediately
after it returns:

- `platforms/chibios/drivers/serial_protocol.c:74-90`
  - Receives the request.
  - Calls `transaction->slave_callback(...)`.
  - Sends the response buffer.

There is no QMK-level completion callback after the later housekeeping work.
The response therefore confirms only that the callback accepted and scheduled
the operation.

The main loop invokes housekeeping only after the normal protocol work for
that iteration:

- `quantum/main.c:47-81`
- `quantum/keyboard.c:436-444`

This confirms that the commit is intentionally asynchronous and that the
master's persistence occurs before the secondary has reset successfully.

### Actual threat

The main failure window is power loss or an unexpected reset after the
acknowledgment and before successful reboot. That case is explicitly excluded
from this assessment.

With stable power, the normal success path is not affected: the delayed
housekeeping commit runs after the response, and the secondary resets as
intended.

### Verdict

**Real power-loss recovery gap, not an ordinary-path bug.**

Power-loss recovery is out of scope, so this does not require a fix for the
current use case. The existing 100 ms delay only gives the response time to
reach the master; it does not close the power-loss completion window.

The acknowledgment is sufficient for the stable-power success path.

## 2. Flash Page Programming Inside the Callback

### Module code

Each completed data chunk eventually calls:

- `bk_split_flash.c:373-395`
  - validates the chunk CRC-8;
  - updates the rolling receive CRC;
  - calls `bk_stage_append()`.

When a page is full, `bk_stage_append()` calls:

- `bk_split_flash.c:189-196`
- `bk_split_flash.c:161-171`

`bk_page_program()` performs all of the following before the callback returns:

1. Disables interrupts.
2. Programs a 256-byte flash page.
3. Re-enters XIP.
4. Reads the programmed page back.
5. Updates the staging read-back CRC.
6. Updates the active-image CRC.

### QMK and RP2040 behavior

The QMK serial thread calls the callback synchronously and does not send the
response until the callback returns:

- `platforms/chibios/drivers/serial_protocol.c:81-90`

The receive side uses `SERIAL_USART_TIMEOUT` for each blocking receive:

- `platforms/chibios/drivers/serial_usart.c:158-165`
- `platforms/chibios/drivers/vendor/RP/RP2040/serial_vendor.c:266-309`

The RP2040 flash shim is RAM-resident and validates page alignment, but it has
no error return for the ROM page-program operation:

- `lib/pico-sdk/src/rp2_common/hardware_flash/flash.c:86-106`

The knowledge-base timing observation for this board is that a 256-byte page
program takes approximately 1-5 ms, while the default serial receive timeout
is 20 ms. That leaves margin on the validated hardware, and the read-back CRC
work is only 256 bytes.

The vendor PIO driver also uses a high-drive push-pull transmit configuration
for the half-duplex pin:

- `platforms/chibios/drivers/vendor/RP/RP2040/serial_vendor.c:325-339`
  - 12 mA drive strength;
  - fast slew;
  - output enable and pull-up configuration.

### Actual threat

The callback is not unbounded: it handles one 256-byte page at a time, not the
whole image. The current implementation is therefore materially safer than a
whole-image read-back or erase in the callback.

The remaining threat is timing margin. A callback can approach or exceed the
transport timeout if flash programming is slower than observed, if the target
flash differs, or if additional work is added to the callback later. A timeout
can also occur after the flash operation has already completed, leaving the
master unaware of the exact slave state.

### Verdict

**Real conditional timing risk, currently acceptable on the validated board.**

No immediate fix is required if the same RP2040/flash configuration and timing
margin are retained. Moving page programming out of the callback would be a
larger protocol redesign, not a necessary correction based on the current
repository evidence.

The important constraint is that the callback must not grow. In particular,
whole-image verification or erase work must remain outside it.

## 3. Module State Shared Across Execution Contexts

### Module code

The callback executes in the split protocol thread:

- `bk_split_flash.c:260-396`

The same module state is consumed or modified by housekeeping:

- `bk_split_flash.c:410-427`
- `bk_split_flash.c:441-470`
- `bk_split_flash.c:948-967`

Some cross-context fields are explicitly volatile:

- staging preparation fields at `bk_split_flash.c:217-220`;
- commit scheduling fields at `bk_split_flash.c:225-228`.

Other fields used by both paths are ordinary objects, including parts of
`bk_slave`, page counters, CRC accumulators, the page buffer, and marker state.

### QMK synchronization behavior

The split protocol takes the QMK shared-memory mutex before accessing the
transaction buffers:

- `platforms/chibios/drivers/serial_protocol.c:63-65`
- `platforms/chibios/drivers/serial_protocol.c:120-122`

That mutex protects shared split transport memory. It does not protect the
module's private globals. The implementation is a ChibiOS mutex:

- `platforms/chibios/synchronization_util.c:7-25`

The slave callback runs on the dedicated high-priority `SlaveThread`:

- `platforms/chibios/drivers/serial_protocol.c:17-39`

Housekeeping runs on the main loop thread:

- `quantum/main.c:47-81`
- `quantum/keyboard.c:436-444`

Therefore, the two contexts can preempt one another. There is no QMK mutex
automatically covering `bk_slave` or `bk_page`.

### Actual threat

There is a genuine absence of formal synchronization, but the normal state
machine substantially reduces the practical overlap:

- The marker task exits once `bk_slave.active` is true.
- The commit task is gated by `bk_commit_pending` and runs only after the
  callback has acknowledged the request.
- Interrupts are disabled around erase/program operations.
- The page buffer is used for data staging before commit and is reused for the
  marker only when the stream is inactive.
- The preparation callback sets `busy` before the housekeeping erase, and the
  erase disables interrupts.

The remaining concern is a race at state boundaries. For example, the marker
task can read an inactive state immediately before a `BEGIN` callback changes
it, or a callback can preempt housekeeping between a check and a subsequent
use of shared state. The likely result is an incorrect state decision or a
stale marker, not direct corruption during the normal serialized stream.

The `volatile` qualifiers also provide visibility for the fields where they
are used, but they do not make compound state transitions atomic and do not
protect the non-volatile fields.

### Verdict

**Real formal-concurrency weakness, low practical threat under the current
state ordering.**

No immediate fix is required based on the observed control flow. A fix becomes
important if the module later adds concurrent marker scanning, retries from
housekeeping, multiple streams, or any callback that leaves the current strict
phase boundaries.

The current implementation should be treated as relying on serialization by
protocol phase, not as generally thread-safe module state.

## 4. Lazy CRC-Table Initialization

### Module code

The table and readiness flag are global:

- `bk_split_flash.c:53-54`

Both update functions lazily build the table:

- `bk_split_flash.c:67-74`
- `bk_split_flash.c:108-115`

The functions are reachable from both the master housekeeping path and the
secondary callback/housekeeping paths.

### Actual execution model

The master performs image CRC work in housekeeping. The secondary can perform
CRC work in both the transaction callback and the marker task. The callback is
on the high-priority split thread, while marker CRC work is on the main loop.

There is no mutex or atomic operation around `bk_crc32_table_ready`.

However, both possible initializers write the same deterministic table values
to the same indices. The flag is set only after the builder's loop completes.
The normal outcome of an interruption during the build is therefore another
complete build of the same table, not a table containing mixed algorithms or
input data.

The practical risk is a C-level data race and lack of a formal memory-ordering
guarantee, not a likely hardware-visible CRC error in this single-core
execution model.

### Verdict

**Theoretical race, not an actionable current bug.**

No fix is currently required. The table could be built before concurrent work
begins, but doing so would be a preventive cleanup rather than a response to a
demonstrated failure path.

## 5. No Integrity Verification After the Active-Image Copy

### Module code

The commit routine:

- erases the active image and EEPROM region;
- copies all non-boot2 pages from staging;
- copies boot2 last;
- writes the provenance marker;
- resets the secondary.

References: `bk_split_flash.c:478-540`.

The source pages are read from staging into the RAM page buffer and passed to
`bk_flash_program()`. There is no read-back or CRC of the newly programmed
active region before reset.

### RP2040 flash behavior

The RP2040 `flash_range_program()` shim performs the ROM program operation and
returns `void`:

- `lib/pico-sdk/src/rp2_common/hardware_flash/flash.c:86-106`

The module's pre-commit checks are strong for the staging image:

- received-stream CRC versus master CRC;
- staging read-back CRC versus master CRC;
- boot2 checksum;
- stack-pointer and reset-vector plausibility.

Those checks prove the source image before copying. They do not prove that
every subsequent active-region page was successfully programmed.

Programming boot2 last is an important safety measure. If the copy is
interrupted before boot2 is written, the image should not become normally
bootable. It does not provide rollback after active sector 0 has been erased,
and it does not detect a failed later page write after boot2 is programmed.

### Actual threat

This is a real but fault-dependent integrity gap:

- ordinary successful flash operations are not affected;
- a flash-program failure is not reported by the primitive;
- the active image is not read back before reset.

In simple terms, the firmware verifies the image in the staging area before
installation, but does not verify the installed active copy before rebooting.
If a non-boot2 page were written incorrectly despite stable power, the
secondary could boot a damaged image without the master knowing.

The existing design substantially limits this risk:

- every staging page is read back after programming;
- the complete staging image is checked before commit;
- boot2 is programmed last;
- the same RP2040 flash primitive successfully wrote the staging image before
  the active-image copy begins.

The remaining case is therefore a rare flash-program failure during the
second write phase, not an ordinary transport or staging-integrity failure.

### Verdict

**Real theoretical fault path, low priority for the current use case.**

No fix is currently required. A true fix would require more than an extra
check in the existing callback: it would need active-image read-back or a
recovery strategy such as a retained known-good image, a commit journal, or a
verification and retry scheme that remains safe after sector 0 is erased.

With stable power and the current hardware, the staging verification and
boot2-last ordering make the existing implementation reasonable.

## Overall Conclusion

The main commit risk is the boundary where the master receives a successful
acknowledgment before the secondary has completed its destructive flash
rewrite. That matters primarily for power-loss recovery, which is outside the
current scope.

The page-program callback risk is bounded and currently supported by the
repository's measured timing. The synchronization and CRC initialization
concerns are mainly formal correctness weaknesses whose practical impact is
limited by the current phase ordering and single-core execution. The absence
of post-copy verification is a real theoretical limitation, but the current
staging checks, boot2-last ordering, stable-power assumption, and validated
hardware make it low priority.

For the current use case, **things are fine and no firmware change is
recommended**. The only risks that merit future reconsideration are:

1. whether power-loss recovery becomes a product requirement; and
2. whether the current page-program timing margin must support hardware or
   flash variants beyond the validated board.
