# Online Alerts, Reports and Data Batches — Design

Date: 2026-10-28
Branch: `firmware` (ChargeScreenX)

## Goal

Let ChargeScreen join WiFi when it has something to send, and push to an HTTP endpoint:

- a **high-drain alert** when the battery discharges faster than a limit over a rolling window (e.g. a fridge left open),
- a **morning battery report** with an estimate of the days remaining,
- an **evening solar report** after the day's production ends,
- a silent **data batch** every 30 minutes.

The first target is `https://ntfy.sh/ghibli_harmony`, used for both testing and production. The same code must later be able to post to a standalone PHP app on the user's own host by changing only the URL.

## Non-goals (v1)

- No server, dashboard or long-term history. The device keeps at most 24 h of cached records plus 7 daily summaries.
- No voltage in messages. Messages use % and Ah only.
- No TLS certificate verification. Nothing private or secret is sent; revisit this before posting to an authenticated endpoint.
- No more than 2 WiFi networks. No battery-capacity setting (capacity is learned). No sending in demo mode.

## Behaviour

### Messages

All messages have a title, a body and a priority. Priorities are ntfy levels: 1 = min (silent), 3 = default, 4 = high.

**High-drain alert** (priority 4, tag `warning`), sent as soon as it triggers:

```
High battery drain
Averaging 6.8 A over the last 45 min (limit 4.0 A). Battery 71% (178 Ah left).
```

**All-clear** (priority 3, tag `white_check_mark`):

```
Drain back to normal
1.4 A average. Battery 69%.
```

**Morning report** (priority 3, tag `sunny`), sent at the report hour:

```
Morning battery report
78% (196 Ah left)
At the last 3 days' rate: about 4.2 days with solar, about 2.1 days with no sun.
Yesterday: used 52 Ah, solar +31 Ah, Orion +40 Ah.
```

**Evening solar report** (priority 3, tag `city_sunset`):

```
Solar today
+31 Ah (412 Wh), peak 286 W at 13:12
Producing from 07:48 to 19:26 (11 h 38 m)
```

**Data batch** (priority 1, no tag): a compact JSON body (see Data formats).

Omission rules:

- Leave out the `solar` and `Orion` parts of "Yesterday:" when their value is 0.0 Ah or that device has no key.
- With no solar key, or when `avgSolar` (see Days remaining) is 0, the estimate line becomes a single figure: `At the last 3 days' rate: about 2.1 days.`
- Leave out "(N Ah left)" while the capacity hasn't been learned yet.
- Leave out the estimate line when no complete day of stats exists yet.
- When the net drain with solar is ≤ 0 (solar covers usage), the "with solar" part reads `solar covers your usage` instead of a number of days.
- Don't send the evening report on a day when the PV never exceeded 5 W.
- Use the real number of averaging days in the text: `last 2 days` if only 2 complete days exist, `last day` for 1.
- Format days with one decimal, and as `more than 30 days` above 30.

### Triggers

- **High drain:**
  - Every 60 s, record the average of the 10 s net shunt current samples, so a discharge is a positive value. Keep the last `dis_win_min` minutes (default 45, range 15–120).
  - Trigger when the full window exists and its average discharge > `dis_amps` (default 4.0 A, range 0.5–50).
  - Clear when the average stays < 0.8 × `dis_amps` for 15 consecutive minutes; that sends the all-clear.
  - After a trigger, don't trigger again for 60 minutes even if the condition cleared and returned.
- **Morning report:** once per local day, at the first sample at or after `report_hour`:00 local time (default 10, range 4–12). Requires valid local time.
- **Evening report:** once per local day, requiring valid local time and that PV exceeded 5 W earlier that day. It is sent at the first sample at or after `evening_hour`:00 local time (default 20, range 16–22), when PV has been < 5 W for the last 30 consecutive minutes. If production is still going on at that hour (e.g. a summer evening), it waits until PV has been < 5 W for 30 minutes, and goes out at 23:00 at the latest.
- **Batch:** every `batch_min` minutes (default 30; options 15, 30, 60). It contains the 5-minute records not yet sent.

### Offline behaviour

- Records, alerts and reports are cached on the device and sent, oldest first, when a network is available.
- Cache limits:
  - up to 288 five-minute records (24 h); the oldest are overwritten;
  - up to 16 pending messages (alerts and reports); if full, the oldest is dropped.
- A message sent more than 10 minutes after its event gets ` (delayed, happened at HH:MM)` appended to its first body line.
- A pending morning report is replaced when the next day's morning report is queued.
- A pending batch backlog is sent as several batch messages, each holding at most 12 records (one hour), with at most 6 batch messages per connection.

### Time

