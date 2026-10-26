# ChargeScreen: partition, Victron lock, auto-cycle, Orion XS — design

Date: 2026-10-26
Base: open-source beta4.16 (`src/main.cpp`)
Flashing model: USB only (laptop). No web/OTA firmware updates.

## 1. Partition layout (single app, no OTA)

Replace `partitions.csv`:

```
# Name,   Type, SubType, Offset,   Size,
nvs,      data, nvs,     0x9000,   0x5000,
factory,  app,  factory, 0x10000,  0x300000,
spiffs,   data, spiffs,  0x310000, 0xF0000,
```

- `nvs` keeps its offset/size, so saved keys and settings survive the reflash.
- App slot grows from 1.25 MB to 3 MB. LittleFS shrinks to 960 KB (BLE capture CSV only).
- First boot after flashing reformats LittleFS (`LittleFS.begin(true)`); any old capture CSV is lost.

Remove all web-OTA code:
- `#include <Update.h>`, `#include <mbedtls/sha256.h>`
- `firmwareUpload*`, `firmwareHeaderChecked`, `firmwareHashActive`, `firmwareDigest`, `firmwareSha` globals
- `looksLikeEsp32FirmwareChunk`, `drawFirmwareUpdateWarning`, `handleFirmwareUpload`, `handleFirmwareUpdateDone`
- `/firmware` route registration
- Setup-page firmware upload form, the "Firmware updates must be…" hint text, and the `ota-ok` / `ota-failed` status messages

## 2. Victron device lock

Applies independently to three device kinds: shunt (record `0x02`), solar (record `0x01`), Orion XS (record `0x0F`).

State per kind:
- `lockedAddress` (String, normalised lowercase hex without separators; empty = unlocked), persisted in NVS keys `shunt_mac`, `solar_mac`, `orion_mac`.
- `candidateAddress` and `candidateCount` (RAM only).
- `lastLockedSeenMs` (RAM only).

Rules:
- **Unlocked:** an advert is a *good candidate* when the key-byte check passes, decryption succeeds, and the decoded values are plausible:
  - shunt: 5.0 V ≤ voltage ≤ 70.0 V and 0 % ≤ SOC ≤ 100 %
  - solar: battery voltage is NaN or 5.0–70.0 V
  - Orion XS: output voltage is NaN or 5.0–70.0 V
  
  A good candidate from the same address as the current candidate increments `candidateCount`. A good candidate from a different address resets the candidate to that address with count 1. A failed advert does not reset the candidate. At `candidateCount >= 3` the address is locked and saved to NVS. Decoded values from good candidates are displayed while unlocked (preserves current behaviour for first use).
- **Locked:** adverts of that record type from any other address are ignored entirely (no decode, no change to stats/error, no change to `lastXAdvertMs`). Adverts from the locked address are decoded normally and refresh `lastLockedSeenMs`.
- **Release:** the lock is cleared (RAM and NVS) when the locked address is not heard for 10 minutes (`VICTRON_LOCK_RELEASE_MS = 600000`), or when the corresponding key is changed or cleared via the setup page.
- **Unlocked, failed adverts:** a decode failure only updates the displayed error when there is no currently valid data, so a neighbour's advert never invalidates good readings.
- The existing `matchesConfiguredVictronAddress` / `matchesConfiguredSolarAddress` stubs are replaced by this logic.
- Serial log lines: `Victron <kind> locked to <address>`, `Victron <kind> lock released (<reason>)`.
- Setup page shows, per kind, `Locked to AA:BB:CC:DD:EE:FF` or `Searching`.

## 3. Auto-cycling screens

- New setting `cycleSeconds`: one of 0 (Off), 10, 20, 30, 60. Default 0. NVS key `cycle_secs`. Invalid stored values sanitise to 0.
- On-device: settings page 3 becomes three rows at the page-2 positions (y = 76, 116, 156): Labels, Grid, Cycle. Tapping the Cycle row advances Off → 10s → 20s → 30s → 60s → Off. Value text shows `-` for Off, otherwise e.g. `20s`.
- Setup page: `Screen cycle` dropdown with the same options.
- Behaviour:
  - Only advances when on a dashboard page (no settings, rotation, info, or capture screen active), the screen is awake, and `cycleSeconds > 0`.
  - Advances when both `now - lastPageChangeMs >= cycleSeconds*1000` and `now - lastTouchActivityMs >= cycleSeconds*1000`.
  - Swipes and cycling both reset `lastPageChangeMs`.
- Page visibility (applies to both swipe and cycle):
  - Battery page is always visible.
  - Solar page is visible when a solar key is saved.
  - Orion page is visible when an Orion key is saved.
  - Demo mode makes all pages visible.
  - If the current page becomes hidden (e.g. key cleared), switch to the battery page.
