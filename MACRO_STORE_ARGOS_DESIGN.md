# Macro Store — Argos Host Design

## Goal

Define how **Argos** (host / webapp) **gets, sets, and clears** keyboard macros **by slot id** over USB Raw HID, using **stock VIA** only:

1. VIA macro buffer commands `0x0C`–`0x10` on the wire
2. Host-side pack/unpack of the shared NUL-terminated EEPROM blob
3. Host-side encode/decode between a QMK Configurator-style action list and send-string bytes

The keyboard runs ordinary QMK/VIA firmware. There is **no** custom Argos macro command and **no** firmware-side slot packing for this path.

## Non-goals

- Custom Argos HID framing (`0x90` + `argos_id_macro` + sub-commands)
- Any new or modified keyboard firmware for macros
- Per-slot maximum size or a host API that returns per-slot byte lengths
- EEPROM layout changes or validity-flag semantics different from VIA
- Full VIA lighting/keymap UI (unless Argos already has it)
- Playback implementation (key press → send-string stays on firmware / VIA)
- Sending structured JSON over HID — only encoded send-string bytes go in the blob

### Obsolete companion

[`MACRO_STORE_QMK_DESIGN.md`](MACRO_STORE_QMK_DESIGN.md) described a custom Argos firmware module (`argos_id_macro`, id-level GET/SET/CLEAR on device). That path is **obsolete and not used**. This document is authoritative for Argos macros.

---

## Storage model (host packs; device stores a blob)

Macros live in one contiguous buffer on the device: null-terminated send-string slots packed back-to-back. The last byte of that buffer is a validity flag (`0` = safe to play; non-zero = write in progress / invalid ⇒ playback no-ops).

There is **no per-macro max length** in stock VIA/QMK. Slots share one blob (`DYNAMIC_KEYMAP_MACRO_EEPROM_SIZE` on device). One large slot simply leaves less room for the others. Usable payload space is `buffer_size - 1` (last byte reserved for the validity flag).

| Property | How the host learns / uses it |
|----------|-------------------------------|
| Max slot count | VIA `0x0C` `get_count` (typically 16) |
| Total blob size | VIA `0x0D` `get_buffer_size` (16-bit) |
| Slot content | Opaque send-string **bytes** (ASCII + `SS_*` escapes); no interior NUL |
| Editor / API model | QMK Configurator-style action list; encode before SET, decode after GET |
| Blob packing / validity | **Host-owned** (same dance as the VIA app) |
| Playback keycodes | `QK_MACRO_n` / `MC_n` (`0x7700 + n`). Macro ops alone do not bind keys |

HID traffic uses **buffer offsets** into the packed blob. Incomplete writes must leave the validity flag non-zero until the host finishes (or the device is recovered); while the flag is non-zero, playback is disabled.

---

## Transport

- USB Raw HID, 32-byte reports (plus report ID `0x00` on the host write path, as in VIA)
- Request/response: send a report, read the echoed/filled report back
- Command byte is a stock VIA `via_command_id` — **not** an Argos prefix

### VIA macro commands

| Cmd | Name | Role |
|-----|------|------|
| `0x0C` | `id_dynamic_keymap_macro_get_count` | Slot capacity |
| `0x0D` | `id_dynamic_keymap_macro_get_buffer_size` | Total blob size (bytes) |
| `0x0E` | `id_dynamic_keymap_macro_get_buffer` | Chunked read of the blob |
| `0x0F` | `id_dynamic_keymap_macro_set_buffer` | Chunked write of the blob |
| `0x10` | `id_dynamic_keymap_macro_reset` | Clear all macros / erase blob |

Multi-byte offsets and sizes on the VIA macro path are **big-endian** (high byte then low byte), matching QMK `via.c` and the VIA app.

### Packet layouts

#### `get_count` (`0x0C`)

- **Request:** `[0x0C]`
- **Response:** count in the next byte

#### `get_buffer_size` (`0x0D`)

- **Request:** `[0x0D]`
- **Response:** size as 16-bit BE (`hi`, `lo`)

#### `get_buffer` (`0x0E`)

