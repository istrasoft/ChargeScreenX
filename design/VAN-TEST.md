# Van test day: beta4.18

What to do in the vehicle, in order. Everything is on branch `firmware`. Spec: `design/specs/2026-11-04-parking-position-design.md`. Plan and status: `design/plans/2026-11-04-parking-position.md`.

## 0. Before leaving (at home, optional)

- [ ] Google Cloud: create a project, enable billing and the **Geolocation API**, create an API key restricted to it, and set a daily quota (Quotas → Geolocation API → requests per day → 300).
- [ ] Note the Deco owner password (the TP-Link ID password the Deco app uses).

## 1. Laptop scans (engine off, everything in the van switched on)

Both tools only read; nothing is changed or paired.

- [ ] **BLE survey**: `tools/ble-survey/README.md`. About 3 minutes. Share `ble_survey.json` (personal devices are already left out). We look for thermometers, gas-level sensors, BMS modules.
- [ ] **Deco probe**: laptop on the Deco Wi-Fi, Deco app closed, then `tools/deco-probe/README.md`. Share `cell_summary.json` only (not `deco_dump.json`).
  - If it shows `mobile_cpe` fields such as `rsrp`, the firmware's 4G signal reading should work: set the Deco password in step 3.
  - If the login fails or there is no `mobile_cpe`, leave the Deco password empty. Everything else works without it.
  - If it lists cell ID / TAC / MCC / MNC fields, tell me: that enables a cell-tower follow-up.

## 2. Flash

USB cable to the ChargeScreen (COM11 last time).

```bash
pio run -e esp32-c3-round-dashboard -t upload
```

```bash
git checkout -- include/build_version.h
```

```bash
pio device monitor -b 115200
```

The info screen should show **beta4.18**. The first boot discards the old record cache once (the record layout grew), so unsent records from before the flash are lost.

## 3. Settings (setup hotspot: Settings → WiFi row → join `ChargeScreen` → http://192.168.4.1/)

- [ ] **Google geolocation key**: paste it.
- [ ] **Deco password**: only if the probe worked.
- [ ] Save, close the hotspot.

## 4. Checks

Tick each one and note anything odd (serial log lines start with `Online:` or `Deco:`).

- [ ] Settings page 3 shows four rows: Labels, Grid, Cycle, **Send**. Tapping Send goes 15m → 30m → 1h → 2h → 4h → `-` → 15m; the value survives a reboot and matches the setup page's Batch interval.
- [ ] Send = `-` (Off): no batches arrive until **Send test message**, then the backlog follows. Set it back afterwards.
- [ ] Batch records (ntfy "ChargeScreen data") contain `eng` (about 300 per record while driving), `pvs` (about 300 in full sun) and `sv`.
- [ ] **`sv` parked = starter battery?** Compare the parked `sv` (or the Orion page input voltage) with a multimeter on the starter battery. If the Orion shows 0 V when off, the starter alert can't work: tell me.
- [ ] **Drive 10+ minutes and park**: about 3 minutes after the engine stops, one **Parked** message with a position near the real one (tap it: Google Maps opens), `drove N min`, and `4G -NN dBm` if the Deco password is set. Serial: `Online: scan found N networks, M usable`, `geolocate -> HTTP 200`, `Online: park google`.
- [ ] **Same spot again**: run the engine 10+ minutes without moving, stop: Parked with `src` `same`, and no new call in the Google Cloud metrics.
- [ ] **Short run**: engine on for 5 minutes, off: no Parked message.
- [ ] **Quota** (optional): set the daily quota to 1, two drives: the second Parked says `Position unknown (quota)`. Restore the quota.
- [ ] **Moved-van watch**: after parking, serial shows `Online: watch scan, N usable, K known` every hour. Switching the Deco off and on must not alert (its networks are ignored).
- [ ] **Starter battery low** can't be forced safely; just check that no false alert arrives overnight.

## 5. Report back

- `ble_survey.json`, `cell_summary.json`, a serial log of one drive and park, and the ticked list.
