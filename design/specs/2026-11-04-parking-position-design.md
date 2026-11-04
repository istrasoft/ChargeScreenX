# Parking, Position, Signal and Starter Battery — Design

Date: 2026-11-04. Builds on `2026-10-28-online-reports-design.md` (outbox, batches, netlink task, sinks).

## Goal

The device sits in a moving vehicle with no GPS. It should:

1. let the user change **how often data batches are sent** from the device's own settings screen, without the setup hotspot;
2. add **engine-running seconds, solar-producing seconds and starter battery voltage** to every 5-minute record, so a backend can total driving and solar time per day, month and year;
3. report **where the van is parked**, using a Wi-Fi scan and Google's Geolocation API, only after a real drive;
4. add the **4G signal at each parking place**, read from the Deco X1500-4G;
5. alert when the **starter battery** runs low while parked;
6. alert when the **van seems to have moved** without the engine running.

Statistics (daily, monthly and yearly totals, position history, maps) belong to a future remote backend. The device keeps no long-term totals; it sends raw records and events.

## Non-goals

- Position while driving.
- On-device monthly or yearly statistics.
- Cell tower data in the geolocation request. It is added later, only if `tools/deco-probe` shows that the Deco gives out MCC, MNC, TAC and the 28-bit cell ID. Google needs the full LTE cell identity (ECI); the 8-bit cell ID alone gives wrong or empty results.
- The Tapo camera watchdog and Bluetooth thermometers (later, separate designs; `tools/ble-survey` finds candidates).
- Certificate checking for Google (the same `setInsecure()` policy as the existing sinks).

## Part 1 — Send interval on the device

### Behaviour

- The existing `batch_min` setting gets these options: **15, 30, 60, 120, 240 min, and Off**. The default stays 30.
- **Off**: one connection about 60 s after boot (clock and time zone, so reports can fire), then no periodic connection. Records are still kept, and the unsent ones go out with the next alert, report, parking event or test. The 288-record ring (24 h) still overwrites the oldest records, as today.
- Records stay at 5-minute resolution; only the sending changes. 240 min is 48 records, which is 4 batch messages, within the existing limit of 6 per connection.
- The setup page's "Batch interval" select shows the same options ("Off" for 0).

### On-device settings

- Settings page 3 gets a fourth row and uses the row positions of the first settings page (y = 66, 100, 134, 168): Labels, Grid, Cycle, **Send**.
- Tapping Send moves to the next option: 15m → 30m → 1h → 2h → 4h → Off → 15m. The value text is `15m`, `30m`, `1h`, `2h`, `4h`, or `-` for Off. The row is highlighted when it is not Off.
- The value is saved to NVS key `batch_min` right away (0 = Off) and passed to netlink.

### Code

- `cs::sanitiseBatchMin` accepts 0, 15, 30, 60, 120 and 240; anything else becomes 30. Storage stays `uint8_t`.
- `netlink` reads `batchMin` from the shared config at every wake, so a change applies without an extra connection. When it is 0, there is no periodic timer after the first attempt; it wakes on "send now" and rechecks the setting every 60 s.
- The setup page form validation range becomes 0–240, checked through the sanitiser.

## Part 2 — New record fields

Each 5-minute batch record gets three fields:

| Field | Meaning |
|---|---|
| `eng` | Seconds the engine was running: 10 s per valid Orion sample whose state is not Off (0). Left out when the Orion had no valid sample in that record. |
| `pvs` | Seconds the panels produced: 10 s per valid solar sample with PV above 5 W (`PV_PRODUCING_WATTS`). Left out when the solar charger had no valid sample. |
| `sv` | Average Orion XS input voltage (V, 1 decimal) over valid samples between 5 and 20 V. With the engine off this is the starter battery; with it running, the alternator. Left out when there was no such sample. |

Example: `{"t":1761638400,"soc":78.1,...,"or":12.4,"eng":300,"pvs":120,"sv":14.1}`.

- The stored `cs::Record` grows from 44 to 52 bytes (`engS` takes the old `reserved` field; `sv`, `pvS` and a new `reserved` are added). The cache file check discards a `/tx_cache.bin` with the old record size, so **unsent records are lost once** when this firmware is flashed.
- Twelve worst-case records still fit ntfy's 4096-byte message limit (the existing test).

## Part 3 — Parking position

### Engine and parking detection