| Field | Meaning |
|-------|---------|
| offset | 16-bit BE offset into the blob |
| size | Bytes requested (1…28) |
| data (response) | Up to `size` bytes starting at offset |

#### `set_buffer` (`0x0F`)

| Field | Meaning |
|-------|---------|
| offset | 16-bit BE offset into the blob |
| size | Bytes in this chunk (1…28) |
| data | Chunk bytes |

#### `reset` (`0x10`)

- **Request:** `[0x10]`
- Clears all macro storage (VIA / QMK `dynamic_keymap_macro_reset`)

---

## Host layering

Argos exposes an **id-oriented** API to the UI. Underneath it mirrors the VIA app: whole-blob read/write over `0x0E` / `0x0F`.

```
UI / editor (structured action lists)
        ↓ encode / decode
Id API: get_count, get_buffer_size, get(id), set(id), clear(id), list
        ↓ pack / unpack NUL-terminated slots
VIA buffer commands 0x0C–0x10
        ↓
Stock QMK/VIA firmware
```

The host never asks the firmware for “slot id N’s bytes” — it reads the blob, finds slot N by scanning NULs, and (on write) rebuilds the full packed image.

---

## Structured macro model (QMK Configurator schema)

UI and host APIs edit macros as an **array of fragments**, matching QMK Configurator / `keymap.json` macros (`docs/feature_macros.md`). The device never sees this structure — only the encoded send-string bytes inside the packed blob.

### Fragment types

| Fragment | Shape | Meaning |
|----------|--------|---------|
| Text | string | Type those characters |
| Tap | `{ "action": "tap", "keycodes": ["F1"] }` | Tap one key, or a chord if multiple |
| Down | `{ "action": "down", "keycodes": ["LSFT"] }` | Key down (hold) for each listed key |
| Up | `{ "action": "up", "keycodes": ["LSFT"] }` | Key up for each listed key |
| Delay | `{ "action": "delay", "duration": 1000 }` | Pause playback `duration` milliseconds |
| Beep | `{ "action": "beep" }` | Optional schema parity → byte `0x07` (`\a`) |

Keycode names use **no `KC_` prefix** (QMK style: `"LCTL"`, `"ENT"`, `"C"`, `"LSFT"`). Host maps name → basic HID usage byte for encode. Only basic keycodes are allowed (same restriction as QMK send-string / Configurator macros).

Example slot (structured):

```json
[
  {"action": "down", "keycodes": ["LSFT"]},
  "hi",
  {"action": "up", "keycodes": ["LSFT"]},
  {"action": "delay", "duration": 500},
  {"action": "tap", "keycodes": ["ENT"]},
  {"action": "tap", "keycodes": ["LCTL", "C"]}
]
```

### Encode (structured → send-string bytes)

Run before every SET. Rules mirror QMK `lib/python/qmk/keymap.py` (`_generate_macros_function`):

| Fragment | Bytes |
|----------|--------|
| `"hello"` | ASCII as-is; reject if it contains `0x00` or bare `0x01` |
| `delay` / `duration` | `01 04` + decimal ASCII of duration + `\|` (`7C`) |
| `beep` | `07` |
| `tap` with 1 key | `01 01 <kc>` |
| `tap` with N>1 keys | `SS_DOWN` each of first N−1, `SS_TAP` last, `SS_UP` first N−1 in reverse |
| `down` | `01 02 <kc>` for each keycode in order |
| `up` | `01 03 <kc>` for each keycode in order |

Escape constants: prefix `0x01` (`SS_QMK_PREFIX`); tap/down/up/delay codes `0x01` / `0x02` / `0x03` / `0x04`.

Examples:

| Intent | Structured | Encoded (hex) |
|--------|------------|---------------|
| Text | `"Hi"` | `48 69` |
| Tap Enter | `{"action":"tap","keycodes":["ENT"]}` | `01 01 28` |
| Ctrl+C chord | `{"action":"tap","keycodes":["LCTL","C"]}` | `01 02 E0` `01 01 06` `01 03 E0` |
| Hold Shift + type | `down LSFT`, `"hi"`, `up LSFT` | `01 02 E1` `68 69` `01 03 E1` |
| Delay 500 ms | `{"action":"delay","duration":500}` | `01 04 35 30 30 7C` |

