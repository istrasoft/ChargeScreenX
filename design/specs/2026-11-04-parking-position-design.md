# Parking Position and On-Device Send Interval — Design

Date: 2026-11-04. Builds on `2026-10-28-online-reports-design.md` (outbox, batches, netlink task, sinks).

## Goal

The device sits in a moving vehicle with no GPS. It should:

1. report **where the van is parked**, using a Wi-Fi scan and Google's Geolocation API, and only after a real drive;
2. let the user change **how often data batches are sent** from the device's own settings screen, without the setup hotspot.

Statistics (daily, monthly and yearly driving and solar time, position history) belong to a future remote backend. The device keeps no long-term totals; it sends raw events.

## Non-goals

- Position while driving.
- On-device monthly or yearly statistics.
- Cell tower data in the geolocation request. It is added later, only if `tools/deco-probe` shows that the Deco X1500-4G gives out MCC, MNC, TAC and the 28-bit cell ID. Google needs the full LTE cell identity (ECI); the 8-bit cell ID alone gives wrong or empty results.
- Per-record engine and solar seconds (`eng`, `pvs`) in batches. Deferred until the backend exists.
- Certificate checking for Google (the same `setInsecure()` policy as the existing sinks).

## Part 1 — Send interval on the device

### Behaviour

- The existing `batch_min` setting gets these options: **15, 30, 60, 120, 240 min, and Off**. The default stays 30.
- **Off**: no periodic connection. Records are still kept, and the unsent ones go out with the next alert, report, parking event or test. The 288-record ring (24 h) still overwrites the oldest records, as today.
- Records stay at 5-minute resolution; only the sending changes. 240 min is 48 records, which is 4 batch messages, within the existing limit of 6 per connection.
- The setup page's "Batch interval" select shows the same options ("Off" for 0).

### On-device settings

- Settings page 3 gets a fourth row and uses the row positions of the first settings page (y = 66, 100, 134, 168): Labels, Grid, Cycle, **Send**.
- Tapping Send moves to the next option: 15m → 30m → 1h → 2h → 4h → Off → 15m. The value text is `15m`, `30m`, `1h`, `2h`, `4h`, or `-` for Off. The row is highlighted when it is not Off.
- The value is saved to NVS key `batch_min` right away (0 = Off). Netlink reads it at its next wake, so the new interval applies from the next batch.

### Code

- `cs::sanitiseBatchMin` accepts 0, 15, 30, 60, 120 and 240; anything else becomes 30. Storage stays `uint8_t`, since 240 fits.
- `netlink`: when `batchMin == 0`, there is no periodic timer; it only wakes on "send now".
- The setup page form validation range becomes 0–240, checked through the sanitiser.

## Part 2 — Parking position

### Engine and parking detection

- **Engine running**: the Orion XS is valid and its device state is not Off (0). The Orion is powered from the starter battery and reports `Engine off` as its off reason, so this follows the ignition and alternator.
- Off periods shorter than **60 s** are bridged, so stop-start at traffic lights doesn't split a drive.
- A **drive** is an engine run of at least **10 min** (`ENGINE_RUN_MIN_MINUTES`), counted from the first running sample to the last.
- A **parking** happens when the engine has stayed off for **3 min** (`PARK_SETTLE_MINUTES`) after a drive. This gives the Deco time to settle and rules out stop-start.
- If the Orion goes invalid (no key, out of range, decode failure), no parking is detected. A drive in progress is dropped when the Orion has been invalid for more than 60 s.
- Demo mode never produces parkings.
- A parking queues one `park` request for netlink (`telemetry` → netlink "send now"). It carries the engine-off time and the run length.

### Position fix (netlink task)

On a pending `park` request, during the normal connection attempt:

1. **Scan** before joining (`WiFi.scanNetworks`, STA mode, about 3 s). Drop:
   - access points whose SSID equals either configured WiFi SSID (the Deco mesh, any phone hotspot);
   - locally administered MACs (bit 0x02 of the first octet), which are phone hotspots and randomised MACs;
   - SSIDs ending in `_nomap`.
   Keep the 15 strongest that are left.
2. **Same place?** Compare the 10 strongest with the BSSID list saved at the last Google fix. If at least half of the current ones (rounded up) are in that list, the van hasn't moved: send `src:"same"` with the saved position, and don't call Google.
3. **Too few networks**: fewer than 2 left means `src:"none"`, with no call.
4. **No key**: an empty Google key means `src:"none"`, with no call.
5. **Quota**: if a quota refusal was seen today, or 20 calls (`GEO_MAX_CALLS_PER_DAY`) have been made today (local day; UTC day if the offset is unknown), send `src:"quota"`, with no call.
6. **Call Google** after joining WiFi:
   `POST https://www.googleapis.com/geolocation/v1/geolocate?key=<key>` with
   `{"considerIp":false,"wifiAccessPoints":[{"macAddress":"aa:bb:..","signalStrength":-67,"channel":6},...]}`.
   `considerIp:false` stops Google falling back to the mobile network's IP.
