# Van test day: beta4.19

What to do in the vehicle, in order. Everything is on branch `firmware`. Spec: `design/specs/2026-11-04-parking-position-design.md`. Plan and status: `design/plans/2026-11-04-parking-position.md`.

## 0. Before leaving (at home, optional)

- [ ] Google Cloud: create a project, enable billing and the **Geolocation API**, create an API key restricted to it, and set a daily quota (Quotas → Geolocation API → requests per day → 300).
- [ ] Note the Deco owner password (the TP-Link ID password the Deco app uses).

## 1. Laptop scans (engine off, everything in the van switched on)

Both tools only read; nothing is changed or paired.

- [ ] **BLE survey**: `tools/ble-survey/README.md`. About 3 minutes. Share `ble_survey.json` (personal devices are already left out). We look for thermometers, gas-level sensors, BMS modules.
- [x] **Deco probe** (done 2026-11-13): the login works and gives the signal values; there is no cell ID, TAC, MCC or MNC.
- [ ] **Wi-Fi positioning from the laptop**: `tools/geo-test/README.md` (turn on Windows Location services first). Checks the Google key and the accuracy at the van's spot.

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

The info screen should show **beta4.19**. The first boot discards the old record cache once (the record layout grew), so unsent records from before the flash are lost.

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
- [ ] **Drive 10+ minutes and park**: about 3 minutes after the engine stops, one **Parked** message with a position near the real one (tap it: Google Maps opens), `drove N min`, and a network part such as `4G+ cov 60 qual 25 · 48 ms · 2.4 Mbit/s`. Serial: `Online: scan found N networks, M usable`, `geolocate -> HTTP 200`, `Online: network 4G+ cov .. qual .., .. ms, .. kbit/s`, `Online: park google`.
- [ ] **Network readings while parked**: batch messages end with `"net":{...}` holding `ms` at every connection, and `kbps` about every 4 h. Serial shows `Online: network ...` at each connection.
- [ ] **Same spot again**: run the engine 10+ minutes without moving, stop: Parked with `src` `same`, and no new call in the Google Cloud metrics.
- [ ] **Short run**: engine on for 5 minutes, off: no Parked message.
- [ ] **Quota** (optional): set the daily quota to 1, two drives: the second Parked says `Position unknown (quota)`. Restore the quota.
- [ ] **Moved-van watch**: after parking, serial shows `Online: watch scan, N usable, K known` every hour. Switching the Deco off and on must not alert (its networks are ignored).
- [ ] **Starter battery low** can't be forced safely; just check that no false alert arrives overnight.

## 5. Report back

- `ble_survey.json`, the `geo_test.py` result, a serial log of one drive and park, and the ticked list.