Note: typing `"C"` as text uses ASCII `0x63` and the send-string LUT; tapping keycode `"C"` uses `SS_TAP` with HID `0x06`. Prefer key actions when modifiers/chords matter.

### Decode (send-string bytes → structured)

Run after GET (and when loading for the editor):

1. Walk bytes; accumulate ASCII runs as string fragments (stop before `0x00` / `0x01`).
2. On `0x01`, read the next code and parse tap/down/up (one keycode byte) or delay (digits until non-digit, conventionally `|`).
3. Emit `{action, keycodes}` / `{action, duration}` objects; map HID bytes back to Configurator keycode names when possible.
4. Malformed escapes → surface as error or leave an opaque undecoded span; do not invent actions.

Round-trip goal: encode(decode(bytes)) preserves wire bytes for well-formed send-string produced by this encoder (and ideally by VIA / QMK tools).

---

## Pack / unpack (host)

### Read whole blob

1. `get_buffer_size` → `N`
2. Repeatedly `get_buffer` with increasing offset, up to 28 bytes per report, until `N` bytes are collected
3. Treat `bytes[0 … N-2]` as packed slots; `bytes[N-1]` is the validity flag

### Split into slots

Scan the usable region for NUL terminators. Slot `id` is the byte range after skipping `id` NULs, up to the next NUL (exclusive). Missing trailing slots are empty. Empty slot = immediate NUL (zero-length payload).

Do not expose per-slot lengths as a public Argos capability; lengths exist only as an implementation detail while packing.

### Rebuild packed image

Given `count` slots as encoded byte arrays (no interior NUL, no trailing NUL in the arrays themselves):

1. Concatenate `slot0 + 0x00 + slot1 + 0x00 + …` for all `count` slots (pad missing ids as empty)
2. Ensure the packed length fits in `N - 1` bytes; reject if not
3. Zero-fill unused bytes in the usable region as needed so the blob stays well-defined
4. Validity byte is written separately during the commit dance (below), not as part of slot payloads

---

## Id-level host logic

### Capabilities

| Capability | Implementation |
|------------|----------------|
| Get slot count | VIA `0x0C` |
| Get total buffer size | VIA `0x0D` |
| Get one slot | Read blob → unpack → decode slot `id` |
| Set one slot | Encode → rebuild full blob → VIA write dance |
| Clear one slot | Same as set with empty encoded bytes |
| List all slots | Count + get/decode each id |

### Read one macro

1. Optionally check `get_count`; reject if `id` is out of range.
2. Read the full blob; if validity flag ≠ `0`, treat as busy / unsafe and surface an error (do not pretend slots are valid).
3. Unpack slot `id`’s send-string bytes (empty if absent).
4. **Decode** into the structured action list for the editor.

### List all macros

1. `get_count`
2. Read blob once
3. For each id in range, unpack + decode

### Set one macro

1. Start from a structured action list (Configurator schema).
2. **Encode** to send-string bytes; reject text fragments that contain interior NUL or bare `0x01`; reject unknown / non-basic keycodes.
3. `get_count` + `get_buffer_size`; reject out-of-range id.
4. Read current blob (or start from empty slots after `reset` if preferred); unpack all slots.
5. Replace slot `id` with the new encoded bytes; rebuild the packed image.
6. Reject if packed length > `buffer_size - 1`.
7. Commit with the VIA write dance (below).
8. Only one in-flight macro write at a time.

### Clear one macro

Same as SET with empty encoded bytes for that id (later slots stay intact in the rebuilt blob). Clearing **all** macros may use VIA `reset` (`0x10`).

### VIA write dance (required on every commit)

Mirror the VIA app (`setMacroBytes`):

1. Optionally `reset` (`0x10`) before a full rewrite (VIA does this before writing).
2. `set_buffer` at offset `buffer_size - 1`, size `1`, data `0xFF` (mark invalid).
3. Write the packed usable bytes with `set_buffer` in chunks of at most 28 bytes at increasing offsets.
4. `set_buffer` at offset `buffer_size - 1`, size `1`, data `0x00` (mark valid) — always, including on failure paths after step 2 when possible, so the device is not left permanently muted.