- `PAGE_COUNT` becomes 3. Page order: 0 battery, 1 solar, 2 Orion.

## 4. Orion XS screen

Decoding (record type `0x0F`, community reverse-engineered, matches `keshavdv/victron-ble` `orion_xs.py`). Decrypted payload, little-endian bit order via existing `readBits`:

| Bits | Field | Scale | Not-available |
|---|---|---|---|
| 0–7 | device state | enum | 0xFF |
| 8–15 | charger error | code | 0xFF |
| 16–31 | output voltage | 0.01 V (unsigned) | 0xFFFF |
| 32–47 | output current | 0.1 A (unsigned) | 0xFFFF |
| 48–63 | input voltage | 0.01 V (unsigned) | 0xFFFF |
| 64–79 | input current | 0.1 A (unsigned) | 0xFFFF |
| 80–111 | off reason | bitmask | — |

Minimum decrypted length: 14 bytes; shorter → error `short data`.

Data/settings:
- `OrionStats` struct mirroring `SolarStats` style; globals `orionStats`, `orionSeen`, `lastOrionAdvertMs`, `lastOrionDecodeMs`.
- Key stored in NVS `orion_key`; setup-page field "Orion XS key" in the Victron section; included in Clear Saved Keys and in the Info page "Keys" summary.
- Same 25 s signal-lost invalidation as solar.
- Demo values (saved, editable on the setup page): output current 32.0 A, output voltage 14.2 V, input voltage 13.4 V, input current 35.5 A; state Bulk. NVS keys `demo_oa`, `demo_ov`, `demo_iv`, `demo_ia`.

Screen layout (mirrors the solar page geometry):
- Arc: output current against a fixed 50 A full scale, clamped 0–50.
- Large value: output current, e.g. `32.0 A`, label `CHARGE`.
- 2×2 grid: input V (`INPUT`), output V (`OUTPUT`), input A (`IN AMPS`), output power W = Vout × Iout (`POWER`). NaN shows `--`.
- Status line, in priority order:
  1. `Error N` if charger error is non-zero and not 0xFF.
  2. If state is Off (0) and the off reason is non-zero, the first matching bit (values from victron-ble `OffReason`): `Engine off` (0x80, also covers 0x81), `No input` (0x01), `Switched off` (0x02 or 0x04), `Remote off` (0x08), `Protection` (0x10), `BMS off` (0x40), `Checking input` (0x100); otherwise `Off`.
  3. State text via the existing `solarStateText`, plus `Power supply` for state 11 (not in `solarStateText` today; added there since it is harmless for solar).
- Invalid/no data: `-- A`, status (`Add key via WiFi` / `Decode error` / `Searching` / `Signal lost`), device line, red error text — same pattern as solar.
- Settings button drawn in the Orion palette.

Palette (violet; distinct from battery blue/green, solar amber, and red reserved for errors):

| Constant | RGB | RGB565 |
|---|---|---|
| `COLOR_ORION_PANEL` | (40, 16, 56) | `0x2887` |
| `COLOR_ORION_TRACK` | (90, 50, 120) | `0x598F` |
| `COLOR_ORION_ARC` | (180, 110, 255) | `0xB37F` |
| `COLOR_ORION_TEXT` | (245, 235, 255) | `0xF75F` |
| `COLOR_ORION_DIM_TEXT` | (200, 180, 225) | `0xCDBC` |

Note: `drawAaText` blends anti-aliased glyphs against `COLOR_PANEL_BLUE` regardless of page; the solar page already lives with this. The Orion page will pass its panel colour as the blend background by adding an optional background parameter to `drawAaText`/`drawAaCentered` (default `COLOR_PANEL_BLUE`, so existing calls are unchanged). Solar calls are left as-is.

## Code structure

All changes stay in `src/main.cpp`, following existing style (file-static globals, `F()` strings, NVS via `secrets`). No refactor of the existing file.

## Verification

Local:
- `pio run` builds with no errors; report flash/RAM usage from the build output and confirm the app fits in 3 MB.

On device (user, via USB flash):
1. Settings and keys are still present after flashing the new partition table.
2. Demo mode: battery, solar, Orion pages all render; Orion page uses the violet palette.
3. Demo off, only shunt key saved: swiping shows battery page only; adding a solar key makes the solar page appear.
4. Cycle 10s: pages advance every 10 s; touching pauses for one interval; no cycling while on settings or while the screen is off.
5. Serial log shows `Victron shunt locked to …` (and solar/Orion) within a few seconds of first decode; setup page shows the locked address.
6. Changing a key on the setup page logs a lock release and relocks.
7. Setup page no longer shows the firmware upload form; `POST /firmware` returns the setup page (not-found handler).

## Out of scope

WiFi client mode and pushing data to a remote endpoint (separate design), other device types, warning/alarm display, rear button.
