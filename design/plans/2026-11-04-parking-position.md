# Parking, Position, Signal and Starter Battery — Implementation Plan

> Executed inline on 2026-11-04 (user away; firmware technical decisions delegated). This file records the tasks, what each one changed, and how it was verified. Spec: `design/specs/2026-11-04-parking-position-design.md`. Van checklist: `design/VAN-TEST.md`.

**Goal:** Parking position (Wi-Fi + Google), 4G signal from the Deco, starter battery alert, moved-van alert, engine/solar/starter fields in records, and an on-device Send interval.

**Architecture:** Pure logic in `include/chargescreen_stats.h` (namespace `cs`, native-tested). `src/telemetry.cpp` feeds the engine tracker and starter watch from `loop()`. `src/netlink.cpp` scans, calls Google and the Deco, runs the moved-van watch, and posts. `src/deco.cpp` holds the TP-Link Deco login (mbedtls). `src/main.cpp` adds the Send row, the setup fields and the sample fields.

**Tech stack:** PlatformIO, espressif32 6.13.0 (Arduino-ESP32 2.x, mbedtls 2.28), NimBLE 2.5.2, Unity native tests.

## Global constraints

- `platform = espressif32@6.13.0` and `NimBLE-Arduino@2.5.2` stay pinned. No new libraries.
- Build: `pio run -e esp32-c3-round-dashboard`, then `git checkout -- include/build_version.h`.
- Native tests (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`.
- `cs::` code never reads clocks or uses Arduino types.
- Commits as `istrasoft` (no-reply email), ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- New NVS keys: `geo_key` (≤ 64, `A–Z a–z 0–9 - _`), `deco_pass` (≤ 64 printable ASCII). `batch_min` now also accepts 0, 120, 240.
- New file: `/lastfix.bin` (`cs::GeoState`). `/tx_cache.bin` record size 44 → 52 bytes (old cache discarded once).

## Tasks

### Task 1: Send interval logic — done
- `cs::sanitiseBatchMin` (0, 15, 30, 60, 120, 240), `nextBatchMin`, `batchMinText`.
- Tests: `test/test_stats` (sanitiser values, cycle order and texts, invalid → 30 → next 60).

### Task 2: Record fields — done
- `Sample.orionState`, `Sample.orionInV`; `orionEngineRunning`, `starterVoltageUsable`.
- `Record.engS` (old `reserved`), `Record.sv`, `Record.pvS`; static_assert 52 bytes. `RecordBuilder` counters; batch JSON `eng`, `pvs`, `sv`, left out when unknown.
- Tests: `test/test_payloads` (`test_record_engine_solar_seconds_and_starter`; the worst-case 12-record ntfy-limit test now includes the new fields).

### Task 3: Engine tracker and starter watch — done
- `EngineTracker` / `engineUpdate` (10 min drive, 3 min settle, 60 s invalid drop), `ParkRequest`, `putDuration`.
- `StarterWatch` / `starterUpdate` (60 min settle, 30 min below 12.2 V, re-arm on engine or 30 min ≥ 12.6 V), `buildStarterBody`, `MOVED_BODY`.
- Tests: `test/test_park` (engine: short run, drive+park, short stop, invalid drop; starter: settle, alert text, restart, re-arm, unusable voltage).

### Task 4: Wi-Fi and Google — done
- `ApInfo`, `apUsable`, `apKeepStrongest`, `apOverlapCount`, `GeoState`, `geoStateValid`, `samePlace`, `geoBudgetAllows`, `geoDecide`, `geoRemember`, `watchSetReference`, `watchSuspicious`, `validGeoKey`, `buildGeoUrl`, `buildGeoRequest`, `parseGeoResponse` (+ `jsonReadNumber`, `jsonStringIs`, `jsonObjectEnd`, `jsonValueIn`).
- Tests: `test/test_park` (filter and sort, same place 5/10 vs 4/10, watch suspicion, decide order and daily budget, request JSON, key/URL, responses 200/404/403/429/400/503/-1).

### Task 5: Park message and Deco helpers — done
- `MsgKind::Park`, `Starter`, `Moved` with names, titles, tags (`parking`, `battery`, `rotating_light`), priorities (2, 4, 4).
- `ParkResult`, `DecoSignal`, `buildParkData`, `buildParkText` (Maps `click`), ntfy envelope `click` parameter.
- `validDecoPassword`, `parseDecoSignal`, `buildDecoSignText`, `urlEncode`.
- Tests: `test/test_park` (data JSON and text with and without position and signal, ntfy click, kinds, Deco reply parsing, sign text, URL encoding).

### Task 6: Device wiring — done
- `src/telemetry.cpp/.h`: `storeLoadGeo` / `storeSaveGeo`; `checkVehicle()` per sample (park request, `netlinkEngineRunning()`, starter alert); trackers reset when telemetry is inactive.
- `src/netlink.cpp/.h`: `NetConfig.geoKey`, `decoPass`; `netlinkRequestPark`, `netlinkEngineRunning`; batch interval read every wake (Off = first attempt after boot, then only "send now"); scan before join when a park is pending; `handlePark` (decide, Google, Deco, watch reference, park message, retry up to 3 attempts); hourly `runWatchScan` with a 5 min confirmation; Park messages skip the delayed suffix and build their text from the data.
- `src/deco.cpp/.h`: Deco login and `admin/network?form=internet` read with mbedtls RSA PKCS#1 v1.5, AES-128-CBC, MD5, base64; buffers allocated only during the read.
- `src/main.cpp`: Send row on settings page 3 (rows moved to y = 66/100/134/168), setup fields for the Google key and Deco password, batch select with 120/240/Off, `orionState` and `orionInV` in the sample, Online hint text.

### Task 7: Docs, tools, version — done
- `README.md` Online section, `design/VAN-TEST.md`, `tools/deco-probe`, `tools/ble-survey`.
- `build_counter.txt` → 18 (beta4.18).

## Verification (2026-11-04)

- Native tests: 108/108 pass (9 suites, new `test_park`).
- Firmware build: success, RAM 19.3 %, Flash 45.8 %.
- Not verified (needs the van): everything in `design/VAN-TEST.md`. The Deco code follows the `tplinkrouterc6u` protocol but has never talked to a real Deco; it stays off until a Deco password is set.

## Follow-ups

- Cell tower data for Google, if the probe shows MCC / MNC / TAC / cell ID.
- Tapo C211 watchdog (camera off while parked).
- Bluetooth thermometers / gas level, depending on the BLE survey.
- Backend: weather and solar forecast, daily/monthly/yearly totals, parking map with 4G signal.