- **Engine running**: the Orion XS is valid and its device state is not Off (0). The Orion reports `Engine off` as its off reason, so this follows the ignition and alternator.
- Off periods shorter than **3 min** (`PARK_SETTLE_MS`) are part of the drive, so stop-start at traffic lights and short stops don't split it.
- A **drive** is an engine run of at least **10 min** (`ENGINE_RUN_MIN_MS`), counted from the first running sample to the last.
- A **parking** happens when the engine has stayed off for 3 min after a drive. This also gives the Deco time to settle.
- If the Orion goes invalid (no key, out of range, decode failure), no parking is detected. A drive in progress is dropped when the Orion has been invalid for more than 60 s.
- Demo mode, or online sending switched off, never produces parkings.
- A parking hands one park request to netlink and wakes it. The request carries the engine-off time and the run length. It lives in RAM; a reboot before it is handled loses it. A newer request replaces an older pending one.
- Any running engine sample also tells netlink the van is driving, which stops the moved-van watch (Part 6).

### Position fix (netlink task)

On a pending park request, during the normal connection attempt:

1. **Scan** before joining (`WiFi.scanNetworks`, STA mode, about 3 s). Drop:
   - access points whose SSID equals either configured WiFi SSID (the Deco mesh, any phone hotspot);
   - locally administered MACs (bit 0x02 of the first octet), which are phone hotspots and randomised MACs;
   - SSIDs ending in `_nomap`.
   Keep the 15 strongest that are left.
2. **Same place?** Compare the 10 strongest with the BSSID list saved at the last Google fix. If at least half of them (rounded up) are in that list, the van hasn't moved: `src:"same"` with the saved position, and no Google call.
3. **Too few networks** (fewer than 2) or **no Google key**: `src:"none"`, no call.
4. **Quota**: if a quota refusal was seen today, or 20 calls (`GEO_MAX_CALLS_PER_DAY`) were made today (local day; UTC day if the offset is unknown): `src:"quota"`, no call.
5. **Call Google** after joining WiFi:
   `POST https://www.googleapis.com/geolocation/v1/geolocate?key=<key>` with
   `{"considerIp":false,"wifiAccessPoints":[{"macAddress":"aa:bb:..","signalStrength":-67,"channel":6},...]}`.
   `considerIp:false` stops Google falling back to the mobile network's IP.
6. **Results**:
   - **200**: `src:"google"` with `lat`, `lon` and `acc` (metres, rounded). Save the position and BSSID list as the new reference.
   - **404, or reason `notFound`**: `src:"none"`. The reference is not changed.
   - **429, or reason `dailyLimitExceeded`, `userRateLimitExceeded` or `rateLimitExceeded`**: mark quota used for today, `src:"quota"`.
   - **Any other 4xx** (bad key, API not enabled): `src:"error"`.
   - **Network or TLS failure, or 5xx**: the request stays pending and the attempt counts as failed (existing backoff). After 3 failed attempts it is sent as `src:"error"`.
7. The Deco signal is read (Part 4), the park message goes into the outbox, and it is sent in the same connection.
8. The 10 strongest scanned BSSIDs become the moved-van watch reference (Part 6).

### Saved state

`/lastfix.bin` (LittleFS), written after a Google call and whenever the watch reference changes:

- `lat`, `lon`, `acc`, fix time, and up to 15 reference BSSIDs (strongest first);
- quota day, calls today, refused flag;
- moved-van watch reference (up to 10 BSSIDs) and whether the watch is active.

### Settings

| Setting | Where | NVS key | Default |
|---|---|---|---|
| Google geolocation key | Setup page, "Online" section, password field with a "Remove saved key" box | `geo_key` | empty (no Google calls) |

The key is never shown on the info screen or in logs. Accepted characters: `A–Z a–z 0–9 - _`, at most 64.

### Message format

Generic sink: `kind:"park"`, priority 2, `time` = engine-off time, `data`:

```json
{"v":1,"t":1762250000,"run":4380,"src":"google","lat":48.85661,"lon":2.35222,"acc":38,"aps":9,"sig":{"net":"lte_plus","rsrp":-95,"rsrq":-11,"snr":8.5,"lvl":75}}
```

| Field | Meaning |
|---|---|
| `t` | Engine-off time, unix seconds (0 if never known) |
| `run` | Length of the drive in seconds; 0 for a fix triggered by the moved-van watch |
| `src` | `google`, `same`, `none`, `quota` or `error` |
| `lat`, `lon` | 5 decimals. Present for `google` and `same` |
| `acc` | Accuracy radius in metres (integer). Present for `google` and `same` |
| `aps` | Usable access points after filtering |
| `sig` | 4G signal from the Deco (Part 4). Left out when it could not be read |

ntfy sink: title `Parked`, priority 2, tag `parking`.

- With a position: `48.85661, 2.35222 ±38 m · drove 1 h 13 min · 4G -95 dBm`, with `click` set to `https://maps.google.com/?q=48.85661,2.35222`.
- Without one: `Position unknown (none) · drove 1 h 13 min`.
- The signal part appears only when `sig` is present.