Treat chunk `data` as raw octets (binary-safe); `0x01` escapes must survive intact.

### Bind a key to a macro

1. Write or clear slot `n` as above.
2. If the host manages keymaps, assign keycode `0x7700 + n` (`QK_MACRO_n` / `MC_n`) via ordinary VIA keymap commands.

---

## Coexistence

| Peer | Notes |
|------|--------|
| Stock VIA firmware | Sole device target for this design |
| VIA Configurator | Same Raw HID macros API and same blob; last complete write wins |
| Obsolete Argos firmware macro module | Not required; do not send `0x90` / `argos_id_macro` for macros |

Do not run two writers concurrently without refresh. If the validity flag is non-zero (foreign mid-write or abandoned write), refuse id-level reads/writes until recovered (`reset` or a complete write dance ending in `0x00`).

---

## Error handling / UX

| Condition | Host behavior |
|-----------|----------------|
| Slot id out of range | Reject |
| Interior NUL or bare `0x01` in a text fragment | Reject before encode/pack |
| Unknown or non-basic keycode in an action | Reject before encode |
| Malformed send-string on decode | Surface decode error; do not silently invent actions |
| Packed image exceeds `buffer_size - 1` | Reject; do not start the write dance |
| Validity flag ≠ `0` before read/write | Warn busy; offer retry or `reset` recovery |
| Transfer failure mid-write | Attempt to clear validity to `0x00` if safe; otherwise warn that playback may be disabled; restart full SET from the beginning |
| Unknown / unhandled VIA command | Firmware or protocol mismatch |

---

## Testing

1. `get_count` matches device capacity.
2. `get_buffer_size` matches device blob size; usable space is size − 1.
3. Set several ids → get round-trips (including payloads that force multi-chunk buffer writes).
4. Enlarge/shrink a middle slot → later ids still correct after reload.
5. Clear empties one slot; later slots intact.
6. Abandon a write after setting `0xFF` → playback disabled until a complete dance or reset.
7. Press `MC_n` after set → expected send-string output.
8. VIA Configurator can still read/write the same macros after Argos writes (and vice versa after refresh).
9. Round-trip text: `"hello"` → encode → SET → GET → decode → `"hello"`.
10. Tap / chord: `tap F1`; `tap LCTL+LALT+DEL` → correct `SS_*` bytes; `MC_n` plays expected keys.
11. Mod hold around text: `down LSFT` + `"hi"` + `up LSFT`.
12. Explicit down/up without text.
13. Delay: `delay 500` → `01 04 35 30 30 7C`; playback pauses ~500 ms.
14. Mixed macro longer than 28 bytes still round-trips through chunked buffer ops.
15. Reject interior NUL / bare `0x01` in text fragments before encode.
16. Binary-safe transport: buffer get/set preserve `0x01` escape sequences (not UTF-8-mangled).
17. Oversized rebuild (sum of slots > usable space) is rejected without corrupting a prior valid blob.

---

## Mapping to VIA / QMK

| Host concept | Device / VIA side |
|--------------|-------------------|
| `get_count` | `0x0C` → `dynamic_keymap_macro_get_count()` |
| `get_buffer_size` | `0x0D` → `dynamic_keymap_macro_get_buffer_size()` |
| Blob read/write | `0x0E` / `0x0F` → `dynamic_keymap_macro_get/set_buffer` |
| Clear all | `0x10` → `dynamic_keymap_macro_reset()` |
| Id get/set/clear | Host pack/unpack only |
| Configurator action list | Host-only; never sent on the wire as JSON |
| Encoded send-string bytes | Slot payloads inside the packed blob |
| Validity dance | Host writes last blob byte `0xFF` then `0x00` |
| Keycode `0x7700 + n` | `QK_MACRO_n` / `MC_n` → `dynamic_keymap_macro_send(n)` |
| Custom Argos macro HID | Not used |

References: `quantum/via.h`, `quantum/via.c`, `quantum/dynamic_keymap.c`, and the VIA app `KeyboardAPI` macro helpers (`getMacroBytes` / `setMacroBytes`).