7. **Results**:
   - **200**: `src:"google"` with `lat`, `lon` and `acc` (metres, rounded). Save this position and the BSSID list as the new reference.
   - **404 `notFound`**: `src:"none"`. The reference is not changed.
   - **403 or 429 with reason `dailyLimitExceeded`, `userRateLimitExceeded` or `rateLimitExceeded`**: mark quota used for today, and send `src:"quota"`.
   - **400 `keyInvalid`, or 403 for any other reason**: `src:"error"`.
   - **Network or TLS failure, or 5xx**: the park request stays pending and is retried at the next connection attempt (existing backoff). After 3 failed attempts it is sent as `src:"error"`.
8. The `park` message goes into the outbox like any other message and is sent in the same connection.

A new parking that arrives while an older `park` request is still pending replaces it. Only the newest parking needs a position.

### Saved state

`/lastfix.bin` (LittleFS), written only after a successful Google fix:

- `lat`, `lon`, `acc`, fix time;
- up to 15 BSSIDs (6 bytes each), strongest first;
- quota day and calls-today counter (also written when they change).

### Settings

| Setting | Where | NVS key | Default |
|---|---|---|---|
| Google geolocation key | Setup page, "Online" section, password-style field with show/clear like the access token | `geo_key` | empty (no Google calls) |

The key is never shown in full on the info screen or in logs.

### Message format

Generic sink: `kind:"park"`, priority 2, no tag, `time` = engine-off time, `data`:

```json
{"v":1,"t":1762250000,"run":4380,"src":"google","lat":48.85661,"lon":2.35222,"acc":38,"aps":9}
```

| Field | Meaning |
|---|---|
| `t` | Engine-off time, unix seconds |
| `run` | Length of the drive in seconds (first running to last running sample) |
| `src` | `google`, `same`, `none`, `quota` or `error` |
| `lat`, `lon` | 5 decimals. Present for `google` and `same` |
| `acc` | Accuracy radius in metres (integer). Present for `google` and `same` |
| `aps` | Usable access points after filtering |

ntfy sink: title `Parked`, priority 2, tags `["parking"]`.

- With a position, the message is `48.85661, 2.35222 ±38 m · drove 1 h 13 min`, with `click` set to `https://maps.google.com/?q=48.85661,2.35222`.
- Without one, it is `Position unknown (<src>) · drove 1 h 13 min`.

### Google setup (user, one-off)

1. In Google Cloud Console, create a project with billing enabled and turn on the Geolocation API.
2. Create an API key restricted to the Geolocation API.
3. Under Quotas, cap the Geolocation API at, for example, 300 requests a day, so it refuses calls rather than bill above the free allowance. The Geolocation SKU is in the Essentials tier (10,000 free calls a month under the March 2025 pricing).
4. Paste the key into the setup page.

## Architecture

- **`include/chargescreen_stats.h`** (native-tested, no Arduino):
  - `EngineTracker`: the running and bridging state machine, drive length, and the parking trigger;
  - `apFilter` (SSID, locally administered, `_nomap`, top-N);
  - `samePlace(current, saved)`;
  - Google request JSON builder and response parser (`lat`, `lon`, `accuracy`, error reason);
  - `park` envelope builders (generic and ntfy text);
  - extended `sanitiseBatchMin` and the send-interval value text.
- **`src/telemetry.cpp`**: feeds `EngineTracker` every 10 s from the Orion sample and raises the park request.
- **`src/netlink.cpp`**: scan before join, the decision steps above, the Google call (10 s timeout, existing heap guard), the park message, and `/lastfix.bin`. The scan adds about 3 s to that connection only.
- **`src/main.cpp`**: the Send row on settings page 3, the `geo_key` field, and the extended batch select.

## Testing

Native unit tests:

- `EngineTracker`:
  - a 9 min run gives no parking;
  - a 10 min run and 3 min off gives one parking with the right `run`;
  - 45 s off mid-drive is bridged;
  - 90 s off ends the run;
  - Orion invalid for over 60 s drops the run.
- AP filter: own SSIDs, locally administered MACs and `_nomap` are dropped; the strongest 15 are kept.
- `samePlace`: 5 of 10 matching is the same place; 4 of 10 is moved; an empty reference counts as moved.
- Google: request JSON shape; parsing of 200, 404, 403 `dailyLimitExceeded` and 400 `keyInvalid` bodies.
- Envelopes: generic and ntfy text, with and without a position; 5-decimal rounding.
- `sanitiseBatchMin`: 0, 15, 30, 60, 120, 240 are kept; 45 and 255 become 30.

On the device:

1. Settings page 3 shows four rows. Send cycles through the options and survives a reboot. The setup page shows the same value.
2. Send = Off: no batches arrive until a test message is sent, and then the backlog follows.
3. Drive at least 10 min and park: one `park` message about 3 min after the engine stops, with a position near the real one.
4. Restart the engine and stop again in the same spot after 10+ min: `src:"same"`, and no Google call in the Cloud Console metrics.
5. Wrong key: `src:"error"`. A daily quota of 1 in the Console and a second drive: `src:"quota"`.
6. A 5 min engine run gives no `park` message.