### Google setup (user, one-off)

1. In Google Cloud Console, create a project with billing enabled and turn on the Geolocation API.
2. Create an API key restricted to the Geolocation API.
3. Under Quotas, cap the Geolocation API at, for example, 300 requests a day, so it refuses calls rather than bill above the free allowance. The Geolocation SKU is in the Essentials tier (10,000 free calls a month under the March 2025 pricing).
4. Paste the key into the setup page.

## Part 4 — 4G signal at each parking (Deco)

**Gate:** implemented only after `tools/deco-probe` has shown, on the real Deco X1500-4G, that the owner-password login works and `admin/network?form=internet` returns a `mobile_cpe` object with `rsrp`. If it doesn't, Part 4 is skipped and the rest ships without it.

- Setting: **Deco password** on the setup page (password field, "Remove saved password" box), NVS key `deco_pass`. It is the Deco owner's TP-Link ID password. Empty means Part 4 is off.
- During park handling (after the position step, while joined), netlink logs in to the WiFi gateway address (`WiFi.gatewayIP()`, plain HTTP), sends one read of `admin/network?form=internet`, and logs out. At most one login per parking; a failure only means no `sig` field.
- Fields read from `result.mobile_cpe`: `network_type` → `net` (string, at most 15 characters), `rsrp`, `rsrq`, `snr`, `signal_strength` → `lvl` (0–100). Missing fields are left out of `sig`.
- ntfy text shows `4G <rsrp> dBm` (or `4G <lvl>%` without `rsrp`).

### Deco local protocol

From the open-source `tplinkrouterc6u` library (Deco client, `EncryptionWrapper`), confirmed by the probe before this part is built:

1. `POST http://<gw>/cgi-bin/luci/;stok=/login?form=keys` with query `operation=read` → JSON `result.password = [n, e]` (hex RSA public key for the password).
2. `POST …/login?form=auth?operation=read` → `result.seq` (integer) and `result.key = [n, e]` (hex RSA public key for signatures).
3. The device makes a random AES-128-CBC key and IV: 16 lowercase hex characters each, used as ASCII bytes.
4. Login body: `{"params":{"password":"<hex RSA-PKCS1v1.5(password) with the password key>"},"operation":"login"}`, AES-encrypted with PKCS7 padding and base64-encoded → `data`.
5. Signature: the text `k=<key>&i=<iv>&h=<md5hex("admin"+password)>&s=<seq + length(data)>`, cut into 53-byte pieces, each RSA-PKCS1v1.5-encrypted with the signature key, hex-joined → `sign`.
6. `POST …/login?form=login` with body `sign=<sign>&data=<url-encoded data>` (header `Content-Type: application/json`, as the library sends it). The reply is `{"data":"<base64>"}`; decrypted, it holds `result.stok` and `error_code: 0`. The `sysauth` cookie comes from `Set-Cookie`.
7. Reads: `POST /cgi-bin/luci/;stok=<stok>/admin/network?form=internet` with cookie `sysauth=<…>` and body `sign=<…>&data=<…>`, where `data` is the AES-encrypted `{"operation":"read"}` and the signature text is `h=<md5hex>&s=<seq + length(data)>`. The decrypted reply has `error_code: 0` and `result`.
8. Logout: the same, with path `admin/system?form=logout` and `{"operation":"logout"}`; the reply is ignored.

The ESP32 uses its built-in mbedtls (RSA, AES, MD5, base64) and `esp_fill_random`.

## Part 5 — Starter battery watch

- **Sample**: the Orion XS input voltage is added to `cs::Sample` (`orionInV`).
- **Watching**: only while the Orion is valid, its state is Off, and the input voltage is between 5 and 20 V. The first **60 min** after the engine stops (or after boot) are ignored, to let the surface charge settle.
- **Low**: every watched sample below **12.2 V** for **30 min** in a row triggers one alert. A sample at or above 12.2 V restarts the 30 min.
- **Alert**: kind `starter`, priority 4, tag `battery`, title `Starter battery low`. Body: `Starter battery at 12.1 V, engine off for 3 h 10 min. Start the engine or charge it soon.` The voltage is the lowest seen in the 30 min.
- **Re-arm**: after the engine runs again, or after 30 min in a row at or above **12.6 V**. So there is at most one alert per parking unless the battery recovers.
- Device check: confirm that the Orion's input voltage really shows the starter battery while it is Off. If it reads 0 V when Off, the watch never fires and the `sv` field shows only driving voltages.

## Part 6 — Moved-van watch