- UTC comes from NTP (`pool.ntp.org`, then `time.google.com`) on every connection.
- The local offset comes from an IP-geolocation time service, refreshed at most once per day. The response must give the current UTC offset, including daylight saving.
  - Primary: get the public IP from `https://api.ipify.org`, then `https://timeapi.io/api/TimeZone/ip?ipAddress=<ip>` and use `currentUtcOffset.seconds`.
  - Fallback: `http://ip-api.com/json/?fields=status,message,offset` (HTTP only on the free tier), field `offset`.
  - Amended 2026-10-29 after real requests during planning: worldtimeapi.org reset every connection, and timeapi.io returns HTTP 400 without `ipAddress`.
- Optional override `tz_override`: a POSIX TZ string such as `CET-1CEST,M3.5.0,M10.5.0/3`. When set, it replaces the online lookup; use it when roaming makes the IP location wrong.
- Before the first successful time sync after boot:
  - records are kept with a relative timestamp, and the absolute time is filled in at the first sync;
  - no morning or evening reports fire;
  - drain alerts still fire;
  - a delayed message whose event time is unknown in clock terms uses ` (delayed, happened N min ago)` instead of the HH:MM form.
- The time is kept across a brief WiFi loss by the system clock. After a reboot it is unknown until the next sync.

### Battery capacity

Learned from the shunt's SOC and consumed Ah, both already decoded:

- **Formula:** `capacity = consumedAh / (1 − SOC/100)`, using the consumed Ah as a positive number.
- **When:** only if SOC ≤ 90 %, SOC isn't the 102.3 "not available" value, and consumed Ah is valid.
- **Smoothing:** exponentially smoothed (α = 0.1), stored in NVS key `cap_ah` when it differs from the stored value by > 2 %, and loaded at boot.
- **Remaining:** `remaining Ah = SOC/100 × capacity`.

### Days remaining

The device keeps the last 7 local days in a `DayStats` record per day:

- **Integrated values** (10 s samples, rectangle rule):
  - `netAh`: the shunt current integrated;
  - `solarAh`: the solar charger battery current integrated, 0 if no solar key;
  - `orionAh`: the Orion output current integrated, 0 if no Orion key.
- **Derived:** `usedAh = solarAh + orionAh − netAh`, clamped ≥ 0.
- **Solar extras:** `solarWh`, `pvPeakW` with its local minute, and the first and last local minutes with PV > 5 W.
- **Missing data:** a sample with no valid data for a device adds 0 for that device. The sample still counts if the shunt was valid, and the whole sample is skipped if the shunt wasn't.

Over the last `days_avg` complete days (default 3, range 1–7; fewer if fewer exist):

- `avgUsed = mean(usedAh)`, `avgSolar = mean(solarAh)`;
- days with solar = `remainingAh / (avgUsed − avgSolar)` (shown as `solar covers your usage` when the denominator is ≤ 0);
- days with no sun = `remainingAh / avgUsed`.

When capacity isn't learned yet, the line is left out.

## Settings

The setup page gets a new **Online** section. All values live in the NVS namespace `victron`, and all key names are at most 15 characters.

| Field | NVS key | Default |
|---|---|---|
| WiFi 1 name / password | `wifi1_ssid`, `wifi1_pass` | empty |
| WiFi 2 name / password | `wifi2_ssid`, `wifi2_pass` | empty |
| Send to URL | `post_url` | empty (feature off) |
| Time zone override | `tz_override` | empty (automatic) |
| Drain limit (A) | `dis_amps` | 4.0 |
| Drain window (min) | `dis_win_min` | 45 |
| Morning report hour | `report_hour` | 10 |
| Evening report hour | `evening_hour` | 20 |
| Batch interval (min) | `batch_min` | 30 |
| Days to average | `days_avg` | 3 |

The feature is off while `post_url` is empty or no WiFi name is set. Passwords use the existing "leave blank to keep" pattern of the key fields. A **Send test message** button queues a priority-3 message `ChargeScreen test` / `Online sending works.`

The info screen gets one line, `Online`, showing one of: `Off`, `No WiFi`, `Sent HH:MM`, or `Error HH:MM`.

## Data formats

### Sink type

- If the URL host is `ntfy.sh`, or ends in `.ntfy.sh`, the sink is **ntfy**. Each message is POSTed as ntfy JSON to the URL's scheme + host (path `/`): `{"topic":"<path without leading />","title":"...","message":"...","priority":N,"tags":["..."]}`.
- Otherwise the sink is **generic**. Each message is POSTed to the URL as `{"v":1,"kind":"alert|clear|morning|evening|batch|test","title":"...","message":"...","priority":N,"time":<unix>,"data":{...}}`.

### Batch body

- For ntfy, this is the `message` string.
- For generic, it is the `data` object.

```json
{"v":1,"recs":[{"t":1761638400,"soc":78.1,"used":44.2,"i":-2.1,"imin":-6.0,"imax":3.2,"pv":120,"pvmax":286,"or":0.0}]}
```