- **Reference**: after each park message, the 10 strongest scanned BSSIDs are saved as the watch reference and the watch becomes active, if there were at least 3.
- **Stop**: any engine-running sample stops the watch (telemetry tells netlink). It stays off until the next parking.
- **Scan**: while the watch is active, netlink scans (no join, about 3 s) every **60 min**, whatever the Send setting, unless the settings hotspot is up or online sending is off.
- **Suspicion**: a scan with at least 3 usable access points and **none** of its 10 strongest in the reference. A second scan 5 min later must agree.
- **Alert**: kind `moved`, priority 4, tag `rotating_light`, title `Van moved?`. Body: `The Wi-Fi networks around the van changed while the engine was off. It may have been moved.` Netlink then queues a park request (run 0) so the new position is looked up and sent as a `park` message, and that scan becomes the new reference.
- A scan with fewer than 3 usable networks, or with some overlap, clears the suspicion.
- Known limit: a campsite whose access points all switch off at night could raise a false alert. The no-overlap rule and the second scan make this rare.

## Backend notes (not in this firmware)

- Solar and weather forecast for the parked position (Open-Meteo, no key) and "panels producing less than forecast" checks.
- Daily, monthly and yearly driving time (`eng`), solar time (`pvs`), starter voltage history (`sv`), and a map of parkings with their 4G signal.

## Architecture

- **`include/chargescreen_stats.h`** (native-tested, no Arduino):
  - batch interval: `sanitiseBatchMin`, `nextBatchMin`, `batchMinText`;
  - records: `eng`, `pvs`, `sv` in `Record`, `RecordBuilder` and the batch JSON;
  - `EngineTracker` (drive and parking trigger), `StarterWatch` and its alert text;
  - Wi-Fi: `apUsable`, `apKeepStrongest`, `samePlace`, `apOverlapCount`, `GeoState` with the daily budget, `geoDecide`, Google request and response;
  - `ParkResult`, `DecoSignal`, the park data JSON and its ntfy text, the ntfy `click` field;
  - Deco: response field reader and key validation.
- **`src/deco.cpp/.h`**: the Deco login and signal read (mbedtls + `HTTPClient`). Only `netlink` calls it.
- **`src/telemetry.cpp`**: feeds `EngineTracker` and `StarterWatch` every 10 s, raises park requests and starter alerts, tells netlink when the engine runs. Adds `/lastfix.bin` load and save to the store.
- **`src/netlink.cpp`**: reads the batch interval every wake, scans before join, park handling, Google call, Deco signal, moved-van watch timer.
- **`src/main.cpp`**: the Send row, `orionInV` and `orionState` in the sample, the `geo_key` and `deco_pass` fields, the extended batch select.

## Testing

Native unit tests:

- `sanitiseBatchMin`, `nextBatchMin`, `batchMinText`.
- Records: `eng`, `pvs` and `sv` values, left out without valid samples; twelve worst-case records under the ntfy limit.
- `EngineTracker`: a 9 min run gives no parking; 10 min then 3 min off gives one with the right `run`; 2 min off mid-drive stays one drive; Orion invalid over 60 s drops the run.
- `StarterWatch`: no alert in the first 60 min; 30 min below 12.2 V alerts once with the lowest voltage; a high sample restarts the count; re-arm after the engine runs or after 30 min at 12.6 V.
- AP filter, `samePlace` (5 of 10 same; 4 of 10 moved; empty reference moved), `apOverlapCount`.
- `geoDecide` order and daily budget; Google request JSON; parsing of 200, 404, 403 `dailyLimitExceeded`, 400 `keyInvalid`, 503.
- Park data JSON and ntfy text, with and without position and signal; 5-decimal rounding; ntfy `click`.
- Deco: reading `mobile_cpe` fields from a sample reply; password and key validation.

On the device:

1. Settings page 3 shows four rows. Send cycles through the options and survives a reboot. The setup page shows the same value.
2. Send = Off: no batches arrive until a test message is sent, and then the backlog follows.
3. Drive at least 10 min and park: one `park` message about 3 min after the engine stops, with a position near the real one and, with the Deco password set, a `sig` part.
4. Restart the engine and stop again in the same spot after 10+ min: `src:"same"`, and no Google call in the Cloud Console metrics.
5. Wrong key: `src:"error"`. A daily quota of 1 in the Console and a second drive: `src:"quota"`.
6. A 5 min engine run gives no `park` message.
7. Batch records show `eng` near 300 while driving, `pvs` near 300 in full sun, and `sv` around 12.6 V parked and 14 V driving.
8. Starter watch: confirm `sv` while parked matches a multimeter on the starter battery.
9. Moved-van watch: after parking, switch the Deco off and on (its own networks are filtered, so this must not alert); a real move without the engine is hard to test, so check the serial log for the hourly scans and overlap counts.