Per 5-minute record:

| Field | Meaning |
|---|---|
| `t` | Start, unix seconds |
| `soc` | Last SOC % |
| `used` | Last consumed Ah (positive) |
| `i`, `imin`, `imax` | Net shunt current average, min, max (A) |
| `pv`, `pvmax` | PV power average and max (W) |
| `or` | Orion output current average (A) |

Rules:

- Fields with no valid data are left out of that record.
- Numbers use at most 1 decimal; W values are integers.
- Twelve records stay under ntfy's 4096-byte message limit.

### Network results

- HTTP 2xx means sent.
- HTTP 429, or any 5xx, means back off.
- Any other 4xx drops that message, so a bad message doesn't block the queue.
- Network or TLS errors mean retry later.

## Architecture

### `include/chargescreen_stats.h`

Header-only, Arduino-free, namespace `cs`, unit-tested natively like `chargescreen_logic.h`. It contains:

- `DrainMonitor` (minute averages, trigger and clear with cooldown);
- `DayStats` plus an integrator;
- `CapacityLearner`;
- `daysRemaining`;
- `SunsetDetector` (evening report timing);
- the report text builders (morning, evening, alert, clear, delayed suffix);
- the batch JSON builder;
- the ntfy and generic envelope builders;
- the sink-type check.

Text is built into caller-provided `char` buffers; no Arduino `String`.

### `src/telemetry.cpp` / `.h`

Called from `loop()`:

- Every 10 s, it snapshots the displayed stats (shunt, solar, Orion), feeds the `cs::` objects, and checks the triggers.
- Every 5 min, it closes a record into the cache ring.
- It enqueues messages for sending.

It owns these files:

| File | Contents |
|---|---|
| `/tx_cache.bin` | Ring of 288 records plus head/tail and the sent index |
| `/outbox.bin` | Up to 16 pending messages |
| `/days.bin` | The last 7 `DayStats` plus the current day |

These are written with the existing LittleFS mount, at most once per 5 minutes plus once per queued message.

### `src/netlink.cpp` / `.h`

A FreeRTOS task (priority 1, 8 KB stack) that owns WiFi client mode:

- It wakes on a "send now" notification (new alert or report, or test) or a batch timer.
- It tries WiFi 1 and then WiFi 2, 15 s each.
- It syncs NTP and refreshes the offset if it is older than 24 h.
- It sends the outbox and then the cached batches, then disconnects and turns WiFi off.
- It uses `WiFiClientSecure` with `setInsecure()` and `HTTPClient`, with a 10 s timeout.
- It skips the attempt if free heap is < 60 KB.

Shared state with `loop()`:

- the outbox and cache, behind one FreeRTOS mutex;
- a status struct (state and last time) read by the info screen.

Backoff after a failed attempt: 1, 2, 5, 10, then 30 minutes. A success resets it. A 429 jumps straight to 30 minutes.

### Changes in `src/main.cpp`

- Call `telemetryLoop()` from `loop()`.
- Start `netlink` in `setup()`.
- Add the setup page section, plus save handling and the test button.
- Add the info screen line.
- The netlink task pauses while the settings hotspot is active, and is released when it stops.
- No sending happens in demo mode.

## Testing

**Native Unity tests** (new suite `test/test_stats`):

- `DrainMonitor`:
  - no trigger before the window fills;
  - triggers above the limit;
  - ignores a short spike;
  - clears only after 15 min below 0.8×;
  - 60 min cooldown.
- `DayStats`:
  - integration of the known currents;
  - `usedAh` formula and clamping;
  - PV first and last minute and peak.
- `CapacityLearner`: learns at 78 % / 44 Ah → 200 Ah; ignores SOC > 90 % and 102.3; smoothing.
- `daysRemaining`: both figures; solar covers usage; fewer days than `days_avg`; no complete day.
- `SunsetDetector`: not before `evening_hour`; needs prior production that day; waits while PV ≥ 5 W until 30 min below; forced at 23:00; once per day.
- Text builders: the exact example texts above, with each omission rule and the delayed suffix.
- Batch JSON: the field omission, rounding, and 12 records under 4096 bytes.
- Envelopes: ntfy topic extraction from the URL; generic shape; sink detection for `ntfy.sh`, a subdomain, and a non-ntfy URL.

**Device checks:**

1. With a URL and WiFi set, **Send test message** arrives on ntfy.
2. A batch arrives silently every 30 min.
3. With WiFi off for 1 h then back on, the backlog arrives as hourly batches.
4. The time and offset are correct (check the report times).
5. A forced drain (e.g. `dis_amps` 0.5 with a load on) sends an alert, and an all-clear after the load is removed.
6. The morning and evening reports arrive.
7. The UI doesn't freeze during sends.
8. BLE decoding continues during sends.
9. Free heap stays above 40 KB during a send (serial log).
