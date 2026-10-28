# Online Alerts, Reports and Data Batches — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let ChargeScreen join WiFi only when it has something to send, and push high-drain alerts, a morning battery report, an evening solar report and a silent 30-minute data batch to an HTTP endpoint (first target `https://ntfy.sh/ghibli_harmony`; a generic JSON endpoint by changing only the URL).

**Architecture:** All device-independent logic (drain monitor, day statistics, capacity learning, days remaining, report timing, report texts, batch JSON, ntfy/generic envelopes, cache ring and outbox index maths, HTTP result classification, backoff, time-zone response parsing) goes into a new header-only, Arduino-free `include/chargescreen_stats.h` (namespace `cs`), unit-tested natively with Unity in five new suites. `src/telemetry.cpp/.h` owns the three LittleFS cache files behind one FreeRTOS mutex and runs the 10 s sampling and triggers from `loop()`. `src/netlink.cpp/.h` is a FreeRTOS task that owns WiFi client mode, NTP, the UTC offset lookup and HTTPS posting. `src/main.cpp` loads the settings, adds the setup-page section, the test button and the info-screen line, feeds samples, suppresses everything in demo mode and pauses netlink while the settings hotspot runs.

**Tech Stack:** PlatformIO, Arduino-ESP32 2.x (espressif32 6.13.0), NimBLE-Arduino 2.5.2, FreeRTOS, LittleFS, `WiFiClientSecure` + `HTTPClient` (framework libraries, no new `lib_deps`), Unity (native tests), MinGW-w64 gcc for native tests on Windows.

**Spec:** `design/specs/2026-10-28-online-reports-design.md`

## Global Constraints

- `platform = espressif32@6.13.0` and `h2zero/NimBLE-Arduino@2.5.2` stay pinned. Don't change library versions. Unpinned espressif32 (8.x, Arduino 3) does not build this code.
- Firmware build command: `pio run -e esp32-c3-round-dashboard`.
- Native test command, run in PowerShell with a gcc PATH refresh: `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`. A single suite: append ` -f <suite>`.
- The Windows compiler command line is close to its 32K limit. "CreateProcess: No such file or directory" means stop and report; don't add libraries with many include paths without checking. No ArduinoJson or any other new library: output JSON is hand-built with `snprintf` in `cs::`, and the two time-zone responses are read with a small tested key finder. (A dry run of this whole plan, done while writing it, built successfully with `HTTPClient` and `WiFiClientSecure` included.)
- The pre-build script rewrites `include/build_version.h`; restore it with `git checkout -- include/build_version.h` after every firmware build and never commit it.
- Commits use the repo's configured identity (`istrasoft`, no-reply email); never pass `--author` or change `user.name`/`user.email`.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- `cs::` code never calls `millis()`, `time()` or any Arduino API; time is always passed in. Text goes into caller-provided `char` buffers, never Arduino `String`.
- NVS namespace `victron` (the existing `secrets` object), keys ≤ 15 characters. New keys:

  | Key | Type | Default | Range |
  |---|---|---|---|
  | `wifi1_ssid`, `wifi2_ssid` | String | empty | ≤ 32 chars |
  | `wifi1_pass`, `wifi2_pass` | String | empty | ≤ 64 chars, "leave blank to keep" |
  | `post_url` | String | empty (feature off) | `http://` or `https://`, ≤ 191 chars |
  | `tz_override` | String | empty (automatic) | POSIX TZ, ≤ 63 chars |
  | `dis_amps` | Float | 4.0 | 0.5–50 |
  | `dis_win_min` | UShort | 45 | 15–120 |
  | `report_hour` | UChar | 10 | 4–12 |
  | `evening_hour` | UChar | 20 | 16–22 |
  | `batch_min` | UChar | 30 | 15, 30 or 60 |
  | `days_avg` | UChar | 3 | 1–7 |
  | `cap_ah` | Float | not set (NAN) | learned capacity |

- Exact values from the spec:
  - Sampling every 10 s; drain minute averages every 60 s; a cache record every 5 min.
  - Cache: 288 five-minute records (24 h), oldest overwritten; 16 pending messages, oldest dropped; 7 daily summaries plus the current day.
  - Batches: at most 12 records per batch message, at most 6 batch messages per connection; ntfy message limit 4096 bytes.
  - PV production threshold 5 W; evening report after 30 consecutive minutes without production, at 23:00 at the latest.
  - Drain clears below 0.8 × `dis_amps` for 15 consecutive minutes; 60 min cooldown after a trigger.
  - A message sent more than 10 min after its event gets the delayed suffix.
  - Capacity: SOC ≤ 90 %, not 102.3; α = 0.1; save to `cap_ah` when it moves > 2 %.
  - Backoff after a failed attempt: 1, 2, 5, 10, then 30 minutes; success resets; HTTP 429 jumps to 30 minutes.
  - Free heap gate 60 KB before connecting; WiFi join timeout 15 s per network; HTTP timeout 10 s.
  - Priorities: alert 4 (tag `warning`), all-clear 3 (`white_check_mark`), morning 3 (`sunny`), evening 3 (`city_sunset`), batch 1 (no tag), test 3 (no tag).

### Decisions and spec deviations

These were resolved while writing the plan. Each is implemented exactly as stated in the tasks below.

1. **Time-zone services (checked with real requests on 2026-10-28).**
   - `https://worldtimeapi.org/api/ip` (the spec's primary) did not answer: both HTTPS and plain HTTP connections were reset (`curl: (35) Recv failure: Connection was reset`). It is **not used**.
   - `https://timeapi.io/api/TimeZone/ip` without a query answers HTTP 400 `"The ipAddress field is required."`. With `?ipAddress=<public IP>` it answers 200 and `currentUtcOffset.seconds` is the current total offset including daylight saving (3600 for Brussels after DST ended, -25200 for Los Angeles during PDT). The public IP comes from `https://api.ipify.org` (plain-text body, e.g. `80.200.185.90`).
   - Fallback: `http://ip-api.com/json/?fields=status,message,offset` answers `{"status":"success",...,"offset":3600}`; `offset` is the current total offset including DST. Its free tier is plain HTTP only, which is acceptable because nothing private is sent or received.
   - Final order: ipify + timeapi.io, then ip-api.com. The captured responses are the test fixtures in Task 5.
2. **Local offset** is kept in RAM: looked up on the first connection after each boot and then at most once per 24 h. No reports fire until it is known (or `tz_override` is set). Without an override, local time is UTC plus the fetched offset; `TZ` stays `UTC0`.
3. **Complete day:** a day closed by local midnight with at least 6480 valid samples (18 h). Only complete days feed the days-remaining average and the "Yesterday:" line.
4. **Morning report** waits until the shunt has a valid SOC (0–100 %) at or after `report_hour`; a report without a battery figure is not sent.
5. **Feature off** (no URL or no WiFi name) or demo mode: telemetry does nothing at all (no sampling, capacity learning, day stats or file writes), and netlink sends nothing.
6. **Pending morning report replacement** is done in place: the newer report takes over the older one's outbox slot and queue position.
7. **Events without a clock time** are dated at the first NTP sync of the same boot (from `millis()`). A message from an earlier boot that was never dated gets ` (delayed)`; a record from an earlier boot that was never dated is dropped (it can't be placed in time).
8. **Info screen** has a fifth state, `Waiting`, between enabling the feature and the first attempt (the first attempt is 60 s after boot).
9. **HTTP results:** 3xx and network/TLS errors count as a failed attempt with the normal backoff, like 5xx. Skipping for low heap also counts as a failed attempt so it can't spin.
10. **WiFi password:** a blank password keeps the stored one; a changed network name with a blank password means an open network and clears the stored password.
11. **Generic batch envelope:** `kind` `batch`, title `ChargeScreen data`, message `N records`, `time` = the first record's `t`, `data` = the batch body. ntfy batch: title `ChargeScreen data`, priority 1, no tag, `message` = the batch body.
12. **PV boundary:** "producing" is PV > 5 W; exactly 5 W counts as not producing. The evening timing uses the last local minute with PV > 5 W.
13. **A test message queued from the setup page** is sent once the settings hotspot turns off, because netlink is paused while the hotspot runs. The page says so.

## File Structure

| File | Responsibility |
|---|---|
| `include/chargescreen_stats.h` (create) | Pure logic in namespace `cs` (Tasks 1–5) |
| `test/test_stats/test_main.cpp` (create) | Settings ranges, local time, `DrainMonitor` |
| `test/test_days/test_main.cpp` (create) | `DayStats`, `DayHistory`, `CapacityLearner`, `daysRemaining`, `morningDue`, `sunsetDue` |
| `test/test_reports/test_main.cpp` (create) | Message kinds, report texts, delayed suffix, info-screen text |
| `test/test_payloads/test_main.cpp` (create) | 5-minute records, batch JSON, sink detection, envelopes |
| `test/test_netproto/test_main.cpp` (create) | Cache ring and outbox index, HTTP results, backoff, time-zone parsing with real fixtures |
| `src/telemetry.h` / `src/telemetry.cpp` (create) | Cache store (`/tx_cache.bin`, `/outbox.bin`, `/days.bin`), 10 s sampling and triggers |
| `src/netlink.h` / `src/netlink.cpp` (create) | WiFi client task, NTP, offset lookup, HTTPS posting, backoff, status, `clockNow()` |
| `src/main.cpp` (modify) | Settings load/save, setup page Online section, test button, info line, sample feed, demo suppression, hotspot pause |
| `README.md` (modify) | "Online reports" section |

---

### Task 0: Baseline and time-service check

**Files:** none modified.

**Interfaces:**
- Consumes: nothing.
- Produces: confirmation that the current tree builds and its tests pass, and that the two time-zone services used in Task 5 still answer in the captured format.

- [ ] **Step 1: Build the unmodified firmware**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: ends with `[SUCCESS]`. Note the `RAM:` and `Flash:` lines for comparison. Then run `git checkout -- include/build_version.h`.

If the build fails, stop and report the full error.

- [ ] **Step 2: Run the existing native tests**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`
Expected: `30 test cases: 30 succeeded` (suites `test_lock`, `test_orion`, `test_pages`).

- [ ] **Step 3: Confirm the time-zone response formats with real requests**

Run each in PowerShell:

```
curl.exe -s https://api.ipify.org
curl.exe -s "https://timeapi.io/api/TimeZone/ip?ipAddress=8.8.8.8"
curl.exe -s "http://ip-api.com/json/8.8.8.8?fields=status,message,offset"
curl.exe -s "http://ip-api.com/json/10.0.0.1?fields=status,message,offset"
curl.exe -s -m 20 https://worldtimeapi.org/api/ip
```

Expected:
- ipify: a bare IP address such as `80.200.185.90`.
- timeapi.io: JSON containing `"currentUtcOffset":{"seconds":-25200,...}` or `{"seconds":-28800,...}` (Los Angeles, depending on DST), followed later by `"standardUtcOffset":{...}`.
- ip-api.com (8.8.8.8): `{"status":"success","offset":-14400}` or `-18000` (field order may differ).
- ip-api.com (10.0.0.1): `{"status":"fail","message":"private range"}`.
- worldtimeapi.org: nothing or a connection error. It is not used (see Decisions 1).

If timeapi.io no longer returns `currentUtcOffset.seconds`, or ip-api.com no longer returns `status` and `offset`, stop and report the new response.

- [ ] **Step 4: Confirm the tree is clean**

Run: `git status --short`
Expected: no output.

No commit.

---

### Task 1: Stats header — settings, local time, drain monitor

**Files:**
- Create: `include/chargescreen_stats.h`
- Create: `test/test_stats/test_main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (namespace `cs`, all `inline`):
  - Constants `SAMPLE_PERIOD_MS` (10000), `RECORD_PERIOD_MS` (300000), `SAMPLES_PER_MINUTE` (6), `RECORD_RING_SIZE` (288), `OUTBOX_SIZE` (16), `RECORDS_PER_BATCH` (12), `BATCHES_PER_CONNECTION` (6), `NTFY_MESSAGE_LIMIT` (4096), `PV_PRODUCING_WATTS` (5.0f), `SUNSET_QUIET_MINUTES` (30), `EVENING_LATEST_MINUTE` (1380), `DRAIN_CLEAR_FACTOR` (0.8f), `DRAIN_CLEAR_MINUTES` (15), `DRAIN_COOLDOWN_MINUTES` (60), `DRAIN_MAX_WINDOW_MIN` (120), `DELAYED_AFTER_S` (600), `CLOCK_VALID_AFTER` (1767225600), `NO_DAY` (`INT32_MIN`)
  - `struct OnlineSettings { float disAmps; uint16_t disWinMin; uint8_t reportHour; uint8_t eveningHour; uint8_t batchMin; uint8_t daysAvg; }` (defaults 4.0, 45, 10, 20, 30, 3)
  - `float sanitiseDisAmps(float)`, `uint16_t sanitiseDisWinMin(int)`, `uint8_t sanitiseReportHour(int)`, `uint8_t sanitiseEveningHour(int)`, `uint8_t sanitiseBatchMin(int)`, `uint8_t sanitiseDaysAvg(int)`
  - `bool onlineConfigured(const char *postUrl, const char *ssid1, const char *ssid2)`
  - `struct ClockNow { uint32_t unixUtc; bool utcValid; int32_t offsetS; bool localValid; }`
  - `bool clockValid(uint32_t unixUtc)`, `int32_t localDayNumber(uint32_t unixUtc, int32_t offsetS)`, `uint16_t localMinuteOfDay(uint32_t unixUtc, int32_t offsetS)`, `void formatHhMm(uint16_t minuteOfDay, char out[6])`
  - `enum class DrainEvent : uint8_t { None, Trigger, Clear }`
  - `struct DrainMonitor` (fields `minuteAvg[120]`, `head`, `count`, `sampleSum`, `sampleCount`, `active`, `belowMinutes`, `minuteCounter`, `lastTriggerMinute`, `everTriggered`)
  - `void drainAddSample(DrainMonitor &, float netCurrentA)`, `float drainWindowAverage(const DrainMonitor &, uint16_t windowMin)`, `DrainEvent drainCloseMinute(DrainMonitor &, float limitA, uint16_t windowMin)`

- [ ] **Step 1: Write the failing tests**

Create `test/test_stats/test_main.cpp`:

```cpp
#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

// One minute of 10 s samples at a constant discharge (positive amps).
static cs::DrainEvent feedMinute(cs::DrainMonitor &m, float dischargeA, float limitA, uint16_t windowMin) {
  for (int i = 0; i < 6; i++) {
    cs::drainAddSample(m, -dischargeA);
  }
  return cs::drainCloseMinute(m, limitA, windowMin);
}

static void test_settings_sanitisers() {
  TEST_ASSERT_EQUAL_FLOAT(4.0f, cs::sanitiseDisAmps(NAN));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, cs::sanitiseDisAmps(0.1f));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, cs::sanitiseDisAmps(80.0f));
  TEST_ASSERT_EQUAL_FLOAT(6.5f, cs::sanitiseDisAmps(6.5f));
  TEST_ASSERT_EQUAL_UINT16(15, cs::sanitiseDisWinMin(5));
  TEST_ASSERT_EQUAL_UINT16(45, cs::sanitiseDisWinMin(45));
  TEST_ASSERT_EQUAL_UINT16(120, cs::sanitiseDisWinMin(500));
  TEST_ASSERT_EQUAL_UINT8(4, cs::sanitiseReportHour(1));
  TEST_ASSERT_EQUAL_UINT8(12, cs::sanitiseReportHour(13));
  TEST_ASSERT_EQUAL_UINT8(16, cs::sanitiseEveningHour(10));
  TEST_ASSERT_EQUAL_UINT8(22, cs::sanitiseEveningHour(23));
  TEST_ASSERT_EQUAL_UINT8(15, cs::sanitiseBatchMin(15));
  TEST_ASSERT_EQUAL_UINT8(60, cs::sanitiseBatchMin(60));
  TEST_ASSERT_EQUAL_UINT8(30, cs::sanitiseBatchMin(20));
  TEST_ASSERT_EQUAL_UINT8(1, cs::sanitiseDaysAvg(0));
  TEST_ASSERT_EQUAL_UINT8(7, cs::sanitiseDaysAvg(9));
  cs::OnlineSettings defaults;
  TEST_ASSERT_EQUAL_FLOAT(4.0f, defaults.disAmps);
  TEST_ASSERT_EQUAL_UINT16(45, defaults.disWinMin);
  TEST_ASSERT_EQUAL_UINT8(10, defaults.reportHour);
  TEST_ASSERT_EQUAL_UINT8(20, defaults.eveningHour);
  TEST_ASSERT_EQUAL_UINT8(30, defaults.batchMin);
  TEST_ASSERT_EQUAL_UINT8(3, defaults.daysAvg);
}

static void test_online_configured() {
  TEST_ASSERT_FALSE(cs::onlineConfigured("", "van", ""));
  TEST_ASSERT_FALSE(cs::onlineConfigured("https://ntfy.sh/x", "", ""));
  TEST_ASSERT_TRUE(cs::onlineConfigured("https://ntfy.sh/x", "van", ""));
  TEST_ASSERT_TRUE(cs::onlineConfigured("https://ntfy.sh/x", "", "phone"));
}

static void test_local_day_and_minute() {
  // 2026-10-28T22:53:17Z, Brussels +3600 -> 23:53 local, same day.
  const uint32_t t = 1793227997UL;
  TEST_ASSERT_TRUE(cs::clockValid(t));
  TEST_ASSERT_FALSE(cs::clockValid(5000));
  TEST_ASSERT_EQUAL_UINT16(22 * 60 + 53, cs::localMinuteOfDay(t, 0));
  TEST_ASSERT_EQUAL_UINT16(23 * 60 + 53, cs::localMinuteOfDay(t, 3600));
  TEST_ASSERT_EQUAL_INT32(cs::localDayNumber(t, 0), cs::localDayNumber(t, 3600));
  // +7200 crosses local midnight: next day, 00:53.
  TEST_ASSERT_EQUAL_INT32(cs::localDayNumber(t, 0) + 1, cs::localDayNumber(t, 7200));
  TEST_ASSERT_EQUAL_UINT16(53, cs::localMinuteOfDay(t, 7200));
  // Los Angeles -25200 -> 15:53 local.
  TEST_ASSERT_EQUAL_UINT16(15 * 60 + 53, cs::localMinuteOfDay(t, -25200));
  char hm[6];
  cs::formatHhMm(7 * 60 + 48, hm);
  TEST_ASSERT_EQUAL_STRING("07:48", hm);
  cs::formatHhMm(23 * 60 + 5, hm);
  TEST_ASSERT_EQUAL_STRING("23:05", hm);
}

static void test_drain_no_trigger_before_window_fills() {
  cs::DrainMonitor m;
  for (int minute = 1; minute < 45; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 10.0f, 4.0f, 45));
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 10.0f, 4.0f, 45));
  TEST_ASSERT_TRUE(m.active);
}

static void test_drain_triggers_only_above_limit() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 15; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 4.0f, 4.0f, 15));
  }
  TEST_ASSERT_EQUAL_FLOAT(4.0f, cs::drainWindowAverage(m, 15));
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 4.6f, 4.0f, 15));
}

static void test_drain_ignores_short_spike() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 14; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 1.0f, 4.0f, 15));
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 30.0f, 4.0f, 15));
  TEST_ASSERT_FALSE(m.active);
}

static void test_drain_charging_is_not_drain() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 20; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, -20.0f, 4.0f, 15));
  }
}

static void test_drain_clears_after_15_minutes_below_80_percent() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 14; minute++) {
    feedMinute(m, 6.0f, 4.0f, 15);
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 6.0f, 4.0f, 15));
  // At 0 A the 15 min average drops below 3.2 A after 8 minutes, and the
  // all-clear needs 15 consecutive minutes below: minute 22.
  for (int k = 1; k <= 21; k++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 0.0f, 4.0f, 15));
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Clear, feedMinute(m, 0.0f, 4.0f, 15));
  TEST_ASSERT_FALSE(m.active);
}

static void test_drain_clear_counter_resets_when_average_rises() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 15; minute++) {
    feedMinute(m, 6.0f, 4.0f, 15);
  }
  TEST_ASSERT_TRUE(m.active);
  for (int k = 0; k < 10; k++) {
    feedMinute(m, 0.0f, 4.0f, 15);
  }
  TEST_ASSERT_TRUE(m.belowMinutes > 0);
  feedMinute(m, 60.0f, 4.0f, 15);
  TEST_ASSERT_EQUAL_UINT16(0, m.belowMinutes);
  TEST_ASSERT_TRUE(m.active);
}

static void test_drain_cooldown_60_minutes() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 14; minute++) {
    feedMinute(m, 6.0f, 4.0f, 15);
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 6.0f, 4.0f, 15));  // minute 15
  for (int k = 0; k < 21; k++) {
    feedMinute(m, 0.0f, 4.0f, 15);
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Clear, feedMinute(m, 0.0f, 4.0f, 15));  // minute 37
  int triggerAt = -1;
  for (int minute = 38; minute <= 100; minute++) {
    if (feedMinute(m, 10.0f, 4.0f, 15) == cs::DrainEvent::Trigger && triggerAt < 0) {
      triggerAt = minute;
    }
  }
  TEST_ASSERT_EQUAL_INT(75, triggerAt);
}

static void test_drain_minute_without_samples_is_skipped() {
  cs::DrainMonitor m;
  feedMinute(m, 2.0f, 4.0f, 15);
  TEST_ASSERT_EQUAL(cs::DrainEvent::None, cs::drainCloseMinute(m, 4.0f, 15));
  TEST_ASSERT_EQUAL_UINT8(1, m.count);
  TEST_ASSERT_EQUAL_UINT32(2, m.minuteCounter);
  cs::drainAddSample(m, NAN);
  TEST_ASSERT_EQUAL_UINT8(0, m.sampleCount);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_settings_sanitisers);
  RUN_TEST(test_online_configured);
  RUN_TEST(test_local_day_and_minute);
  RUN_TEST(test_drain_no_trigger_before_window_fills);
  RUN_TEST(test_drain_triggers_only_above_limit);
  RUN_TEST(test_drain_ignores_short_spike);
  RUN_TEST(test_drain_charging_is_not_drain);
  RUN_TEST(test_drain_clears_after_15_minutes_below_80_percent);
  RUN_TEST(test_drain_clear_counter_resets_when_average_rises);
  RUN_TEST(test_drain_cooldown_60_minutes);
  RUN_TEST(test_drain_minute_without_samples_is_skipped);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_stats`
Expected: FAIL, compile error `chargescreen_stats.h: No such file or directory`.

- [ ] **Step 3: Create the header**

Create `include/chargescreen_stats.h`:

```cpp
#pragma once

#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Online alerts, reports and data batches: device-independent logic.
// No Arduino dependencies and no clock reads: every time value is passed in,
// so all of this is unit-tested natively (pio test -e native).
namespace cs {

constexpr uint32_t SAMPLE_PERIOD_MS = 10000;
constexpr uint32_t RECORD_PERIOD_MS = 300000;
constexpr uint8_t SAMPLES_PER_MINUTE = 6;
constexpr uint16_t RECORD_RING_SIZE = 288;
constexpr uint8_t OUTBOX_SIZE = 16;
constexpr uint8_t RECORDS_PER_BATCH = 12;
constexpr uint8_t BATCHES_PER_CONNECTION = 6;
constexpr size_t NTFY_MESSAGE_LIMIT = 4096;
constexpr float PV_PRODUCING_WATTS = 5.0f;
constexpr uint16_t SUNSET_QUIET_MINUTES = 30;
constexpr uint16_t EVENING_LATEST_MINUTE = 23 * 60;
constexpr float DRAIN_CLEAR_FACTOR = 0.8f;
constexpr uint16_t DRAIN_CLEAR_MINUTES = 15;
constexpr uint16_t DRAIN_COOLDOWN_MINUTES = 60;
constexpr uint8_t DRAIN_MAX_WINDOW_MIN = 120;
constexpr uint32_t DELAYED_AFTER_S = 600;
constexpr uint32_t CLOCK_VALID_AFTER = 1767225600UL;  // 2026-01-01T00:00:00Z
constexpr int32_t NO_DAY = INT32_MIN;

// ---------------------------------------------------------------- settings

struct OnlineSettings {
  float disAmps = 4.0f;
  uint16_t disWinMin = 45;
  uint8_t reportHour = 10;
  uint8_t eveningHour = 20;
  uint8_t batchMin = 30;
  uint8_t daysAvg = 3;
};

inline int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline float sanitiseDisAmps(float v) {
  if (std::isnan(v)) {
    return 4.0f;
  }
  return v < 0.5f ? 0.5f : (v > 50.0f ? 50.0f : v);
}

inline uint16_t sanitiseDisWinMin(int v) {
  return static_cast<uint16_t>(clampInt(v, 15, 120));
}

inline uint8_t sanitiseReportHour(int v) {
  return static_cast<uint8_t>(clampInt(v, 4, 12));
}

inline uint8_t sanitiseEveningHour(int v) {
  return static_cast<uint8_t>(clampInt(v, 16, 22));
}

inline uint8_t sanitiseBatchMin(int v) {
  return (v == 15 || v == 30 || v == 60) ? static_cast<uint8_t>(v) : 30;
}

inline uint8_t sanitiseDaysAvg(int v) {
  return static_cast<uint8_t>(clampInt(v, 1, 7));
}

// The feature is off while the URL is empty or no WiFi name is set.
inline bool onlineConfigured(const char *postUrl, const char *ssid1, const char *ssid2) {
  return postUrl && postUrl[0] && ((ssid1 && ssid1[0]) || (ssid2 && ssid2[0]));
}

// ---------------------------------------------------------------- time

struct ClockNow {
  uint32_t unixUtc = 0;
  bool utcValid = false;
  int32_t offsetS = 0;
  bool localValid = false;
};

inline bool clockValid(uint32_t unixUtc) {
  return unixUtc >= CLOCK_VALID_AFTER;
}

inline int32_t localDayNumber(uint32_t unixUtc, int32_t offsetS) {
  int64_t local = static_cast<int64_t>(unixUtc) + offsetS;
  int64_t day = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
  return static_cast<int32_t>(day);
}

inline uint16_t localMinuteOfDay(uint32_t unixUtc, int32_t offsetS) {
  int64_t seconds = (static_cast<int64_t>(unixUtc) + offsetS) % 86400;
  if (seconds < 0) {
    seconds += 86400;
  }
  return static_cast<uint16_t>(seconds / 60);
}

inline void formatHhMm(uint16_t minuteOfDay, char out[6]) {
  unsigned hours = (minuteOfDay / 60U) % 24U;
  unsigned minutes = minuteOfDay % 60U;
  out[0] = static_cast<char>('0' + hours / 10);
  out[1] = static_cast<char>('0' + hours % 10);
  out[2] = ':';
  out[3] = static_cast<char>('0' + minutes / 10);
  out[4] = static_cast<char>('0' + minutes % 10);
  out[5] = '\0';
}

// ---------------------------------------------------------------- drain

enum class DrainEvent : uint8_t { None, Trigger, Clear };

struct DrainMonitor {
  float minuteAvg[DRAIN_MAX_WINDOW_MIN] = {};
  uint8_t head = 0;   // next write index
  uint8_t count = 0;  // stored minute averages
  float sampleSum = 0.0f;
  uint8_t sampleCount = 0;
  bool active = false;
  uint16_t belowMinutes = 0;
  uint32_t minuteCounter = 0;
  uint32_t lastTriggerMinute = 0;
  bool everTriggered = false;
};

// netCurrentA is the shunt current (+ charging); discharge is stored positive.
inline void drainAddSample(DrainMonitor &m, float netCurrentA) {
  if (std::isnan(netCurrentA)) {
    return;
  }
  m.sampleSum += -netCurrentA;
  if (m.sampleCount < 255) {
    m.sampleCount++;
  }
}

inline float drainWindowAverage(const DrainMonitor &m, uint16_t windowMin) {
  uint16_t n = windowMin < m.count ? windowMin : m.count;
  if (n == 0) {
    return NAN;
  }
  float sum = 0.0f;
  for (uint16_t i = 0; i < n; i++) {
    uint16_t index = (m.head + DRAIN_MAX_WINDOW_MIN - 1 - i) % DRAIN_MAX_WINDOW_MIN;
    sum += m.minuteAvg[index];
  }
  return sum / n;
}

// Call once per minute. A minute with no samples is not stored.
inline DrainEvent drainCloseMinute(DrainMonitor &m, float limitA, uint16_t windowMin) {
  m.minuteCounter++;
  if (windowMin > DRAIN_MAX_WINDOW_MIN) {
    windowMin = DRAIN_MAX_WINDOW_MIN;
  }
  if (m.sampleCount > 0) {
    m.minuteAvg[m.head] = m.sampleSum / m.sampleCount;
    m.head = static_cast<uint8_t>((m.head + 1) % DRAIN_MAX_WINDOW_MIN);
    if (m.count < DRAIN_MAX_WINDOW_MIN) {
      m.count++;
    }
  }
  m.sampleSum = 0.0f;
  m.sampleCount = 0;
  if (m.count < windowMin) {
    return DrainEvent::None;
  }
  float avg = drainWindowAverage(m, windowMin);
  if (!m.active) {
    bool cooled = !m.everTriggered || m.minuteCounter - m.lastTriggerMinute >= DRAIN_COOLDOWN_MINUTES;
    if (avg > limitA && cooled) {
      m.active = true;
      m.belowMinutes = 0;
      m.lastTriggerMinute = m.minuteCounter;
      m.everTriggered = true;
      return DrainEvent::Trigger;
    }
    return DrainEvent::None;
  }
  if (avg < DRAIN_CLEAR_FACTOR * limitA) {
    m.belowMinutes++;
    if (m.belowMinutes >= DRAIN_CLEAR_MINUTES) {
      m.active = false;
      m.belowMinutes = 0;
      return DrainEvent::Clear;
    }
  } else {
    m.belowMinutes = 0;
  }
  return DrainEvent::None;
}

}  // namespace cs
```

- [ ] **Step 4: Run tests to verify they pass**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_stats`
Expected: `11 Tests 0 Failures 0 Ignored` and `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add include/chargescreen_stats.h test/test_stats/test_main.cpp
git commit -m "Add online stats header with settings, local time and drain monitor

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Day statistics, capacity, days remaining, report timing

**Files:**
- Modify: `include/chargescreen_stats.h`
- Create: `test/test_days/test_main.cpp`

**Interfaces:**
- Consumes: `NO_DAY`, `PV_PRODUCING_WATTS`, `SUNSET_QUIET_MINUTES`, `EVENING_LATEST_MINUTE` (Task 1).
- Produces (namespace `cs`):
  - `struct Sample { bool shuntValid; float currentA; float soc; float consumedAh; bool solarValid; float solarBatA; float pvW; bool orionValid; float orionOutA; }` (`consumedAh` positive; NAN = unknown)
  - `bool socKnown(float soc)`, `float finiteOr0(float)`
  - `DAY_HISTORY_DAYS` (7), `DAY_COMPLETE_MIN_SAMPLES` (6480)
  - `struct DayStats { int32_t day; float netAh, solarAh, orionAh, solarWh, pvPeakW; int16_t pvPeakMinute, pvFirstMinute, pvLastMinute; uint32_t samples; }`
  - `void dayAddSample(DayStats &, const Sample &, uint32_t dtS, uint16_t localMinute)`, `float dayUsedAh(const DayStats &)`, `bool dayComplete(const DayStats &)`
  - `struct DayHistory { DayStats current; DayStats days[7]; uint8_t count; int32_t morningSentDay; int32_t eveningSentDay; }`, `void dayHistoryRoll(DayHistory &, int32_t today)`, `const DayStats *dayHistoryFind(const DayHistory &, int32_t day)`
  - `CAPACITY_ALPHA`, `CAPACITY_MAX_SOC`, `CAPACITY_PERSIST_CHANGE`; `struct CapacityLearner { float capacityAh; }`, `bool capacityUpdate(CapacityLearner &, float soc, float consumedAh)`, `bool capacityShouldPersist(float learned, float stored)`, `float remainingAh(const CapacityLearner &, float soc)`
  - `struct DaysEstimate { bool valid; uint8_t daysUsed; bool showSolar; bool solarCovers; float withSolar; float noSun; }`, `DaysEstimate daysRemaining(const DayHistory &, uint8_t daysAvg, float remaining, bool hasSolarKey)`
  - `bool morningDue(int32_t morningSentDay, int32_t localDay, uint16_t localMinute, uint8_t reportHour)`
  - `bool sunsetDue(int32_t eveningSentDay, const DayStats &today, int32_t localDay, uint16_t localMinute, uint8_t eveningHour)` (the spec's `SunsetDetector`)

- [ ] **Step 1: Write the failing tests**

Create `test/test_days/test_main.cpp`:

```cpp
#include <unity.h>

#include <cmath>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static cs::Sample shuntSample(float currentA) {
  cs::Sample s;
  s.shuntValid = true;
  s.currentA = currentA;
  s.soc = 80.0f;
  s.consumedAh = 40.0f;
  return s;
}

// A closed day with the given totals that counts as complete.
static cs::DayStats completeDay(int32_t day, float usedAh, float solarAh) {
  cs::DayStats d;
  d.day = day;
  d.solarAh = solarAh;
  d.netAh = solarAh - usedAh;
  d.samples = 8640;
  return d;
}

static void test_day_integrates_known_currents() {
  cs::DayStats d;
  cs::Sample s = shuntSample(-5.0f);
  s.solarValid = true;
  s.solarBatA = 3.0f;
  s.pvW = 120.0f;
  s.orionValid = true;
  s.orionOutA = 2.0f;
  for (int i = 0; i < 360; i++) {  // one hour of 10 s samples
    cs::dayAddSample(d, s, 10, 600);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, d.netAh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, d.solarAh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, d.orionAh);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 120.0f, d.solarWh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, cs::dayUsedAh(d));
  TEST_ASSERT_EQUAL_UINT32(360, d.samples);
}

static void test_day_used_is_clamped_at_zero() {
  cs::DayStats d;
  d.netAh = 10.0f;
  d.solarAh = 3.0f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, cs::dayUsedAh(d));
}

static void test_day_missing_devices_add_zero_and_invalid_shunt_skips() {
  cs::DayStats d;
  cs::Sample s = shuntSample(-3.6f);
  s.solarValid = false;
  s.solarBatA = 50.0f;
  s.orionValid = true;
  s.orionOutA = NAN;
  cs::dayAddSample(d, s, 3600, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -3.6f, d.netAh);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.solarAh);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.orionAh);
  TEST_ASSERT_EQUAL_UINT32(1, d.samples);
  cs::Sample bad = shuntSample(-3.6f);
  bad.shuntValid = false;
  bad.solarValid = true;
  bad.solarBatA = 10.0f;
  cs::dayAddSample(d, bad, 3600, 1);
  TEST_ASSERT_EQUAL_UINT32(1, d.samples);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.solarAh);
}

static void test_day_pv_first_last_and_peak() {
  cs::DayStats d;
  cs::Sample s = shuntSample(0.0f);
  s.solarValid = true;
  s.solarBatA = 0.0f;
  const struct {
    uint16_t minute;
    float pv;
  } points[] = {
      {7 * 60 + 40, 3.0f}, {7 * 60 + 48, 6.0f}, {13 * 60 + 12, 286.0f}, {15 * 60, 150.0f},
      {19 * 60 + 26, 5.5f}, {19 * 60 + 30, 5.0f}, {20 * 60, 0.0f},
  };
  for (const auto &p : points) {
    s.pvW = p.pv;
    cs::dayAddSample(d, s, 10, p.minute);
  }
  TEST_ASSERT_EQUAL_INT16(7 * 60 + 48, d.pvFirstMinute);
  TEST_ASSERT_EQUAL_INT16(19 * 60 + 26, d.pvLastMinute);
  TEST_ASSERT_EQUAL_FLOAT(286.0f, d.pvPeakW);
  TEST_ASSERT_EQUAL_INT16(13 * 60 + 12, d.pvPeakMinute);
}

static void test_day_history_roll_and_find() {
  cs::DayHistory h;
  cs::dayHistoryRoll(h, 100);
  TEST_ASSERT_EQUAL_INT32(100, h.current.day);
  TEST_ASSERT_EQUAL_UINT8(0, h.count);
  h.current.netAh = -1.0f;
  cs::dayHistoryRoll(h, 100);
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, h.current.netAh);
  for (int32_t day = 101; day <= 110; day++) {
    cs::dayHistoryRoll(h, day);
  }
  TEST_ASSERT_EQUAL_UINT8(7, h.count);
  TEST_ASSERT_EQUAL_INT32(110, h.current.day);
  TEST_ASSERT_EQUAL_INT32(109, h.days[0].day);
  TEST_ASSERT_EQUAL_INT32(103, h.days[6].day);
  TEST_ASSERT_NOT_NULL(cs::dayHistoryFind(h, 105));
  TEST_ASSERT_NULL(cs::dayHistoryFind(h, 100));
}

static void test_capacity_learns_200ah() {
  cs::CapacityLearner c;
  TEST_ASSERT_TRUE(cs::capacityUpdate(c, 78.0f, 44.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, c.capacityAh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 156.0f, cs::remainingAh(c, 78.0f));
}

static void test_capacity_ignores_high_or_unknown_soc() {
  cs::CapacityLearner c;
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 91.0f, 10.0f));
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 102.3f, 10.0f));
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 50.0f, NAN));
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 50.0f, 0.0f));
  TEST_ASSERT_TRUE(std::isnan(c.capacityAh));
  TEST_ASSERT_TRUE(std::isnan(cs::remainingAh(c, 50.0f)));
  TEST_ASSERT_TRUE(cs::capacityUpdate(c, 90.0f, 20.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, c.capacityAh);
  TEST_ASSERT_TRUE(std::isnan(cs::remainingAh(c, 102.3f)));
}

static void test_capacity_smoothing_and_persist_rule() {
  cs::CapacityLearner c;
  cs::capacityUpdate(c, 78.0f, 44.0f);         // 200
  TEST_ASSERT_TRUE(cs::capacityUpdate(c, 78.0f, 55.0f));  // raw 250 -> 205
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 205.0f, c.capacityAh);
  TEST_ASSERT_TRUE(cs::capacityShouldPersist(205.0f, NAN));
  TEST_ASSERT_FALSE(cs::capacityShouldPersist(203.0f, 200.0f));
  TEST_ASSERT_TRUE(cs::capacityShouldPersist(205.0f, 200.0f));
  TEST_ASSERT_FALSE(cs::capacityShouldPersist(NAN, 200.0f));
}

static void test_days_remaining_both_figures() {
  cs::DayHistory h;
  h.days[0] = completeDay(10, 100.0f, 50.0f);
  h.days[1] = completeDay(9, 100.0f, 50.0f);
  h.days[2] = completeDay(8, 100.0f, 50.0f);
  h.count = 3;
  cs::DaysEstimate e = cs::daysRemaining(h, 3, 200.0f, true);
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_EQUAL_UINT8(3, e.daysUsed);
  TEST_ASSERT_TRUE(e.showSolar);
  TEST_ASSERT_FALSE(e.solarCovers);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, e.withSolar);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, e.noSun);
  cs::DaysEstimate noKey = cs::daysRemaining(h, 3, 200.0f, false);
  TEST_ASSERT_FALSE(noKey.showSolar);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, noKey.noSun);
}

static void test_days_remaining_solar_covers_usage() {
  cs::DayHistory h;
  h.days[0] = completeDay(10, 40.0f, 60.0f);
  h.count = 1;
  cs::DaysEstimate e = cs::daysRemaining(h, 3, 150.0f, true);
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_TRUE(e.solarCovers);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.75f, e.noSun);
}

static void test_days_remaining_uses_fewer_days_and_skips_incomplete() {
  cs::DayHistory h;
  h.days[0] = completeDay(10, 60.0f, 0.0f);
  h.days[1] = completeDay(9, 60.0f, 0.0f);
  h.days[1].samples = 100;  // partial day, not counted
  h.days[2] = completeDay(8, 40.0f, 0.0f);
  h.count = 3;
  cs::DaysEstimate e = cs::daysRemaining(h, 3, 100.0f, true);
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_EQUAL_UINT8(2, e.daysUsed);
  TEST_ASSERT_FALSE(e.showSolar);  // avgSolar is 0
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, e.noSun);
  cs::DaysEstimate one = cs::daysRemaining(h, 1, 100.0f, true);
  TEST_ASSERT_EQUAL_UINT8(1, one.daysUsed);
}

static void test_days_remaining_needs_complete_day_and_capacity() {
  cs::DayHistory h;
  TEST_ASSERT_FALSE(cs::daysRemaining(h, 3, 100.0f, true).valid);
  h.days[0] = completeDay(10, 60.0f, 0.0f);
  h.days[0].samples = 10;
  h.count = 1;
  TEST_ASSERT_FALSE(cs::daysRemaining(h, 3, 100.0f, true).valid);
  h.days[0].samples = 8640;
  TEST_ASSERT_FALSE(cs::daysRemaining(h, 3, NAN, true).valid);
  TEST_ASSERT_TRUE(cs::daysRemaining(h, 3, 100.0f, true).valid);
}

static void test_morning_due() {
  TEST_ASSERT_FALSE(cs::morningDue(cs::NO_DAY, 50, 9 * 60 + 59, 10));
  TEST_ASSERT_TRUE(cs::morningDue(cs::NO_DAY, 50, 10 * 60, 10));
  TEST_ASSERT_TRUE(cs::morningDue(49, 50, 15 * 60, 10));
  TEST_ASSERT_FALSE(cs::morningDue(50, 50, 15 * 60, 10));
}

static cs::DayStats producedUntil(int32_t day, int16_t lastMinute) {
  cs::DayStats d;
  d.day = day;
  d.pvFirstMinute = 8 * 60;
  d.pvLastMinute = lastMinute;
  return d;
}

static void test_sunset_not_before_evening_hour() {
  cs::DayStats d = producedUntil(50, 17 * 60);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 19 * 60 + 59, 20));
  TEST_ASSERT_TRUE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60, 20));
}

static void test_sunset_needs_production_that_day() {
  cs::DayStats d = producedUntil(50, -1);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 21 * 60, 20));
  cs::DayStats yesterday = producedUntil(49, 17 * 60);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, yesterday, 50, 21 * 60, 20));
}

static void test_sunset_waits_for_30_quiet_minutes() {
  cs::DayStats d = producedUntil(50, 20 * 60 + 10);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60 + 10, 20));
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60 + 39, 20));
  TEST_ASSERT_TRUE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60 + 40, 20));
}

static void test_sunset_forced_at_23() {
  cs::DayStats d = producedUntil(50, 22 * 60 + 59);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 22 * 60 + 59, 20));
  TEST_ASSERT_TRUE(cs::sunsetDue(cs::NO_DAY, d, 50, 23 * 60, 20));
}

static void test_sunset_once_per_day() {
  cs::DayStats d = producedUntil(50, 17 * 60);
  TEST_ASSERT_FALSE(cs::sunsetDue(50, d, 50, 21 * 60, 20));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_day_integrates_known_currents);
  RUN_TEST(test_day_used_is_clamped_at_zero);
  RUN_TEST(test_day_missing_devices_add_zero_and_invalid_shunt_skips);
  RUN_TEST(test_day_pv_first_last_and_peak);
  RUN_TEST(test_day_history_roll_and_find);
  RUN_TEST(test_capacity_learns_200ah);
  RUN_TEST(test_capacity_ignores_high_or_unknown_soc);
  RUN_TEST(test_capacity_smoothing_and_persist_rule);
  RUN_TEST(test_days_remaining_both_figures);
  RUN_TEST(test_days_remaining_solar_covers_usage);
  RUN_TEST(test_days_remaining_uses_fewer_days_and_skips_incomplete);
  RUN_TEST(test_days_remaining_needs_complete_day_and_capacity);
  RUN_TEST(test_morning_due);
  RUN_TEST(test_sunset_not_before_evening_hour);
  RUN_TEST(test_sunset_needs_production_that_day);
  RUN_TEST(test_sunset_waits_for_30_quiet_minutes);
  RUN_TEST(test_sunset_forced_at_23);
  RUN_TEST(test_sunset_once_per_day);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_days`
Expected: FAIL, compile errors such as `'Sample' is not a member of 'cs'`.

- [ ] **Step 3: Implement in the header**

In `include/chargescreen_stats.h`, directly before the final `}  // namespace cs`, add:

```cpp
// ---------------------------------------------------------------- samples

// One 10 s snapshot of the dashboard values. Invalid devices have valid=false.
struct Sample {
  bool shuntValid = false;
  float currentA = NAN;    // net shunt current, + = charging
  float soc = NAN;         // %, 102.3 = "not available"
  float consumedAh = NAN;  // positive Ah, NAN = unknown
  bool solarValid = false;
  float solarBatA = NAN;   // solar charger battery current
  float pvW = NAN;
  bool orionValid = false;
  float orionOutA = NAN;   // Orion XS output current
};

inline bool socKnown(float soc) {
  return !std::isnan(soc) && soc >= 0.0f && soc <= 100.05f;
}

inline float finiteOr0(float v) {
  return std::isfinite(v) ? v : 0.0f;
}

// ---------------------------------------------------------------- days

constexpr uint8_t DAY_HISTORY_DAYS = 7;
constexpr uint32_t DAY_COMPLETE_MIN_SAMPLES = 6480;  // 18 h of 10 s samples

struct DayStats {
  int32_t day = NO_DAY;  // local day number, see localDayNumber()
  float netAh = 0.0f;
  float solarAh = 0.0f;
  float orionAh = 0.0f;
  float solarWh = 0.0f;
  float pvPeakW = 0.0f;
  int16_t pvPeakMinute = -1;
  int16_t pvFirstMinute = -1;  // first local minute with PV > 5 W
  int16_t pvLastMinute = -1;   // last local minute with PV > 5 W
  uint32_t samples = 0;
};

// Rectangle rule. Skipped entirely when the shunt is invalid; an invalid
// solar charger or Orion adds 0 for that device.
inline void dayAddSample(DayStats &d, const Sample &s, uint32_t dtS, uint16_t localMinute) {
  if (!s.shuntValid || std::isnan(s.currentA)) {
    return;
  }
  float hours = static_cast<float>(dtS) / 3600.0f;
  d.netAh += s.currentA * hours;
  if (s.solarValid) {
    d.solarAh += finiteOr0(s.solarBatA) * hours;
    float pv = finiteOr0(s.pvW);
    d.solarWh += pv * hours;
    if (pv > PV_PRODUCING_WATTS) {
      if (d.pvFirstMinute < 0) {
        d.pvFirstMinute = static_cast<int16_t>(localMinute);
      }
      d.pvLastMinute = static_cast<int16_t>(localMinute);
    }
    if (pv > d.pvPeakW) {
      d.pvPeakW = pv;
      d.pvPeakMinute = static_cast<int16_t>(localMinute);
    }
  }
  if (s.orionValid) {
    d.orionAh += finiteOr0(s.orionOutA) * hours;
  }
  d.samples++;
}

inline float dayUsedAh(const DayStats &d) {
  float used = d.solarAh + d.orionAh - d.netAh;
  return used > 0.0f ? used : 0.0f;
}

inline bool dayComplete(const DayStats &d) {
  return d.day != NO_DAY && d.samples >= DAY_COMPLETE_MIN_SAMPLES;
}

struct DayHistory {
  DayStats current;
  DayStats days[DAY_HISTORY_DAYS];  // days[0] = most recent closed day
  uint8_t count = 0;
  int32_t morningSentDay = NO_DAY;
  int32_t eveningSentDay = NO_DAY;
};

inline void dayHistoryRoll(DayHistory &h, int32_t today) {
  if (h.current.day == today) {
    return;
  }
  if (h.current.day != NO_DAY) {
    for (int i = DAY_HISTORY_DAYS - 1; i > 0; --i) {
      h.days[i] = h.days[i - 1];
    }
    h.days[0] = h.current;
    if (h.count < DAY_HISTORY_DAYS) {
      h.count++;
    }
  }
  h.current = DayStats();
  h.current.day = today;
}

inline const DayStats *dayHistoryFind(const DayHistory &h, int32_t day) {
  for (uint8_t i = 0; i < h.count; i++) {
    if (h.days[i].day == day) {
      return &h.days[i];
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------- capacity

constexpr float CAPACITY_ALPHA = 0.1f;
constexpr float CAPACITY_MAX_SOC = 90.0f;
constexpr float CAPACITY_PERSIST_CHANGE = 0.02f;

struct CapacityLearner {
  float capacityAh = NAN;
};

// consumedAh is positive. Returns true when the estimate was updated.
inline bool capacityUpdate(CapacityLearner &c, float soc, float consumedAh) {
  if (!socKnown(soc) || soc > CAPACITY_MAX_SOC) {
    return false;
  }
  if (!std::isfinite(consumedAh) || consumedAh <= 0.0f) {
    return false;
  }
  float raw = consumedAh / (1.0f - soc / 100.0f);
  c.capacityAh = std::isnan(c.capacityAh) ? raw : c.capacityAh + CAPACITY_ALPHA * (raw - c.capacityAh);
  return true;
}

inline bool capacityShouldPersist(float learned, float stored) {
  if (std::isnan(learned)) {
    return false;
  }
  if (std::isnan(stored) || stored <= 0.0f) {
    return true;
  }
  return std::fabs(learned - stored) > CAPACITY_PERSIST_CHANGE * stored;
}

inline float remainingAh(const CapacityLearner &c, float soc) {
  if (std::isnan(c.capacityAh) || !socKnown(soc)) {
    return NAN;
  }
  return soc / 100.0f * c.capacityAh;
}

// ---------------------------------------------------------------- days remaining

struct DaysEstimate {
  bool valid = false;
  uint8_t daysUsed = 0;
  bool showSolar = false;    // two figures: with solar and with no sun
  bool solarCovers = false;  // avgUsed - avgSolar <= 0
  float withSolar = NAN;
  float noSun = NAN;
};

inline DaysEstimate daysRemaining(const DayHistory &h, uint8_t daysAvg, float remaining, bool hasSolarKey) {
  DaysEstimate e;
  if (std::isnan(remaining)) {
    return e;
  }
  float used = 0.0f;
  float solar = 0.0f;
  uint8_t n = 0;
  for (uint8_t i = 0; i < h.count && n < daysAvg; i++) {
    if (dayComplete(h.days[i])) {
      used += dayUsedAh(h.days[i]);
      solar += h.days[i].solarAh;
      n++;
    }
  }
  if (n == 0) {
    return e;
  }
  float avgUsed = used / n;
  float avgSolar = solar / n;
  e.valid = true;
  e.daysUsed = n;
  e.noSun = avgUsed > 0.0f ? remaining / avgUsed : INFINITY;
  e.showSolar = hasSolarKey && avgSolar > 0.0f;
  if (e.showSolar) {
    float net = avgUsed - avgSolar;
    if (net <= 0.0f) {
      e.solarCovers = true;
    } else {
      e.withSolar = remaining / net;
    }
  }
  return e;
}

// ---------------------------------------------------------------- report timing

inline bool morningDue(int32_t morningSentDay, int32_t localDay, uint16_t localMinute, uint8_t reportHour) {
  return localDay != morningSentDay && localMinute >= reportHour * 60U;
}

// SunsetDetector: the evening report is due once per day, after PV exceeded
// 5 W that day, at or after eveningHour:00 once PV has been <= 5 W for 30
// minutes, and at 23:00 at the latest.
inline bool sunsetDue(int32_t eveningSentDay, const DayStats &today, int32_t localDay, uint16_t localMinute,
                      uint8_t eveningHour) {
  if (localDay == eveningSentDay || today.day != localDay || today.pvLastMinute < 0) {
    return false;
  }
  if (localMinute < eveningHour * 60U) {
    return false;
  }
  if (localMinute >= EVENING_LATEST_MINUTE) {
    return true;
  }
  return localMinute - today.pvLastMinute >= SUNSET_QUIET_MINUTES;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_days`
Expected: `18 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add include/chargescreen_stats.h test/test_days/test_main.cpp
git commit -m "Add day statistics, capacity learning, days remaining and report timing

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Messages and report texts

**Files:**
- Modify: `include/chargescreen_stats.h`
- Create: `test/test_reports/test_main.cpp`

**Interfaces:**
- Consumes: `clockValid`, `formatHhMm`, `localMinuteOfDay`, `DELAYED_AFTER_S` (Task 1); `socKnown`, `DayStats`, `dayUsedAh`, `DaysEstimate` (Task 2).
- Produces (namespace `cs`):
  - `struct TextWriter { char *buf; size_t cap; size_t len; bool overflow; TextWriter(char *, size_t); void putChar(char); void putStr(const char *); void putf(const char *fmt, ...); void putEscaped(const char *); size_t result() const; }` (`result()` is 0 on overflow)
  - `enum class MsgKind : uint8_t { Alert = 0, Clear = 1, Morning = 2, Evening = 3, Batch = 4, Test = 5 }`, `TEST_BODY` (`"Online sending works."`)
  - `const char *kindName(MsgKind)`, `const char *kindTitle(MsgKind)`, `const char *kindTag(MsgKind)`, `uint8_t kindPriority(MsgKind)`
  - `MSG_TITLE_LEN` (48), `MSG_BODY_LEN` (320); `struct Message { uint8_t kind; uint8_t priority; uint16_t boot; uint32_t eventUnix; uint32_t eventMs; char title[48]; char body[320]; }` (380 bytes, stored on flash)
  - `void messageInit(Message &, MsgKind, uint16_t boot, uint32_t unixOr0, uint32_t nowMs)`
  - `size_t buildAlertBody(char *out, size_t cap, float avgA, uint16_t windowMin, float limitA, float soc, float remaining)`
  - `size_t buildClearBody(char *out, size_t cap, float avgA, float soc)`
  - `size_t buildMorningBody(char *out, size_t cap, float soc, float remaining, const DaysEstimate &, const DayStats *yesterday, bool hasSolarKey, bool hasOrionKey)`
  - `size_t buildEveningBody(char *out, size_t cap, const DayStats &)`
  - `size_t formatDelayedSuffix(char *out, size_t cap, uint32_t nowUnix, uint32_t eventUnix, bool offsetKnown, int32_t offsetS, bool sameBoot, uint32_t nowMs, uint32_t eventMs)`, `bool insertAfterFirstLine(char *body, size_t cap, const char *suffix)`
  - `enum class OnlineState : uint8_t { Off = 0, Waiting = 1, NoWifi = 2, Sent = 3, Error = 4 }`, `void onlineStatusText(OnlineState, int16_t localMinute, char *out, size_t cap)`

- [ ] **Step 1: Write the failing tests**

Create `test/test_reports/test_main.cpp`:

```cpp
#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static cs::DaysEstimate estimate(uint8_t daysUsed, bool showSolar, bool solarCovers, float withSolar, float noSun) {
  cs::DaysEstimate e;
  e.valid = true;
  e.daysUsed = daysUsed;
  e.showSolar = showSolar;
  e.solarCovers = solarCovers;
  e.withSolar = withSolar;
  e.noSun = noSun;
  return e;
}

static cs::DayStats yesterdayStats(float usedAh, float solarAh, float orionAh) {
  cs::DayStats d;
  d.day = 9;
  d.samples = 8640;
  d.solarAh = solarAh;
  d.orionAh = orionAh;
  d.netAh = solarAh + orionAh - usedAh;
  return d;
}

static void test_kinds() {
  TEST_ASSERT_EQUAL_STRING("alert", cs::kindName(cs::MsgKind::Alert));
  TEST_ASSERT_EQUAL_STRING("test", cs::kindName(cs::MsgKind::Test));
  TEST_ASSERT_EQUAL_UINT8(4, cs::kindPriority(cs::MsgKind::Alert));
  TEST_ASSERT_EQUAL_UINT8(3, cs::kindPriority(cs::MsgKind::Clear));
  TEST_ASSERT_EQUAL_UINT8(1, cs::kindPriority(cs::MsgKind::Batch));
  TEST_ASSERT_EQUAL_UINT8(3, cs::kindPriority(cs::MsgKind::Test));
  TEST_ASSERT_EQUAL_STRING("warning", cs::kindTag(cs::MsgKind::Alert));
  TEST_ASSERT_EQUAL_STRING("white_check_mark", cs::kindTag(cs::MsgKind::Clear));
  TEST_ASSERT_EQUAL_STRING("sunny", cs::kindTag(cs::MsgKind::Morning));
  TEST_ASSERT_EQUAL_STRING("city_sunset", cs::kindTag(cs::MsgKind::Evening));
  TEST_ASSERT_EQUAL_STRING("", cs::kindTag(cs::MsgKind::Batch));
  TEST_ASSERT_EQUAL_STRING("Solar today", cs::kindTitle(cs::MsgKind::Evening));
  cs::Message m;
  cs::messageInit(m, cs::MsgKind::Test, 7, 1793227997UL, 1234);
  TEST_ASSERT_EQUAL_STRING("ChargeScreen test", m.title);
  TEST_ASSERT_EQUAL_UINT8(3, m.priority);
  TEST_ASSERT_EQUAL_UINT16(7, m.boot);
  TEST_ASSERT_EQUAL_UINT32(1793227997UL, m.eventUnix);
  cs::messageInit(m, cs::MsgKind::Alert, 7, 42, 1234);
  TEST_ASSERT_EQUAL_UINT32(0, m.eventUnix);
}

static void test_alert_and_clear_text() {
  char body[cs::MSG_BODY_LEN];
  TEST_ASSERT_TRUE(cs::buildAlertBody(body, sizeof body, 6.8f, 45, 4.0f, 71.0f, 178.0f) > 0);
  TEST_ASSERT_EQUAL_STRING("Averaging 6.8 A over the last 45 min (limit 4.0 A). Battery 71% (178 Ah left).", body);
  cs::buildAlertBody(body, sizeof body, 6.8f, 45, 4.0f, 71.0f, NAN);
  TEST_ASSERT_EQUAL_STRING("Averaging 6.8 A over the last 45 min (limit 4.0 A). Battery 71%.", body);
  cs::buildAlertBody(body, sizeof body, 6.8f, 45, 4.0f, 102.3f, NAN);
  TEST_ASSERT_EQUAL_STRING("Averaging 6.8 A over the last 45 min (limit 4.0 A).", body);
  TEST_ASSERT_TRUE(cs::buildClearBody(body, sizeof body, 1.4f, 69.0f) > 0);
  TEST_ASSERT_EQUAL_STRING("1.4 A average. Battery 69%.", body);
}

static void test_morning_full_example() {
  char body[cs::MSG_BODY_LEN];
  cs::DayStats y = yesterdayStats(52.0f, 31.0f, 40.0f);
  TEST_ASSERT_TRUE(cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(3, true, false, 4.2f, 2.1f), &y,
                                        true, true) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\n"
      "At the last 3 days' rate: about 4.2 days with solar, about 2.1 days with no sun.\n"
      "Yesterday: used 52 Ah, solar +31 Ah, Orion +40 Ah.",
      body);
}

static void test_morning_omissions() {
  char body[cs::MSG_BODY_LEN];
  cs::DayStats y = yesterdayStats(52.0f, 0.0f, 40.0f);
  // No solar key: single figure, no solar part.
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(3, false, false, NAN, 2.1f), &y, false, true);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\nAt the last 3 days' rate: about 2.1 days.\nYesterday: used 52 Ah, Orion +40 Ah.", body);
  // Orion key missing and solar 0.0 Ah: both parts left out.
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(3, false, false, NAN, 2.1f), &y, true, false);
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left)\nAt the last 3 days' rate: about 2.1 days.\nYesterday: used 52 Ah.", body);
  // Capacity not learned: no "Ah left" and no estimate line.
  cs::DaysEstimate none;
  cs::buildMorningBody(body, sizeof body, 78.0f, NAN, none, &y, true, true);
  TEST_ASSERT_EQUAL_STRING("78%\nYesterday: used 52 Ah, Orion +40 Ah.", body);
  // No complete yesterday and no complete day at all.
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, none, nullptr, true, true);
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left)", body);
}

static void test_morning_day_counts_and_limits() {
  char body[cs::MSG_BODY_LEN];
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(2, true, true, NAN, 2.1f), nullptr, true, true);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\nAt the last 2 days' rate: solar covers your usage, about 2.1 days with no sun.", body);
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(1, true, false, 45.0f, INFINITY), nullptr, true,
                       true);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\nAt the last day's rate: more than 30 days with solar, more than 30 days with no sun.", body);
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(1, false, false, NAN, 30.0f), nullptr, true, true);
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left)\nAt the last day's rate: about 30.0 days.", body);
}

static void test_evening_example() {
  char body[cs::MSG_BODY_LEN];
  cs::DayStats d;
  d.day = 10;
  d.solarAh = 31.0f;
  d.solarWh = 412.0f;
  d.pvPeakW = 286.0f;
  d.pvPeakMinute = 13 * 60 + 12;
  d.pvFirstMinute = 7 * 60 + 48;
  d.pvLastMinute = 19 * 60 + 26;
  TEST_ASSERT_TRUE(cs::buildEveningBody(body, sizeof body, d) > 0);
  TEST_ASSERT_EQUAL_STRING("+31 Ah (412 Wh), peak 286 W at 13:12\nProducing from 07:48 to 19:26 (11 h 38 m)", body);
}

static void test_delayed_suffix_forms() {
  char s[48];
  const uint32_t event = 1793227997UL;  // 22:53:17 UTC, 23:53 Brussels
  TEST_ASSERT_EQUAL_UINT32(0, cs::formatDelayedSuffix(s, sizeof s, event + 600, event, true, 3600, true, 0, 0));
  TEST_ASSERT_EQUAL_STRING("", s);
  cs::formatDelayedSuffix(s, sizeof s, event + 601, event, true, 3600, true, 0, 0);
  TEST_ASSERT_EQUAL_STRING(" (delayed, happened 23:53)", s);
  // Clock known but no local offset yet: minutes ago.
  cs::formatDelayedSuffix(s, sizeof s, event + 1500, event, false, 0, true, 0, 0);
  TEST_ASSERT_EQUAL_STRING(" (delayed, happened 25 min ago)", s);
  // Event before the first time sync: compare millis() in the same boot.
  cs::formatDelayedSuffix(s, sizeof s, event, 0, true, 3600, true, 3000000UL, 1200000UL);
  TEST_ASSERT_EQUAL_STRING(" (delayed, happened 30 min ago)", s);
  cs::formatDelayedSuffix(s, sizeof s, 0, 0, false, 0, true, 300000UL, 0);
  TEST_ASSERT_EQUAL_STRING("", s);
  // Event time unknown and from an earlier boot.
  cs::formatDelayedSuffix(s, sizeof s, event, 0, true, 3600, false, 0, 0);
  TEST_ASSERT_EQUAL_STRING(" (delayed)", s);
}

static void test_insert_after_first_line() {
  char body[64] = "78% (196 Ah left)\nYesterday: used 52 Ah.";
  TEST_ASSERT_TRUE(cs::insertAfterFirstLine(body, sizeof body, " (delayed)"));
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left) (delayed)\nYesterday: used 52 Ah.", body);
  char single[40] = "1.4 A average.";
  TEST_ASSERT_TRUE(cs::insertAfterFirstLine(single, sizeof single, " (delayed)"));
  TEST_ASSERT_EQUAL_STRING("1.4 A average. (delayed)", single);
  char tiny[16] = "1.4 A average.";
  TEST_ASSERT_FALSE(cs::insertAfterFirstLine(tiny, sizeof tiny, " (delayed)"));
  TEST_ASSERT_EQUAL_STRING("1.4 A average.", tiny);
}

static void test_online_status_text() {
  char t[16];
  cs::onlineStatusText(cs::OnlineState::Off, -1, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Off", t);
  cs::onlineStatusText(cs::OnlineState::NoWifi, 600, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("No WiFi", t);
  cs::onlineStatusText(cs::OnlineState::Sent, 7 * 60 + 5, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Sent 07:05", t);
  cs::onlineStatusText(cs::OnlineState::Error, -1, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Error --:--", t);
  cs::onlineStatusText(cs::OnlineState::Waiting, -1, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Waiting", t);
}

static void test_writer_escapes_and_overflows() {
  char buf[32];
  cs::TextWriter w(buf, sizeof buf);
  w.putEscaped("a\"b\\c\nd\te");
  TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\nd\\u0009e", buf);
  char small[8];
  cs::TextWriter s(small, sizeof small);
  s.putStr("1234");
  s.putf("%s", "56789");
  TEST_ASSERT_TRUE(s.overflow);
  TEST_ASSERT_EQUAL_UINT32(0, s.result());
  TEST_ASSERT_EQUAL_STRING("1234", small);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_kinds);
  RUN_TEST(test_alert_and_clear_text);
  RUN_TEST(test_morning_full_example);
  RUN_TEST(test_morning_omissions);
  RUN_TEST(test_morning_day_counts_and_limits);
  RUN_TEST(test_evening_example);
  RUN_TEST(test_delayed_suffix_forms);
  RUN_TEST(test_insert_after_first_line);
  RUN_TEST(test_online_status_text);
  RUN_TEST(test_writer_escapes_and_overflows);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_reports`
Expected: FAIL, compile errors such as `'MsgKind' is not a member of 'cs'`.

- [ ] **Step 3: Implement in the header**

In `include/chargescreen_stats.h`, directly before the final `}  // namespace cs`, add:

```cpp
// ---------------------------------------------------------------- text output

// Appends into a caller-provided buffer; on overflow the text is cut at the
// last complete piece and `overflow` is set.
struct TextWriter {
  char *buf;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  TextWriter(char *buffer, size_t capacity) : buf(buffer), cap(capacity) {
    if (cap) {
      buf[0] = '\0';
    }
  }

  void putChar(char c) {
    if (overflow || len + 1 >= cap) {
      overflow = true;
      return;
    }
    buf[len++] = c;
    buf[len] = '\0';
  }

  void putStr(const char *s) {
    while (*s && !overflow) {
      putChar(*s++);
    }
  }

  void putf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (overflow || cap == 0) {
      overflow = true;
      return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + len, cap - len, fmt, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= cap - len) {
      overflow = true;
      buf[len] = '\0';
      return;
    }
    len += static_cast<size_t>(n);
  }

  // JSON string content (without the surrounding quotes).
  void putEscaped(const char *s) {
    for (; *s && !overflow; ++s) {
      unsigned char c = static_cast<unsigned char>(*s);
      if (c == '"' || c == '\\') {
        putChar('\\');
        putChar(static_cast<char>(c));
      } else if (c == '\n') {
        putStr("\\n");
      } else if (c < 0x20) {
        putf("\\u%04x", c);
      } else {
        putChar(static_cast<char>(c));
      }
    }
  }

  size_t result() const {
    return overflow ? 0 : len;
  }
};

// ---------------------------------------------------------------- messages

enum class MsgKind : uint8_t { Alert = 0, Clear = 1, Morning = 2, Evening = 3, Batch = 4, Test = 5 };

constexpr const char *TEST_BODY = "Online sending works.";

inline const char *kindName(MsgKind k) {
  switch (k) {
    case MsgKind::Alert:
      return "alert";
    case MsgKind::Clear:
      return "clear";
    case MsgKind::Morning:
      return "morning";
    case MsgKind::Evening:
      return "evening";
    case MsgKind::Batch:
      return "batch";
    default:
      return "test";
  }
}

inline const char *kindTitle(MsgKind k) {
  switch (k) {
    case MsgKind::Alert:
      return "High battery drain";
    case MsgKind::Clear:
      return "Drain back to normal";
    case MsgKind::Morning:
      return "Morning battery report";
    case MsgKind::Evening:
      return "Solar today";
    case MsgKind::Batch:
      return "ChargeScreen data";
    default:
      return "ChargeScreen test";
  }
}

// ntfy tag; empty = no tag.
inline const char *kindTag(MsgKind k) {
  switch (k) {
    case MsgKind::Alert:
      return "warning";
    case MsgKind::Clear:
      return "white_check_mark";
    case MsgKind::Morning:
      return "sunny";
    case MsgKind::Evening:
      return "city_sunset";
    default:
      return "";
  }
}

// ntfy levels: 1 = min (silent), 3 = default, 4 = high.
inline uint8_t kindPriority(MsgKind k) {
  return k == MsgKind::Alert ? 4 : (k == MsgKind::Batch ? 1 : 3);
}

constexpr size_t MSG_TITLE_LEN = 48;
constexpr size_t MSG_BODY_LEN = 320;

// Stored as-is in /outbox.bin.
struct Message {
  uint8_t kind = 0;
  uint8_t priority = 3;
  uint16_t boot = 0;        // boot id when queued; eventMs is only meaningful in that boot
  uint32_t eventUnix = 0;   // 0 = clock unknown when it happened
  uint32_t eventMs = 0;     // millis() when it happened
  char title[MSG_TITLE_LEN] = {};
  char body[MSG_BODY_LEN] = {};
};
static_assert(sizeof(Message) == 380, "Message layout is stored on flash");

inline void messageInit(Message &m, MsgKind kind, uint16_t boot, uint32_t unixOr0, uint32_t nowMs) {
  m = Message();
  m.kind = static_cast<uint8_t>(kind);
  m.priority = kindPriority(kind);
  m.boot = boot;
  m.eventUnix = clockValid(unixOr0) ? unixOr0 : 0;
  m.eventMs = nowMs;
  snprintf(m.title, sizeof(m.title), "%s", kindTitle(kind));
}

// ---------------------------------------------------------------- report text

inline void putDays(TextWriter &w, float days) {
  if (!(days <= 30.0f)) {
    w.putStr("more than 30 days");
  } else {
    w.putf("about %.1f days", days);
  }
}

inline void putBatterySentence(TextWriter &w, float soc, float remaining) {
  if (!socKnown(soc)) {
    return;
  }
  w.putf(" Battery %.0f%%", soc);
  if (std::isfinite(remaining)) {
    w.putf(" (%.0f Ah left)", remaining);
  }
  w.putChar('.');
}

inline size_t buildAlertBody(char *out, size_t cap, float avgA, uint16_t windowMin, float limitA, float soc,
                             float remaining) {
  TextWriter w(out, cap);
  w.putf("Averaging %.1f A over the last %u min (limit %.1f A).", avgA, static_cast<unsigned>(windowMin), limitA);
  putBatterySentence(w, soc, remaining);
  return w.result();
}

inline size_t buildClearBody(char *out, size_t cap, float avgA, float soc) {
  TextWriter w(out, cap);
  w.putf("%.1f A average.", avgA);
  putBatterySentence(w, soc, NAN);
  return w.result();
}

inline bool shownAh(float v) {
  return std::isfinite(v) && std::fabs(v) >= 0.05f;
}

// yesterday may be null (no complete record of yesterday).
inline size_t buildMorningBody(char *out, size_t cap, float soc, float remaining, const DaysEstimate &est,
                               const DayStats *yesterday, bool hasSolarKey, bool hasOrionKey) {
  TextWriter w(out, cap);
  w.putf("%.0f%%", soc);
  if (std::isfinite(remaining)) {
    w.putf(" (%.0f Ah left)", remaining);
  }
  if (est.valid) {
    w.putStr("\nAt the last ");
    if (est.daysUsed == 1) {
      w.putStr("day's");
    } else {
      w.putf("%u days'", static_cast<unsigned>(est.daysUsed));
    }
    w.putStr(" rate: ");
    if (est.showSolar) {
      if (est.solarCovers) {
        w.putStr("solar covers your usage");
      } else {
        putDays(w, est.withSolar);
        w.putStr(" with solar");
      }
      w.putStr(", ");
      putDays(w, est.noSun);
      w.putStr(" with no sun.");
    } else {
      putDays(w, est.noSun);
      w.putChar('.');
    }
  }
  if (yesterday) {
    w.putf("\nYesterday: used %.0f Ah", dayUsedAh(*yesterday));
    if (hasSolarKey && shownAh(yesterday->solarAh)) {
      w.putf(", solar +%.0f Ah", yesterday->solarAh);
    }
    if (hasOrionKey && shownAh(yesterday->orionAh)) {
      w.putf(", Orion +%.0f Ah", yesterday->orionAh);
    }
    w.putChar('.');
  }
  return w.result();
}

inline size_t buildEveningBody(char *out, size_t cap, const DayStats &d) {
  TextWriter w(out, cap);
  char peak[6];
  char first[6];
  char last[6];
  formatHhMm(d.pvPeakMinute < 0 ? 0 : static_cast<uint16_t>(d.pvPeakMinute), peak);
  formatHhMm(d.pvFirstMinute < 0 ? 0 : static_cast<uint16_t>(d.pvFirstMinute), first);
  formatHhMm(d.pvLastMinute < 0 ? 0 : static_cast<uint16_t>(d.pvLastMinute), last);
  int duration = d.pvLastMinute - d.pvFirstMinute;
  if (duration < 0) {
    duration = 0;
  }
  w.putf("+%.0f Ah (%.0f Wh), peak %.0f W at %s", d.solarAh, d.solarWh, d.pvPeakW, peak);
  w.putf("\nProducing from %s to %s (%d h %d m)", first, last, duration / 60, duration % 60);
  return w.result();
}

// Empty when the message is not late. Uses HH:MM when both clocks and the
// local offset are known, "N min ago" when only millis() is comparable
// (same boot), and a bare "(delayed)" when neither is.
inline size_t formatDelayedSuffix(char *out, size_t cap, uint32_t nowUnix, uint32_t eventUnix, bool offsetKnown,
                                  int32_t offsetS, bool sameBoot, uint32_t nowMs, uint32_t eventMs) {
  TextWriter w(out, cap);
  bool haveClock = clockValid(nowUnix) && clockValid(eventUnix);
  uint32_t elapsed;
  if (haveClock) {
    elapsed = nowUnix >= eventUnix ? nowUnix - eventUnix : 0;
  } else if (sameBoot) {
    elapsed = static_cast<uint32_t>(nowMs - eventMs) / 1000U;
  } else {
    w.putStr(" (delayed)");
    return w.result();
  }
  if (elapsed <= DELAYED_AFTER_S) {
    return 0;
  }
  if (haveClock && offsetKnown) {
    char hm[6];
    formatHhMm(localMinuteOfDay(eventUnix, offsetS), hm);
    w.putf(" (delayed, happened %s)", hm);
  } else {
    w.putf(" (delayed, happened %lu min ago)", static_cast<unsigned long>(elapsed / 60U));
  }
  return w.result();
}

// Appends suffix to the first line of body. False if it does not fit.
inline bool insertAfterFirstLine(char *body, size_t cap, const char *suffix) {
  size_t bodyLen = strlen(body);
  size_t suffixLen = strlen(suffix);
  if (suffixLen == 0) {
    return true;
  }
  if (bodyLen + suffixLen + 1 > cap) {
    return false;
  }
  const char *newline = strchr(body, '\n');
  size_t at = newline ? static_cast<size_t>(newline - body) : bodyLen;
  memmove(body + at + suffixLen, body + at, bodyLen - at + 1);
  memcpy(body + at, suffix, suffixLen);
  return true;
}

// ---------------------------------------------------------------- info screen

enum class OnlineState : uint8_t { Off = 0, Waiting = 1, NoWifi = 2, Sent = 3, Error = 4 };

// localMinute < 0 when the local time of the last attempt is unknown.
inline void onlineStatusText(OnlineState state, int16_t localMinute, char *out, size_t cap) {
  TextWriter w(out, cap);
  char hm[6] = "--:--";
  if (localMinute >= 0) {
    formatHhMm(static_cast<uint16_t>(localMinute), hm);
  }
  switch (state) {
    case OnlineState::Off:
      w.putStr("Off");
      break;
    case OnlineState::Waiting:
      w.putStr("Waiting");
      break;
    case OnlineState::NoWifi:
      w.putStr("No WiFi");
      break;
    case OnlineState::Sent:
      w.putf("Sent %s", hm);
      break;
    case OnlineState::Error:
      w.putf("Error %s", hm);
      break;
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_reports`
Expected: `10 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add include/chargescreen_stats.h test/test_reports/test_main.cpp
git commit -m "Add online message kinds, report texts and delayed suffix

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Records, batch JSON, sinks and envelopes

**Files:**
- Modify: `include/chargescreen_stats.h`
- Create: `test/test_payloads/test_main.cpp`

**Interfaces:**
- Consumes: `clockValid`, `RECORDS_PER_BATCH`, `NTFY_MESSAGE_LIMIT` (Task 1); `Sample`, `socKnown` (Task 2); `TextWriter`, `MsgKind`, `kindName` (Task 3).
- Produces (namespace `cs`):
  - `BATCH_JSON_MAX` (2048), `ENVELOPE_MAX` (4608)
  - `struct Record { uint32_t t; uint32_t startMs; uint16_t boot; uint16_t reserved; float soc, used, i, imin, imax, pv, pvmax, orion; }` (44 bytes, stored on flash; NAN = no valid data)
  - `struct RecordBuilder`, `void recordStart(RecordBuilder &, uint32_t unixOr0, uint32_t nowMs, uint16_t boot)`, `void recordAdd(RecordBuilder &, const Sample &)`, `Record recordClose(const RecordBuilder &)`
  - `size_t buildBatchJson(char *out, size_t cap, const Record *recs, uint8_t count)`
  - `enum class SinkType : uint8_t { Ntfy, Generic }`, `bool urlHost(const char *url, char *host, size_t cap)`, `SinkType sinkType(const char *url)`, `bool ntfyTarget(const char *url, char *base, size_t baseCap, char *topic, size_t topicCap)`
  - `size_t buildNtfyEnvelope(char *out, size_t cap, const char *topic, const char *title, const char *message, uint8_t priority, const char *tag)`
  - `size_t buildGenericEnvelope(char *out, size_t cap, MsgKind kind, const char *title, const char *message, uint8_t priority, uint32_t unixTime, const char *dataJson)`

- [ ] **Step 1: Write the failing tests**

Create `test/test_payloads/test_main.cpp`:

```cpp
#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static cs::Record specRecord() {
  cs::Record r;
  r.t = 1761638400UL;
  r.soc = 78.1f;
  r.used = 44.2f;
  r.i = -2.1f;
  r.imin = -6.0f;
  r.imax = 3.2f;
  r.pv = 120.0f;
  r.pvmax = 286.0f;
  r.orion = 0.0f;
  return r;
}

static cs::Record worstCaseRecord() {
  cs::Record r;
  r.t = 4294967295UL;
  r.soc = 100.0f;
  r.used = -9999.9f;
  r.i = -9999.9f;
  r.imin = -9999.9f;
  r.imax = -9999.9f;
  r.pv = 99999.0f;
  r.pvmax = 99999.0f;
  r.orion = -9999.9f;
  return r;
}

static void test_record_builder_averages_and_extremes() {
  cs::RecordBuilder b;
  cs::recordStart(b, 1793174400UL, 5000, 3);
  cs::Sample s;
  s.shuntValid = true;
  s.soc = 78.0f;
  s.consumedAh = 44.0f;
  s.solarValid = true;
  s.orionValid = true;
  const float currents[] = {-6.0f, 3.2f, -3.5f};
  const float pvs[] = {100.0f, 286.0f, 0.0f};
  for (int k = 0; k < 3; k++) {
    s.currentA = currents[k];
    s.pvW = pvs[k];
    s.orionOutA = 1.0f;
    cs::recordAdd(b, s);
  }
  s.soc = 102.3f;  // not available: keeps the last known SOC
  s.consumedAh = 44.2f;
  s.currentA = -2.1f;
  s.solarValid = false;
  s.orionValid = false;
  cs::recordAdd(b, s);
  cs::Record r = cs::recordClose(b);
  TEST_ASSERT_EQUAL_UINT32(1793174400UL, r.t);
  TEST_ASSERT_EQUAL_UINT32(5000, r.startMs);
  TEST_ASSERT_EQUAL_UINT16(3, r.boot);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 78.0f, r.soc);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 44.2f, r.used);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -2.1f, r.i);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -6.0f, r.imin);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, r.imax);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 128.667f, r.pv);
  TEST_ASSERT_EQUAL_FLOAT(286.0f, r.pvmax);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, r.orion);
}

static void test_record_without_clock_or_devices() {
  cs::RecordBuilder b;
  cs::recordStart(b, 1000, 77, 1);
  cs::Record r = cs::recordClose(b);
  TEST_ASSERT_EQUAL_UINT32(0, r.t);
  TEST_ASSERT_TRUE(std::isnan(r.i));
  TEST_ASSERT_TRUE(std::isnan(r.soc));
  TEST_ASSERT_TRUE(std::isnan(r.pv));
  TEST_ASSERT_TRUE(std::isnan(r.pvmax));
  TEST_ASSERT_TRUE(std::isnan(r.orion));
}

static void test_batch_json_matches_spec_example() {
  char out[cs::BATCH_JSON_MAX];
  cs::Record r = specRecord();
  TEST_ASSERT_TRUE(cs::buildBatchJson(out, sizeof out, &r, 1) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"recs\":[{\"t\":1761638400,\"soc\":78.1,\"used\":44.2,\"i\":-2.1,\"imin\":-6.0,\"imax\":3.2,"
      "\"pv\":120,\"pvmax\":286,\"or\":0.0}]}",
      out);
}

static void test_batch_json_omits_invalid_fields_and_rounds() {
  char out[cs::BATCH_JSON_MAX];
  cs::Record recs[2];
  recs[0].t = 100;
  recs[0].soc = 78.14f;
  recs[0].i = -0.04f;
  recs[0].pv = 119.6f;
  recs[1].t = 400;
  recs[1].orion = 12.25f;
  cs::buildBatchJson(out, sizeof out, recs, 2);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"recs\":[{\"t\":100,\"soc\":78.1,\"i\":0.0,\"pv\":120},{\"t\":400,\"or\":12.3}]}",
                           out);
  cs::buildBatchJson(out, sizeof out, recs, 0);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"recs\":[]}", out);
}

static void test_twelve_records_fit_ntfy_limit() {
  cs::Record recs[cs::RECORDS_PER_BATCH];
  for (auto &r : recs) {
    r = worstCaseRecord();
  }
  char json[cs::BATCH_JSON_MAX];
  size_t len = cs::buildBatchJson(json, sizeof json, recs, cs::RECORDS_PER_BATCH);
  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE(len < cs::NTFY_MESSAGE_LIMIT);
  static char envelope[cs::ENVELOPE_MAX];
  TEST_ASSERT_TRUE(cs::buildNtfyEnvelope(envelope, sizeof envelope, "ghibli_harmony", "ChargeScreen data", json, 1,
                                         "") > 0);
  TEST_ASSERT_TRUE(cs::buildGenericEnvelope(envelope, sizeof envelope, cs::MsgKind::Batch, "ChargeScreen data",
                                            "12 records", 1, 1761638400UL, json) > 0);
}

static void test_sink_detection() {
  TEST_ASSERT_EQUAL(cs::SinkType::Ntfy, cs::sinkType("https://ntfy.sh/ghibli_harmony"));
  TEST_ASSERT_EQUAL(cs::SinkType::Ntfy, cs::sinkType("https://NTFY.sh/ghibli_harmony"));
  TEST_ASSERT_EQUAL(cs::SinkType::Ntfy, cs::sinkType("https://eu.ntfy.sh/topic"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("https://example.com/cs/post.php"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("https://notntfy.sh/topic"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("https://ntfy.sh.example.com/topic"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("garbage"));
}

static void test_ntfy_topic_extraction() {
  char base[96];
  char topic[64];
  TEST_ASSERT_TRUE(cs::ntfyTarget("https://ntfy.sh/ghibli_harmony", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/", base);
  TEST_ASSERT_EQUAL_STRING("ghibli_harmony", topic);
  TEST_ASSERT_TRUE(cs::ntfyTarget("https://eu.ntfy.sh:443/my-topic/?x=1", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_EQUAL_STRING("https://eu.ntfy.sh:443/", base);
  TEST_ASSERT_EQUAL_STRING("my-topic", topic);
  TEST_ASSERT_FALSE(cs::ntfyTarget("https://ntfy.sh/", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_FALSE(cs::ntfyTarget("https://ntfy.sh", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_FALSE(cs::ntfyTarget("https://ntfy.sh/a/b", base, sizeof base, topic, sizeof topic));
}

static void test_ntfy_envelope() {
  char out[512];
  TEST_ASSERT_TRUE(cs::buildNtfyEnvelope(out, sizeof out, "ghibli_harmony", "High battery drain",
                                         "Averaging 6.8 A.\n\"quoted\"", 4, "warning") > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"topic\":\"ghibli_harmony\",\"title\":\"High battery drain\",\"message\":\"Averaging 6.8 A.\\n\\\"quoted\\\"\","
      "\"priority\":4,\"tags\":[\"warning\"]}",
      out);
  cs::buildNtfyEnvelope(out, sizeof out, "t", "ChargeScreen data", "{}", 1, "");
  TEST_ASSERT_EQUAL_STRING("{\"topic\":\"t\",\"title\":\"ChargeScreen data\",\"message\":\"{}\",\"priority\":1}", out);
}

static void test_generic_envelope() {
  char out[512];
  TEST_ASSERT_TRUE(cs::buildGenericEnvelope(out, sizeof out, cs::MsgKind::Clear, "Drain back to normal",
                                            "1.4 A average. Battery 69%.", 3, 1761638400UL, nullptr) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"kind\":\"clear\",\"title\":\"Drain back to normal\",\"message\":\"1.4 A average. Battery 69%.\","
      "\"priority\":3,\"time\":1761638400,\"data\":{}}",
      out);
  cs::buildGenericEnvelope(out, sizeof out, cs::MsgKind::Batch, "ChargeScreen data", "1 records", 1, 5,
                           "{\"v\":1,\"recs\":[]}");
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"kind\":\"batch\",\"title\":\"ChargeScreen data\",\"message\":\"1 records\",\"priority\":1,"
      "\"time\":5,\"data\":{\"v\":1,\"recs\":[]}}",
      out);
  char tiny[16];
  TEST_ASSERT_EQUAL_UINT32(0, cs::buildGenericEnvelope(tiny, sizeof tiny, cs::MsgKind::Test, "x", "y", 3, 0, nullptr));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_record_builder_averages_and_extremes);
  RUN_TEST(test_record_without_clock_or_devices);
  RUN_TEST(test_batch_json_matches_spec_example);
  RUN_TEST(test_batch_json_omits_invalid_fields_and_rounds);
  RUN_TEST(test_twelve_records_fit_ntfy_limit);
  RUN_TEST(test_sink_detection);
  RUN_TEST(test_ntfy_topic_extraction);
  RUN_TEST(test_ntfy_envelope);
  RUN_TEST(test_generic_envelope);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_payloads`
Expected: FAIL, compile errors such as `'Record' is not a member of 'cs'`.

- [ ] **Step 3: Implement in the header**

In `include/chargescreen_stats.h`, directly before the final `}  // namespace cs`, add:

```cpp
// ---------------------------------------------------------------- records

constexpr size_t BATCH_JSON_MAX = 2048;
constexpr size_t ENVELOPE_MAX = 4608;

// One 5-minute record, stored as-is in /tx_cache.bin. NAN = no valid data.
struct Record {
  uint32_t t = 0;        // start, unix seconds; 0 until the clock is known
  uint32_t startMs = 0;  // millis() at the start, meaningful while boot matches
  uint16_t boot = 0;
  uint16_t reserved = 0;
  float soc = NAN;
  float used = NAN;
  float i = NAN;
  float imin = NAN;
  float imax = NAN;
  float pv = NAN;
  float pvmax = NAN;
  float orion = NAN;
};
static_assert(sizeof(Record) == 44, "Record layout is stored on flash");

struct RecordBuilder {
  Record rec;
  uint16_t shuntCount = 0;
  uint16_t pvCount = 0;
  uint16_t orionCount = 0;
  float currentSum = 0.0f;
  float pvSum = 0.0f;
  float orionSum = 0.0f;
};

inline void recordStart(RecordBuilder &b, uint32_t unixOr0, uint32_t nowMs, uint16_t boot) {
  b = RecordBuilder();
  b.rec.t = clockValid(unixOr0) ? unixOr0 : 0;
  b.rec.startMs = nowMs;
  b.rec.boot = boot;
}

inline void recordAdd(RecordBuilder &b, const Sample &s) {
  if (s.shuntValid && !std::isnan(s.currentA)) {
    if (b.shuntCount == 0 || s.currentA < b.rec.imin) {
      b.rec.imin = s.currentA;
    }
    if (b.shuntCount == 0 || s.currentA > b.rec.imax) {
      b.rec.imax = s.currentA;
    }
    b.currentSum += s.currentA;
    b.shuntCount++;
    if (socKnown(s.soc)) {
      b.rec.soc = s.soc;
    }
    if (std::isfinite(s.consumedAh)) {
      b.rec.used = s.consumedAh;
    }
  }
  if (s.solarValid && std::isfinite(s.pvW)) {
    if (b.pvCount == 0 || s.pvW > b.rec.pvmax) {
      b.rec.pvmax = s.pvW;
    }
    b.pvSum += s.pvW;
    b.pvCount++;
  }
  if (s.orionValid && std::isfinite(s.orionOutA)) {
    b.orionSum += s.orionOutA;
    b.orionCount++;
  }
}

inline Record recordClose(const RecordBuilder &b) {
  Record r = b.rec;
  r.i = b.shuntCount ? b.currentSum / b.shuntCount : NAN;
  r.pv = b.pvCount ? b.pvSum / b.pvCount : NAN;
  r.orion = b.orionCount ? b.orionSum / b.orionCount : NAN;
  return r;
}

inline float roundTo(float v, float scale) {
  float r = std::round(v * scale) / scale;
  return r == 0.0f ? 0.0f : r;  // no "-0.0"
}

inline void putField1(TextWriter &w, const char *key, float v) {
  if (std::isfinite(v)) {
    w.putf(",\"%s\":%.1f", key, static_cast<double>(roundTo(v, 10.0f)));
  }
}

inline void putField0(TextWriter &w, const char *key, float v) {
  if (std::isfinite(v)) {
    w.putf(",\"%s\":%.0f", key, static_cast<double>(roundTo(v, 1.0f)));
  }
}

inline size_t buildBatchJson(char *out, size_t cap, const Record *recs, uint8_t count) {
  TextWriter w(out, cap);
  w.putStr("{\"v\":1,\"recs\":[");
  for (uint8_t k = 0; k < count; k++) {
    const Record &r = recs[k];
    w.putf("%s{\"t\":%lu", k ? "," : "", static_cast<unsigned long>(r.t));
    putField1(w, "soc", r.soc);
    putField1(w, "used", r.used);
    putField1(w, "i", r.i);
    putField1(w, "imin", r.imin);
    putField1(w, "imax", r.imax);
    putField0(w, "pv", r.pv);
    putField0(w, "pvmax", r.pvmax);
    putField1(w, "or", r.orion);
    w.putChar('}');
  }
  w.putStr("]}");
  return w.result();
}

// ---------------------------------------------------------------- sinks

enum class SinkType : uint8_t { Ntfy, Generic };

// Lower-case host of an http(s) URL, without user info or port.
inline bool urlHost(const char *url, char *host, size_t cap) {
  const char *start = strstr(url, "://");
  if (!start || cap == 0) {
    return false;
  }
  start += 3;
  const char *end = start;
  while (*end && *end != '/' && *end != '?' && *end != '#') {
    end++;
  }
  for (const char *p = start; p < end; ++p) {
    if (*p == '@') {
      start = p + 1;
    }
  }
  const char *colon = start;
  while (colon < end && *colon != ':') {
    colon++;
  }
  size_t len = static_cast<size_t>(colon - start);
  if (len == 0 || len >= cap) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    char c = start[i];
    host[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  host[len] = '\0';
  return true;
}

inline SinkType sinkType(const char *url) {
  char host[96];
  if (!urlHost(url, host, sizeof host)) {
    return SinkType::Generic;
  }
  size_t len = strlen(host);
  const char *suffix = ".ntfy.sh";
  size_t suffixLen = strlen(suffix);
  if (strcmp(host, "ntfy.sh") == 0 || (len > suffixLen && strcmp(host + len - suffixLen, suffix) == 0)) {
    return SinkType::Ntfy;
  }
  return SinkType::Generic;
}

// base = scheme://host[:port]/ and topic = path without the leading '/'.
inline bool ntfyTarget(const char *url, char *base, size_t baseCap, char *topic, size_t topicCap) {
  const char *scheme = strstr(url, "://");
  if (!scheme) {
    return false;
  }
  const char *authority = scheme + 3;
  const char *path = authority;
  while (*path && *path != '/' && *path != '?' && *path != '#') {
    path++;
  }
  size_t baseLen = static_cast<size_t>(path - url);
  if (path == authority || baseLen + 2 > baseCap) {
    return false;
  }
  memcpy(base, url, baseLen);
  base[baseLen] = '/';
  base[baseLen + 1] = '\0';
  if (*path != '/') {
    return false;
  }
  const char *t = path + 1;
  const char *tEnd = t;
  while (*tEnd && *tEnd != '?' && *tEnd != '#') {
    tEnd++;
  }
  while (tEnd > t && tEnd[-1] == '/') {
    tEnd--;
  }
  size_t topicLen = static_cast<size_t>(tEnd - t);
  if (topicLen == 0 || topicLen >= topicCap || memchr(t, '/', topicLen)) {
    return false;
  }
  memcpy(topic, t, topicLen);
  topic[topicLen] = '\0';
  return true;
}

// {"topic":..,"title":..,"message":..,"priority":N[,"tags":[tag]]}
inline size_t buildNtfyEnvelope(char *out, size_t cap, const char *topic, const char *title, const char *message,
                                uint8_t priority, const char *tag) {
  TextWriter w(out, cap);
  w.putStr("{\"topic\":\"");
  w.putEscaped(topic);
  w.putStr("\",\"title\":\"");
  w.putEscaped(title);
  w.putStr("\",\"message\":\"");
  w.putEscaped(message);
  w.putf("\",\"priority\":%u", static_cast<unsigned>(priority));
  if (tag && tag[0]) {
    w.putStr(",\"tags\":[\"");
    w.putEscaped(tag);
    w.putStr("\"]");
  }
  w.putChar('}');
  return w.result();
}

// {"v":1,"kind":..,"title":..,"message":..,"priority":N,"time":unix,"data":{..}}
// dataJson is a JSON object, or null for {}.
inline size_t buildGenericEnvelope(char *out, size_t cap, MsgKind kind, const char *title, const char *message,
                                   uint8_t priority, uint32_t unixTime, const char *dataJson) {
  TextWriter w(out, cap);
  w.putf("{\"v\":1,\"kind\":\"%s\",\"title\":\"", kindName(kind));
  w.putEscaped(title);
  w.putStr("\",\"message\":\"");
  w.putEscaped(message);
  w.putf("\",\"priority\":%u,\"time\":%lu,\"data\":", static_cast<unsigned>(priority),
         static_cast<unsigned long>(unixTime));
  w.putStr(dataJson ? dataJson : "{}");
  w.putChar('}');
  return w.result();
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_payloads`
Expected: `9 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 5: Commit**

```bash
git add include/chargescreen_stats.h test/test_payloads/test_main.cpp
git commit -m "Add 5-minute records, batch JSON and ntfy/generic envelopes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Cache indexes, network results, backoff, time-zone parsing

**Files:**
- Modify: `include/chargescreen_stats.h`
- Create: `test/test_netproto/test_main.cpp`

**Interfaces:**
- Consumes: `clockValid`, `OUTBOX_SIZE`, `RECORD_RING_SIZE` (Task 1); `TextWriter`, `MsgKind`, `Message` (Task 3); `Record` (Task 4).
- Produces (namespace `cs`):
  - `struct RingIndex { uint32_t nextSeq; uint32_t sentSeq; }`, `uint32_t ringOldestSeq(const RingIndex &, uint16_t cap)`, `uint32_t ringFirstUnsentSeq(const RingIndex &, uint16_t cap)`, `uint32_t ringUnsentCount(const RingIndex &, uint16_t cap)`, `uint16_t ringSlot(uint32_t seq, uint16_t cap)`, `uint32_t ringPush(RingIndex &)`, `void ringMarkSentThrough(RingIndex &, uint32_t endSeq)`
  - `enum class RecordAction : uint8_t { Include, Wait, Skip }`, `RecordAction recordSendAction(const Record &, uint16_t currentBoot)`, `bool recordFillTime(Record &, uint16_t currentBoot, uint32_t nowUnix, uint32_t nowMs)`, `bool messageFillTime(Message &, uint16_t currentBoot, uint32_t nowUnix, uint32_t nowMs)`
  - `struct QueueIndex { uint32_t headSeq; uint32_t nextSeq; uint32_t nextId; uint32_t ids[16]; uint8_t kinds[16]; }`, `uint8_t queueCount(const QueueIndex &)`, `int16_t queueOldestSlot(const QueueIndex &)`, `int16_t queueFindKind(const QueueIndex &, MsgKind)`, `uint8_t queuePush(QueueIndex &, MsgKind, uint32_t &id, bool &dropped)`, `bool queuePopIfId(QueueIndex &, uint32_t id)`
  - `enum class NetResult : uint8_t { Sent, Drop, Fail, RateLimited }`, `NetResult classifyHttp(int code)`, `BACKOFF_MAX_FAILURES` (5), `uint32_t backoffSeconds(uint8_t failures)`
  - `IP_LOOKUP_URL`, `TIMEAPI_URL_PREFIX`, `IPAPI_URL`, `MAX_UTC_OFFSET_S`
  - `const char *jsonValueAfterKey(const char *json, const char *key)`, `bool jsonReadOffset(const char *p, int32_t &out)`, `bool parseTimeapiOffset(const char *json, int32_t &offsetS)`, `bool parseIpApiOffset(const char *json, int32_t &offsetS)`, `bool parsePlainIp(const char *body, char *out, size_t cap)`, `bool buildTimeapiUrl(const char *ip, char *out, size_t cap)`
  - `int32_t tmOffsetSeconds(int localYear, int localYday, int32_t localSecOfDay, int utcYear, int utcYday, int32_t utcSecOfDay)`

- [ ] **Step 1: Write the failing tests**

The fixtures are the real responses captured with `curl.exe -s` while writing this plan (see Decisions 1). Create `test/test_netproto/test_main.cpp`:

```cpp
#include <unity.h>

#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

// Real responses captured on 2026-10-28 with curl.exe -s.
// https://timeapi.io/api/TimeZone/ip?ipAddress=<public IP> (Brussels, CET after DST ended)
static const char *TIMEAPI_BRUSSELS =
    R"JSON({"timeZone":"Europe/Brussels","currentLocalTime":"2026-10-28T23:53:17.7460455","currentUtcOffset":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"standardUtcOffset":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"hasDayLightSaving":true,"isDayLightSavingActive":false,"dstInterval":{"dstName":"CEST","dstOffsetToUtc":{"seconds":7200,"milliseconds":7200000,"ticks":72000000000,"nanoseconds":7200000000000},"dstOffsetToStandardTime":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"dstStart":"2027-03-28T01:00:00Z","dstEnd":"2027-10-31T01:00:00Z","dstDuration":{"days":217,"nanosecondOfDay":0,"hours":0,"minutes":0,"seconds":0,"milliseconds":0,"subsecondTicks":0,"subsecondNanoseconds":0,"bclCompatibleTicks":187488000000000,"totalDays":217,"totalHours":5208,"totalMinutes":312480,"totalSeconds":18748800,"totalMilliseconds":18748800000,"totalTicks":187488000000000,"totalNanoseconds":18748800000000000}}})JSON";

// https://timeapi.io/api/TimeZone/ip?ipAddress=8.8.8.8 (Los Angeles, PDT active)
static const char *TIMEAPI_LOS_ANGELES =
    R"JSON({"timeZone":"America/Los_Angeles","currentLocalTime":"2026-10-28T15:53:10.4713795","currentUtcOffset":{"seconds":-25200,"milliseconds":-25200000,"ticks":-252000000000,"nanoseconds":-25200000000000},"standardUtcOffset":{"seconds":-28800,"milliseconds":-28800000,"ticks":-288000000000,"nanoseconds":-28800000000000},"hasDayLightSaving":true,"isDayLightSavingActive":true,"dstInterval":{"dstName":"PDT","dstOffsetToUtc":{"seconds":-25200,"milliseconds":-25200000,"ticks":-252000000000,"nanoseconds":-25200000000000},"dstOffsetToStandardTime":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"dstStart":"2026-03-08T10:00:00Z","dstEnd":"2026-11-01T09:00:00Z","dstDuration":{"days":237,"nanosecondOfDay":82800000000000,"hours":23,"minutes":0,"seconds":0,"milliseconds":0,"subsecondTicks":0,"subsecondNanoseconds":0,"bclCompatibleTicks":205596000000000,"totalDays":237.95833333333334,"totalHours":5711,"totalMinutes":342660,"totalSeconds":20559600,"totalMilliseconds":20559600000,"totalTicks":205596000000000,"totalNanoseconds":20559600000000000}}})JSON";

// https://timeapi.io/api/TimeZone/ip without ipAddress (HTTP 400)
static const char *TIMEAPI_NO_IP =
    R"JSON({"type":"https://tools.ietf.org/html/rfc9110#section-15.5.1","title":"One or more validation errors occurred.","status":400,"errors":{"ipAddress":["The ipAddress field is required."]},"traceId":"00-65fe893e925122ee5ab90ced4fdf984c-b177feb16b6f8636-00"})JSON";

// http://ip-api.com/json/?fields=status,timezone,offset
static const char *IPAPI_BRUSSELS = R"JSON({"status":"success","timezone":"Europe/Brussels","offset":3600})JSON";
// http://ip-api.com/json/8.8.8.8?fields=status,timezone,offset
static const char *IPAPI_NEW_YORK = R"JSON({"status":"success","timezone":"America/New_York","offset":-14400})JSON";
// http://ip-api.com/json/10.0.0.1?fields=status,message,offset
static const char *IPAPI_FAIL = R"JSON({"status":"fail","message":"private range"})JSON";

static void test_timeapi_offset_uses_current_offset() {
  int32_t offset = 0;
  TEST_ASSERT_TRUE(cs::parseTimeapiOffset(TIMEAPI_BRUSSELS, offset));
  TEST_ASSERT_EQUAL_INT32(3600, offset);
  TEST_ASSERT_TRUE(cs::parseTimeapiOffset(TIMEAPI_LOS_ANGELES, offset));
  TEST_ASSERT_EQUAL_INT32(-25200, offset);
}

static void test_timeapi_rejects_error_and_garbage() {
  int32_t offset = 99;
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset(TIMEAPI_NO_IP, offset));
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset("", offset));
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset("{\"currentUtcOffset\":{\"seconds\":99999}}", offset));
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset("{\"currentUtcOffset\":3600}", offset));
  TEST_ASSERT_EQUAL_INT32(99, offset);
}

static void test_ipapi_offset() {
  int32_t offset = 0;
  TEST_ASSERT_TRUE(cs::parseIpApiOffset(IPAPI_BRUSSELS, offset));
  TEST_ASSERT_EQUAL_INT32(3600, offset);
  TEST_ASSERT_TRUE(cs::parseIpApiOffset(IPAPI_NEW_YORK, offset));
  TEST_ASSERT_EQUAL_INT32(-14400, offset);
  TEST_ASSERT_FALSE(cs::parseIpApiOffset(IPAPI_FAIL, offset));
  TEST_ASSERT_FALSE(cs::parseIpApiOffset("not json", offset));
}

static void test_json_key_finder() {
  const char *json = "{ \"a\" : 5, \"x\":\"seconds\", \"seconds\" :\n -12 }";
  const char *p = cs::jsonValueAfterKey(json, "seconds");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_CHAR('-', *p);
  TEST_ASSERT_NULL(cs::jsonValueAfterKey(json, "second"));
  TEST_ASSERT_EQUAL_CHAR('5', *cs::jsonValueAfterKey(json, "a"));
}

static void test_plain_ip_and_timeapi_url() {
  char ip[46];
  TEST_ASSERT_TRUE(cs::parsePlainIp("80.200.185.90", ip, sizeof ip));
  TEST_ASSERT_EQUAL_STRING("80.200.185.90", ip);
  TEST_ASSERT_TRUE(cs::parsePlainIp(" 2a02:a03f::1\n", ip, sizeof ip));
  TEST_ASSERT_EQUAL_STRING("2a02:a03f::1", ip);
  TEST_ASSERT_FALSE(cs::parsePlainIp("<html>", ip, sizeof ip));
  TEST_ASSERT_FALSE(cs::parsePlainIp("1.2.3.4 extra", ip, sizeof ip));
  TEST_ASSERT_FALSE(cs::parsePlainIp("", ip, sizeof ip));
  char url[128];
  TEST_ASSERT_TRUE(cs::buildTimeapiUrl("80.200.185.90", url, sizeof url));
  TEST_ASSERT_EQUAL_STRING("https://timeapi.io/api/TimeZone/ip?ipAddress=80.200.185.90", url);
}

static void test_tm_offset() {
  // 2026-10-28 23:53 local vs 22:53 UTC (yday 300).
  TEST_ASSERT_EQUAL_INT32(3600, cs::tmOffsetSeconds(126, 300, 23 * 3600 + 53 * 60, 126, 300, 22 * 3600 + 53 * 60));
  // Local already in the new year.
  TEST_ASSERT_EQUAL_INT32(3600, cs::tmOffsetSeconds(127, 0, 30 * 60, 126, 364, 23 * 3600 + 30 * 60));
  // Los Angeles 15:53 vs 22:53 UTC.
  TEST_ASSERT_EQUAL_INT32(-25200, cs::tmOffsetSeconds(126, 300, 15 * 3600 + 53 * 60, 126, 300, 22 * 3600 + 53 * 60));
  // Local still on the previous day.
  TEST_ASSERT_EQUAL_INT32(-18000, cs::tmOffsetSeconds(126, 299, 21 * 3600, 126, 300, 2 * 3600));
}

static void test_http_classification_and_backoff() {
  TEST_ASSERT_EQUAL(cs::NetResult::Sent, cs::classifyHttp(200));
  TEST_ASSERT_EQUAL(cs::NetResult::Sent, cs::classifyHttp(204));
  TEST_ASSERT_EQUAL(cs::NetResult::RateLimited, cs::classifyHttp(429));
  TEST_ASSERT_EQUAL(cs::NetResult::Drop, cs::classifyHttp(400));
  TEST_ASSERT_EQUAL(cs::NetResult::Drop, cs::classifyHttp(413));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(500));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(503));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(-1));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(301));
  TEST_ASSERT_EQUAL_UINT32(0, cs::backoffSeconds(0));
  TEST_ASSERT_EQUAL_UINT32(60, cs::backoffSeconds(1));
  TEST_ASSERT_EQUAL_UINT32(120, cs::backoffSeconds(2));
  TEST_ASSERT_EQUAL_UINT32(300, cs::backoffSeconds(3));
  TEST_ASSERT_EQUAL_UINT32(600, cs::backoffSeconds(4));
  TEST_ASSERT_EQUAL_UINT32(1800, cs::backoffSeconds(5));
  TEST_ASSERT_EQUAL_UINT32(1800, cs::backoffSeconds(9));
}

static void test_ring_overwrites_oldest_and_tracks_unsent() {
  cs::RingIndex r;
  const uint16_t cap = cs::RECORD_RING_SIZE;
  for (int k = 0; k < 10; k++) {
    cs::ringPush(r);
  }
  TEST_ASSERT_EQUAL_UINT32(10, cs::ringUnsentCount(r, cap));
  cs::ringMarkSentThrough(r, 4);
  TEST_ASSERT_EQUAL_UINT32(4, cs::ringFirstUnsentSeq(r, cap));
  TEST_ASSERT_EQUAL_UINT32(6, cs::ringUnsentCount(r, cap));
  cs::ringMarkSentThrough(r, 2);  // never moves back
  TEST_ASSERT_EQUAL_UINT32(4, r.sentSeq);
  for (int k = 0; k < 300; k++) {
    cs::ringPush(r);
  }
  TEST_ASSERT_EQUAL_UINT32(310, r.nextSeq);
  TEST_ASSERT_EQUAL_UINT32(22, cs::ringOldestSeq(r, cap));
  TEST_ASSERT_EQUAL_UINT32(22, cs::ringFirstUnsentSeq(r, cap));
  TEST_ASSERT_EQUAL_UINT32(288, cs::ringUnsentCount(r, cap));
  TEST_ASSERT_EQUAL_UINT16(22, cs::ringSlot(310, cap));
  cs::ringMarkSentThrough(r, 999);
  TEST_ASSERT_EQUAL_UINT32(310, r.sentSeq);
  TEST_ASSERT_EQUAL_UINT32(0, cs::ringUnsentCount(r, cap));
}

static void test_record_send_action_and_fill() {
  cs::Record r;
  r.boot = 4;
  r.startMs = 100000;
  TEST_ASSERT_EQUAL(cs::RecordAction::Wait, cs::recordSendAction(r, 4));
  TEST_ASSERT_EQUAL(cs::RecordAction::Skip, cs::recordSendAction(r, 5));
  TEST_ASSERT_FALSE(cs::recordFillTime(r, 5, 1793227997UL, 400000));
  TEST_ASSERT_FALSE(cs::recordFillTime(r, 4, 1000, 400000));
  TEST_ASSERT_TRUE(cs::recordFillTime(r, 4, 1793227997UL, 400000));
  TEST_ASSERT_EQUAL_UINT32(1793227997UL - 300, r.t);
  TEST_ASSERT_EQUAL(cs::RecordAction::Include, cs::recordSendAction(r, 9));
  cs::Message m;
  m.boot = 4;
  m.eventMs = 340000;
  TEST_ASSERT_TRUE(cs::messageFillTime(m, 4, 1793227997UL, 400000));
  TEST_ASSERT_EQUAL_UINT32(1793227997UL - 60, m.eventUnix);
  TEST_ASSERT_FALSE(cs::messageFillTime(m, 4, 1793227997UL, 400000));
}

static void test_queue_drops_oldest_when_full() {
  cs::QueueIndex q;
  uint32_t id = 0;
  bool dropped = false;
  uint32_t firstId = 0;
  for (int k = 0; k < cs::OUTBOX_SIZE; k++) {
    cs::queuePush(q, cs::MsgKind::Alert, id, dropped);
    if (k == 0) {
      firstId = id;
    }
    TEST_ASSERT_FALSE(dropped);
  }
  TEST_ASSERT_EQUAL_UINT8(16, cs::queueCount(q));
  TEST_ASSERT_EQUAL_UINT32(firstId, q.ids[cs::queueOldestSlot(q)]);
  uint8_t slot = cs::queuePush(q, cs::MsgKind::Clear, id, dropped);
  TEST_ASSERT_TRUE(dropped);
  TEST_ASSERT_EQUAL_UINT8(0, slot);
  TEST_ASSERT_EQUAL_UINT8(16, cs::queueCount(q));
  TEST_ASSERT_FALSE(cs::queuePopIfId(q, firstId));  // already dropped
  TEST_ASSERT_EQUAL_INT16(1, cs::queueOldestSlot(q));
}

static void test_queue_pop_and_morning_replacement() {
  cs::QueueIndex q;
  uint32_t alertId = 0;
  uint32_t morningId = 0;
  uint32_t newMorningId = 0;
  bool dropped = false;
  TEST_ASSERT_EQUAL_INT16(-1, cs::queueOldestSlot(q));
  cs::queuePush(q, cs::MsgKind::Alert, alertId, dropped);
  uint8_t morningSlot = cs::queuePush(q, cs::MsgKind::Morning, morningId, dropped);
  uint8_t replaced = cs::queuePush(q, cs::MsgKind::Morning, newMorningId, dropped);
  TEST_ASSERT_EQUAL_UINT8(morningSlot, replaced);
  TEST_ASSERT_EQUAL_UINT8(2, cs::queueCount(q));
  TEST_ASSERT_TRUE(cs::queuePopIfId(q, alertId));
  TEST_ASSERT_FALSE(cs::queuePopIfId(q, morningId));  // replaced while being sent
  TEST_ASSERT_TRUE(cs::queuePopIfId(q, newMorningId));
  TEST_ASSERT_EQUAL_UINT8(0, cs::queueCount(q));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_timeapi_offset_uses_current_offset);
  RUN_TEST(test_timeapi_rejects_error_and_garbage);
  RUN_TEST(test_ipapi_offset);
  RUN_TEST(test_json_key_finder);
  RUN_TEST(test_plain_ip_and_timeapi_url);
  RUN_TEST(test_tm_offset);
  RUN_TEST(test_http_classification_and_backoff);
  RUN_TEST(test_ring_overwrites_oldest_and_tracks_unsent);
  RUN_TEST(test_record_send_action_and_fill);
  RUN_TEST(test_queue_drops_oldest_when_full);
  RUN_TEST(test_queue_pop_and_morning_replacement);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_netproto`
Expected: FAIL, compile errors such as `'parseTimeapiOffset' is not a member of 'cs'`.

- [ ] **Step 3: Implement in the header**

In `include/chargescreen_stats.h`, directly before the final `}  // namespace cs`, add:

```cpp
// ---------------------------------------------------------------- record cache ring

// Sequence numbers never wrap in practice. Record seq lives in slot seq % cap.
struct RingIndex {
  uint32_t nextSeq = 0;  // seq of the next record to write
  uint32_t sentSeq = 0;  // every seq below this has been sent
};

inline uint32_t ringOldestSeq(const RingIndex &r, uint16_t cap) {
  return r.nextSeq > cap ? r.nextSeq - cap : 0;
}

inline uint32_t ringFirstUnsentSeq(const RingIndex &r, uint16_t cap) {
  uint32_t oldest = ringOldestSeq(r, cap);
  return r.sentSeq > oldest ? r.sentSeq : oldest;
}

inline uint32_t ringUnsentCount(const RingIndex &r, uint16_t cap) {
  return r.nextSeq - ringFirstUnsentSeq(r, cap);
}

inline uint16_t ringSlot(uint32_t seq, uint16_t cap) {
  return static_cast<uint16_t>(seq % cap);
}

// Returns the seq to write; the oldest record is overwritten when full.
inline uint32_t ringPush(RingIndex &r) {
  return r.nextSeq++;
}

inline void ringMarkSentThrough(RingIndex &r, uint32_t endSeq) {
  if (endSeq > r.nextSeq) {
    endSeq = r.nextSeq;
  }
  if (endSeq > r.sentSeq) {
    r.sentSeq = endSeq;
  }
}

enum class RecordAction : uint8_t { Include, Wait, Skip };

// Records without a clock time wait for the first sync in their own boot;
// those left over from an earlier boot can never be dated and are skipped.
inline RecordAction recordSendAction(const Record &r, uint16_t currentBoot) {
  if (r.t != 0) {
    return RecordAction::Include;
  }
  return r.boot == currentBoot ? RecordAction::Wait : RecordAction::Skip;
}

inline bool recordFillTime(Record &r, uint16_t currentBoot, uint32_t nowUnix, uint32_t nowMs) {
  if (r.t != 0 || r.boot != currentBoot || !clockValid(nowUnix)) {
    return false;
  }
  r.t = nowUnix - static_cast<uint32_t>(nowMs - r.startMs) / 1000U;
  return true;
}

inline bool messageFillTime(Message &m, uint16_t currentBoot, uint32_t nowUnix, uint32_t nowMs) {
  if (m.eventUnix != 0 || m.boot != currentBoot || !clockValid(nowUnix)) {
    return false;
  }
  m.eventUnix = nowUnix - static_cast<uint32_t>(nowMs - m.eventMs) / 1000U;
  return true;
}

// ---------------------------------------------------------------- outbox queue

struct QueueIndex {
  uint32_t headSeq = 0;  // oldest pending message
  uint32_t nextSeq = 0;
  uint32_t nextId = 1;
  uint32_t ids[OUTBOX_SIZE] = {};
  uint8_t kinds[OUTBOX_SIZE] = {};
};

inline uint8_t queueCount(const QueueIndex &q) {
  return static_cast<uint8_t>(q.nextSeq - q.headSeq);
}

inline int16_t queueOldestSlot(const QueueIndex &q) {
  return queueCount(q) ? static_cast<int16_t>(q.headSeq % OUTBOX_SIZE) : -1;
}

inline int16_t queueFindKind(const QueueIndex &q, MsgKind kind) {
  for (uint32_t seq = q.headSeq; seq != q.nextSeq; seq++) {
    uint8_t slot = static_cast<uint8_t>(seq % OUTBOX_SIZE);
    if (q.kinds[slot] == static_cast<uint8_t>(kind)) {
      return slot;
    }
  }
  return -1;
}

// Returns the slot to write the message into. A pending morning report is
// replaced in place by a new one; otherwise the message is appended and the
// oldest is dropped when the queue is full.
inline uint8_t queuePush(QueueIndex &q, MsgKind kind, uint32_t &id, bool &dropped) {
  dropped = false;
  id = q.nextId++;
  if (q.nextId == 0) {
    q.nextId = 1;
  }
  if (kind == MsgKind::Morning) {
    int16_t pending = queueFindKind(q, kind);
    if (pending >= 0) {
      q.ids[pending] = id;
      return static_cast<uint8_t>(pending);
    }
  }
  if (queueCount(q) >= OUTBOX_SIZE) {
    q.headSeq++;
    dropped = true;
  }
  uint8_t slot = static_cast<uint8_t>(q.nextSeq % OUTBOX_SIZE);
  q.nextSeq++;
  q.ids[slot] = id;
  q.kinds[slot] = static_cast<uint8_t>(kind);
  return slot;
}

// Removes the oldest message only if it is still the one that was sent.
inline bool queuePopIfId(QueueIndex &q, uint32_t id) {
  int16_t slot = queueOldestSlot(q);
  if (slot < 0 || q.ids[slot] != id) {
    return false;
  }
  q.headSeq++;
  return true;
}

// ---------------------------------------------------------------- network results

enum class NetResult : uint8_t { Sent, Drop, Fail, RateLimited };

// code <= 0 is a network or TLS error from HTTPClient.
inline NetResult classifyHttp(int code) {
  if (code >= 200 && code < 300) {
    return NetResult::Sent;
  }
  if (code == 429) {
    return NetResult::RateLimited;
  }
  if (code >= 400 && code < 500) {
    return NetResult::Drop;
  }
  return NetResult::Fail;
}

constexpr uint8_t BACKOFF_MAX_FAILURES = 5;

// 1, 2, 5, 10, then 30 minutes. A 429 sets failures to BACKOFF_MAX_FAILURES.
inline uint32_t backoffSeconds(uint8_t failures) {
  constexpr uint16_t steps[BACKOFF_MAX_FAILURES] = {60, 120, 300, 600, 1800};
  if (failures == 0) {
    return 0;
  }
  return steps[(failures > BACKOFF_MAX_FAILURES ? BACKOFF_MAX_FAILURES : failures) - 1];
}

// ---------------------------------------------------------------- time zone lookup

constexpr const char *IP_LOOKUP_URL = "https://api.ipify.org";
constexpr const char *TIMEAPI_URL_PREFIX = "https://timeapi.io/api/TimeZone/ip?ipAddress=";
constexpr const char *IPAPI_URL = "http://ip-api.com/json/?fields=status,message,offset";
constexpr int32_t MAX_UTC_OFFSET_S = 14 * 3600;

inline bool jsonSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Minimal key finder: returns a pointer to the value after "key": (spaces
// skipped), searching from json, or nullptr.
inline const char *jsonValueAfterKey(const char *json, const char *key) {
  size_t keyLen = strlen(key);
  for (const char *p = strchr(json, '"'); p; p = strchr(p + 1, '"')) {
    if (strncmp(p + 1, key, keyLen) != 0 || p[1 + keyLen] != '"') {
      continue;
    }
    const char *q = p + 2 + keyLen;
    while (jsonSpace(*q)) {
      q++;
    }
    if (*q != ':') {
      continue;
    }
    q++;
    while (jsonSpace(*q)) {
      q++;
    }
    return q;
  }
  return nullptr;
}

inline bool jsonReadOffset(const char *p, int32_t &out) {
  if (!p) {
    return false;
  }
  char *end = nullptr;
  long value = strtol(p, &end, 10);
  if (end == p || value < -MAX_UTC_OFFSET_S || value > MAX_UTC_OFFSET_S) {
    return false;
  }
  out = static_cast<int32_t>(value);
  return true;
}

// timeapi.io /api/TimeZone/ip?ipAddress=..: currentUtcOffset.seconds
// (includes daylight saving).
inline bool parseTimeapiOffset(const char *json, int32_t &offsetS) {
  const char *current = jsonValueAfterKey(json, "currentUtcOffset");
  if (!current || *current != '{') {
    return false;
  }
  return jsonReadOffset(jsonValueAfterKey(current, "seconds"), offsetS);
}

// ip-api.com /json/?fields=status,message,offset: offset (includes daylight
// saving) when status is "success".
inline bool parseIpApiOffset(const char *json, int32_t &offsetS) {
  const char *status = jsonValueAfterKey(json, "status");
  if (!status || strncmp(status, "\"success\"", 9) != 0) {
    return false;
  }
  return jsonReadOffset(jsonValueAfterKey(json, "offset"), offsetS);
}

// api.ipify.org returns the public IP as plain text.
inline bool parsePlainIp(const char *body, char *out, size_t cap) {
  while (jsonSpace(*body)) {
    body++;
  }
  size_t len = 0;
  bool separator = false;
  for (; *body && !jsonSpace(*body); ++body) {
    char c = *body;
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex && c != '.' && c != ':') {
      return false;
    }
    separator = separator || c == '.' || c == ':';
    if (len + 1 >= cap) {
      return false;
    }
    out[len++] = c;
  }
  while (jsonSpace(*body)) {
    body++;
  }
  out[len] = '\0';
  return *body == '\0' && separator && len >= 3;
}

inline bool buildTimeapiUrl(const char *ip, char *out, size_t cap) {
  TextWriter w(out, cap);
  w.putStr(TIMEAPI_URL_PREFIX);
  w.putStr(ip);
  return w.result() > 0;
}

// Offset of local time from UTC, from the struct tm fields of both
// (tm_year, tm_yday, seconds of day). Used with a POSIX TZ override.
inline int32_t tmOffsetSeconds(int localYear, int localYday, int32_t localSecOfDay, int utcYear, int utcYday,
                               int32_t utcSecOfDay) {
  int dayDiff;
  if (localYear != utcYear) {
    dayDiff = localYear > utcYear ? 1 : -1;
  } else {
    dayDiff = localYday - utcYday;
  }
  return dayDiff * 86400 + localSecOfDay - utcSecOfDay;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native -f test_netproto`
Expected: `11 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 5: Run every suite**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`
Expected: `89 test cases: 89 succeeded` across 8 suites.

- [ ] **Step 6: Build firmware**

No firmware file includes the header yet (Task 7 is the first), so this only confirms nothing else broke.
Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`. Then run `git checkout -- include/build_version.h`.

- [ ] **Step 7: Commit**

```bash
git add include/chargescreen_stats.h test/test_netproto/test_main.cpp
git commit -m "Add cache indexes, HTTP result handling, backoff and time-zone parsing

Time-zone parsers are tested against real timeapi.io and ip-api.com
responses. worldtimeapi.org was unreachable and is not used.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---
### Task 6: Cache store

**Files:**
- Create: `src/telemetry.h`
- Create: `src/telemetry.cpp`

**Interfaces:**
- Consumes: `RECORD_RING_SIZE`, `OUTBOX_SIZE`, `clockValid` (Task 1); `DayHistory` (Task 2); `MsgKind`, `Message` (Task 3); `Record` (Task 4); `RingIndex`, `ringPush`, `ringSlot`, `ringOldestSeq`, `ringFirstUnsentSeq`, `ringUnsentCount`, `ringMarkSentThrough`, `RecordAction`, `recordSendAction`, `recordFillTime`, `messageFillTime`, `QueueIndex`, `queueCount`, `queueOldestSlot`, `queuePush`, `queuePopIfId` (Task 5).
- Produces (global functions in `telemetry.h`, each takes the store mutex):
  - `bool storeBegin()` — creates the mutex, validates or recreates `/tx_cache.bin` and `/outbox.bin`, increments the boot id. Call after `LittleFS.begin()`.
  - `uint16_t storeBootId()`
  - `bool storeAppendRecord(const cs::Record &rec)`
  - `bool storeEnqueueMessage(const cs::Message &msg)`
  - `bool storePeekMessage(cs::Message &msg, uint32_t &id)`, `bool storePopMessage(uint32_t id)`
  - `uint8_t storeReadBatch(cs::Record *out, uint8_t maxRecords, uint32_t &endSeq)` — the oldest unsent dated records; `endSeq` also covers skipped undatable ones
  - `void storeMarkRecordsSent(uint32_t endSeq)`
  - `void storeFillTimes(uint32_t nowUnix, uint32_t nowMs)` — dates records and messages of this boot that were created before the clock was known
  - `uint8_t storeMessageCount()`, `uint32_t storeUnsentRecordCount()`
  - `bool storeLoadDays(cs::DayHistory &days)`, `bool storeSaveDays(const cs::DayHistory &days)`

File formats (fixed-size binary; a wrong version, element size, capacity or file size discards the file):
- `/tx_cache.bin`: `CacheHeader { uint8_t version (1); uint8_t reserved; uint16_t recordSize (44); uint16_t capacity (288); uint16_t boot; cs::RingIndex ring; }` then 288 × `cs::Record`.
- `/outbox.bin`: `OutboxHeader { uint8_t version (1); uint8_t reserved; uint16_t messageSize (380); uint16_t capacity (16); uint16_t reserved2; cs::QueueIndex queue; }` then 16 × `cs::Message`.
- `/days.bin`: `DaysHeader { uint8_t version (1); uint8_t reserved; uint16_t size; }` then one `cs::DayHistory`.

- [ ] **Step 1: Create the header**

Create `src/telemetry.h`:

```cpp
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "chargescreen_stats.h"

// Cache store: /tx_cache.bin, /outbox.bin and /days.bin on the LittleFS
// mount. Every store function takes the store mutex, so it is safe to call
// from loop() and from the netlink task.
bool storeBegin();
uint16_t storeBootId();
bool storeAppendRecord(const cs::Record &rec);
bool storeEnqueueMessage(const cs::Message &msg);
bool storePeekMessage(cs::Message &msg, uint32_t &id);
bool storePopMessage(uint32_t id);
uint8_t storeReadBatch(cs::Record *out, uint8_t maxRecords, uint32_t &endSeq);
void storeMarkRecordsSent(uint32_t endSeq);
void storeFillTimes(uint32_t nowUnix, uint32_t nowMs);
uint8_t storeMessageCount();
uint32_t storeUnsentRecordCount();
bool storeLoadDays(cs::DayHistory &days);
bool storeSaveDays(const cs::DayHistory &days);
```

- [ ] **Step 2: Create the store**

Create `src/telemetry.cpp`:

```cpp
#include "telemetry.h"

#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr const char *CACHE_PATH = "/tx_cache.bin";
constexpr const char *OUTBOX_PATH = "/outbox.bin";
constexpr const char *DAYS_PATH = "/days.bin";
constexpr uint8_t STORE_VERSION = 1;

struct CacheHeader {
  uint8_t version = 0;
  uint8_t reserved = 0;
  uint16_t recordSize = 0;
  uint16_t capacity = 0;
  uint16_t boot = 0;
  cs::RingIndex ring;
};

struct OutboxHeader {
  uint8_t version = 0;
  uint8_t reserved = 0;
  uint16_t messageSize = 0;
  uint16_t capacity = 0;
  uint16_t reserved2 = 0;
  cs::QueueIndex queue;
};

struct DaysHeader {
  uint8_t version = 0;
  uint8_t reserved = 0;
  uint16_t size = 0;
};

SemaphoreHandle_t storeMutex = nullptr;
CacheHeader cacheHeader;
OutboxHeader outboxHeader;
bool storeReady = false;

class StoreLock {
 public:
  StoreLock() { xSemaphoreTake(storeMutex, portMAX_DELAY); }
  ~StoreLock() { xSemaphoreGive(storeMutex); }
};

size_t cacheOffset(uint32_t seq) {
  return sizeof(CacheHeader) + sizeof(cs::Record) * cs::ringSlot(seq, cs::RECORD_RING_SIZE);
}

size_t outboxOffset(uint8_t slot) {
  return sizeof(OutboxHeader) + sizeof(cs::Message) * slot;
}

bool writeAt(const char *path, size_t offset, const void *data, size_t len) {
  File f = LittleFS.open(path, "r+");
  if (!f) {
    return false;
  }
  bool ok = f.seek(offset) && f.write(static_cast<const uint8_t *>(data), len) == len;
  f.close();
  return ok;
}

bool readAt(const char *path, size_t offset, void *data, size_t len) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    return false;
  }
  bool ok = f.seek(offset) && f.read(static_cast<uint8_t *>(data), len) == len;
  f.close();
  return ok;
}

size_t fileSize(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    return 0;
  }
  size_t size = f.size();
  f.close();
  return size;
}

bool createZeroFile(const char *path, size_t size) {
  LittleFS.remove(path);
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  uint8_t zeros[256] = {};
  size_t left = size;
  while (left) {
    size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
    if (f.write(zeros, n) != n) {
      f.close();
      return false;
    }
    left -= n;
  }
  f.close();
  return true;
}

// A wrong version, record size, capacity or file size discards the file.
bool openCache() {
  const size_t size = sizeof(CacheHeader) + sizeof(cs::Record) * cs::RECORD_RING_SIZE;
  CacheHeader h;
  bool ok = fileSize(CACHE_PATH) == size && readAt(CACHE_PATH, 0, &h, sizeof(h)) && h.version == STORE_VERSION &&
            h.recordSize == sizeof(cs::Record) && h.capacity == cs::RECORD_RING_SIZE;
  if (!ok) {
    Serial.println("Online: new record cache");
    if (!createZeroFile(CACHE_PATH, size)) {
      return false;
    }
    h = CacheHeader();
    h.version = STORE_VERSION;
    h.recordSize = sizeof(cs::Record);
    h.capacity = cs::RECORD_RING_SIZE;
  }
  h.boot++;
  if (h.boot == 0) {
    h.boot = 1;
  }
  cacheHeader = h;
  return writeAt(CACHE_PATH, 0, &cacheHeader, sizeof(cacheHeader));
}

bool openOutbox() {
  const size_t size = sizeof(OutboxHeader) + sizeof(cs::Message) * cs::OUTBOX_SIZE;
  OutboxHeader h;
  bool ok = fileSize(OUTBOX_PATH) == size && readAt(OUTBOX_PATH, 0, &h, sizeof(h)) && h.version == STORE_VERSION &&
            h.messageSize == sizeof(cs::Message) && h.capacity == cs::OUTBOX_SIZE;
  if (!ok) {
    Serial.println("Online: new outbox");
    if (!createZeroFile(OUTBOX_PATH, size)) {
      return false;
    }
    h = OutboxHeader();
    h.version = STORE_VERSION;
    h.messageSize = sizeof(cs::Message);
    h.capacity = cs::OUTBOX_SIZE;
    if (!writeAt(OUTBOX_PATH, 0, &h, sizeof(h))) {
      return false;
    }
  }
  outboxHeader = h;
  return true;
}

}  // namespace

bool storeBegin() {
  if (!storeMutex) {
    storeMutex = xSemaphoreCreateMutex();
  }
  StoreLock lock;
  storeReady = openCache() && openOutbox();
  Serial.printf("Online: store %s, boot %u, %lu unsent records, %u messages\n",
                storeReady ? "ready" : "FAILED",
                cacheHeader.boot,
                static_cast<unsigned long>(cs::ringUnsentCount(cacheHeader.ring, cs::RECORD_RING_SIZE)),
                cs::queueCount(outboxHeader.queue));
  return storeReady;
}

uint16_t storeBootId() {
  return cacheHeader.boot;
}

bool storeAppendRecord(const cs::Record &rec) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  uint32_t seq = cs::ringPush(cacheHeader.ring);
  bool ok = writeAt(CACHE_PATH, cacheOffset(seq), &rec, sizeof(rec));
  return writeAt(CACHE_PATH, 0, &cacheHeader, sizeof(cacheHeader)) && ok;
}

bool storeEnqueueMessage(const cs::Message &msg) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  uint32_t id = 0;
  bool dropped = false;
  uint8_t slot = cs::queuePush(outboxHeader.queue, static_cast<cs::MsgKind>(msg.kind), id, dropped);
  if (dropped) {
    Serial.println("Online: outbox full, dropped the oldest message");
  }
  bool ok = writeAt(OUTBOX_PATH, outboxOffset(slot), &msg, sizeof(msg));
  return writeAt(OUTBOX_PATH, 0, &outboxHeader, sizeof(outboxHeader)) && ok;
}

bool storePeekMessage(cs::Message &msg, uint32_t &id) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  int16_t slot = cs::queueOldestSlot(outboxHeader.queue);
  if (slot < 0) {
    return false;
  }
  id = outboxHeader.queue.ids[slot];
  return readAt(OUTBOX_PATH, outboxOffset(static_cast<uint8_t>(slot)), &msg, sizeof(msg));
}

bool storePopMessage(uint32_t id) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  if (!cs::queuePopIfId(outboxHeader.queue, id)) {
    return false;
  }
  return writeAt(OUTBOX_PATH, 0, &outboxHeader, sizeof(outboxHeader));
}

uint8_t storeReadBatch(cs::Record *out, uint8_t maxRecords, uint32_t &endSeq) {
  endSeq = 0;
  if (!storeReady) {
    return 0;
  }
  StoreLock lock;
  uint32_t seq = cs::ringFirstUnsentSeq(cacheHeader.ring, cs::RECORD_RING_SIZE);
  uint8_t count = 0;
  File f = LittleFS.open(CACHE_PATH, "r");
  if (f) {
    while (seq < cacheHeader.ring.nextSeq && count < maxRecords) {
      cs::Record rec;
      if (!f.seek(cacheOffset(seq)) || f.read(reinterpret_cast<uint8_t *>(&rec), sizeof(rec)) != sizeof(rec)) {
        break;
      }
      cs::RecordAction action = cs::recordSendAction(rec, cacheHeader.boot);
      if (action == cs::RecordAction::Wait) {
        break;
      }
      if (action == cs::RecordAction::Include) {
        out[count++] = rec;
      }
      seq++;
    }
    f.close();
  }
  endSeq = seq;
  return count;
}

void storeMarkRecordsSent(uint32_t endSeq) {
  if (!storeReady) {
    return;
  }
  StoreLock lock;
  uint32_t before = cacheHeader.ring.sentSeq;
  cs::ringMarkSentThrough(cacheHeader.ring, endSeq);
  if (cacheHeader.ring.sentSeq != before) {
    writeAt(CACHE_PATH, 0, &cacheHeader, sizeof(cacheHeader));
  }
}

void storeFillTimes(uint32_t nowUnix, uint32_t nowMs) {
  if (!storeReady || !cs::clockValid(nowUnix)) {
    return;
  }
  StoreLock lock;
  File cache = LittleFS.open(CACHE_PATH, "r+");
  if (cache) {
    for (uint32_t seq = cs::ringOldestSeq(cacheHeader.ring, cs::RECORD_RING_SIZE); seq < cacheHeader.ring.nextSeq;
         seq++) {
      cs::Record rec;
      size_t offset = cacheOffset(seq);
      if (!cache.seek(offset) || cache.read(reinterpret_cast<uint8_t *>(&rec), sizeof(rec)) != sizeof(rec)) {
        break;
      }
      if (cs::recordFillTime(rec, cacheHeader.boot, nowUnix, nowMs)) {
        cache.seek(offset);
        cache.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
      }
    }
    cache.close();
  }
  File outbox = LittleFS.open(OUTBOX_PATH, "r+");
  if (outbox) {
    for (uint32_t seq = outboxHeader.queue.headSeq; seq != outboxHeader.queue.nextSeq; seq++) {
      cs::Message msg;
      size_t offset = outboxOffset(static_cast<uint8_t>(seq % cs::OUTBOX_SIZE));
      if (!outbox.seek(offset) || outbox.read(reinterpret_cast<uint8_t *>(&msg), sizeof(msg)) != sizeof(msg)) {
        break;
      }
      if (cs::messageFillTime(msg, cacheHeader.boot, nowUnix, nowMs)) {
        outbox.seek(offset);
        outbox.write(reinterpret_cast<const uint8_t *>(&msg), sizeof(msg));
      }
    }
    outbox.close();
  }
}

uint8_t storeMessageCount() {
  if (!storeReady) {
    return 0;
  }
  StoreLock lock;
  return cs::queueCount(outboxHeader.queue);
}

uint32_t storeUnsentRecordCount() {
  if (!storeReady) {
    return 0;
  }
  StoreLock lock;
  return cs::ringUnsentCount(cacheHeader.ring, cs::RECORD_RING_SIZE);
}

bool storeLoadDays(cs::DayHistory &days) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  DaysHeader h;
  if (fileSize(DAYS_PATH) != sizeof(DaysHeader) + sizeof(cs::DayHistory) || !readAt(DAYS_PATH, 0, &h, sizeof(h)) ||
      h.version != STORE_VERSION || h.size != sizeof(cs::DayHistory)) {
    LittleFS.remove(DAYS_PATH);
    return false;
  }
  return readAt(DAYS_PATH, sizeof(DaysHeader), &days, sizeof(days));
}

bool storeSaveDays(const cs::DayHistory &days) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  File f = LittleFS.open(DAYS_PATH, "w");
  if (!f) {
    return false;
  }
  DaysHeader h;
  h.version = STORE_VERSION;
  h.size = sizeof(cs::DayHistory);
  bool ok = f.write(reinterpret_cast<const uint8_t *>(&h), sizeof(h)) == sizeof(h) &&
            f.write(reinterpret_cast<const uint8_t *>(&days), sizeof(days)) == sizeof(days);
  f.close();
  return ok;
}
```

- [ ] **Step 3: Build**

Nothing calls the store yet; this checks that it compiles for the ESP32-C3.
Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`. Then run `git checkout -- include/build_version.h`.

- [ ] **Step 4: Commit**

```bash
git add src/telemetry.h src/telemetry.cpp
git commit -m "Add LittleFS cache store for records, outbox and day stats

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: netlink task

**Files:**
- Create: `src/netlink.h`
- Create: `src/netlink.cpp`

**Interfaces:**
- Consumes: Tasks 1–5 (`clockValid`, `ClockNow`, `OnlineState`, `MsgKind`, `Message`, `kindName`, `kindTitle`, `kindTag`, `kindPriority`, `formatDelayedSuffix`, `insertAfterFirstLine`, `Record`, `buildBatchJson`, `BATCH_JSON_MAX`, `ENVELOPE_MAX`, `RECORDS_PER_BATCH`, `BATCHES_PER_CONNECTION`, `SinkType`, `sinkType`, `ntfyTarget`, `buildNtfyEnvelope`, `buildGenericEnvelope`, `NetResult`, `classifyHttp`, `BACKOFF_MAX_FAILURES`, `backoffSeconds`, `IP_LOOKUP_URL`, `IPAPI_URL`, `parsePlainIp`, `buildTimeapiUrl`, `parseTimeapiOffset`, `parseIpApiOffset`, `tmOffsetSeconds`); Task 6 (`storeBootId`, `storePeekMessage`, `storePopMessage`, `storeReadBatch`, `storeMarkRecordsSent`, `storeFillTimes`, `storeMessageCount`).
- Produces (`netlink.h`):
  - `struct NetConfig { char ssid[2][33]; char pass[2][65]; char url[192]; char tz[64]; uint8_t batchMin; bool enabled; }`
  - `struct OnlineStatus { cs::OnlineState state; uint32_t lastUnix; }`
  - `void netlinkBegin()`, `void netlinkConfigure(const NetConfig &config)`, `void netlinkNotify()`, `void netlinkPause()`, `void netlinkResume()`, `void netlinkSetSuppressed(bool suppressed)`, `OnlineStatus netlinkStatus()`
  - `cs::ClockNow clockNow()`

Behaviour implemented here:
- Task: `xTaskCreate`, priority 1, stack 8192, woken by `xTaskNotifyGive` (alert, report, test) or a timeout (batch timer, backoff end). First attempt 60 s after boot, then every `batchMin` minutes.
- Attempt: skip if paused, demo mode or feature off; skip as a failure if `ESP.getFreeHeap()` < 60 KB; `WiFi.mode(WIFI_STA)`, try WiFi 1 then WiFi 2 for 15 s each; NTP with `configTime(0, 0, "pool.ntp.org", "time.google.com")` (then re-apply `TZ`, because `configTime` rewrites it); date undated records and messages; refresh the offset if unknown or older than 24 h (ipify + timeapi.io, then ip-api.com); send the outbox oldest first, then up to 6 batches of up to 12 records; finally `WiFi.disconnect(true); WiFi.mode(WIFI_OFF)`.
- HTTPS: `WiFiClientSecure` with `setInsecure()`, `HTTPClient` with 10 s connect and read timeouts; plain `WiFiClient` for `http://` URLs (ip-api.com, or a generic `http://` endpoint).
- Results: 2xx sent; other 4xx dropped; 429 → 30 min backoff; everything else → next backoff step (1, 2, 5, 10, 30 min); success resets. After a failure the task retries at the backoff end if alerts or reports are pending.
- Pause: `netlinkPause()` sets the flag and waits (up to 30 s) until the task is idle. The task sets `busy` before it checks `paused`, and checks `paused` while joining WiFi, during NTP and between messages, so a pause aborts at the next step.
- Serial log after every POST: HTTP code, free heap, minimum free heap and remaining task stack (for the device checks).

- [ ] **Step 1: Create the header**

Create `src/netlink.h`:

```cpp
#pragma once

#include <Arduino.h>

#include "chargescreen_stats.h"

struct NetConfig {
  char ssid[2][33];
  char pass[2][65];
  char url[192];
  char tz[64];  // POSIX TZ override, empty = automatic offset lookup
  uint8_t batchMin;
  bool enabled;
};

struct OnlineStatus {
  cs::OnlineState state;
  uint32_t lastUnix;  // UTC of the last attempt, 0 = none
};

// The netlink task owns WiFi client mode. It joins WiFi only to send.
void netlinkBegin();
void netlinkConfigure(const NetConfig &config);
void netlinkNotify();
// Blocks until any send in progress has finished or aborted; no new
// attempt starts until netlinkResume(). Used around the settings hotspot.
void netlinkPause();
void netlinkResume();
void netlinkSetSuppressed(bool suppressed);
OnlineStatus netlinkStatus();

// UTC from the system clock, plus the local offset (lookup or TZ override).
cs::ClockNow clockNow();
```

- [ ] **Step 2: Create the task**

Create `src/netlink.cpp`:

```cpp
#include "netlink.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>
#include <time.h>

#include "telemetry.h"

namespace {

constexpr uint32_t NET_TASK_STACK = 8192;
constexpr UBaseType_t NET_TASK_PRIORITY = 1;
constexpr uint32_t WIFI_CONNECT_MS = 15000;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_WAIT_MS = 10000;
constexpr uint32_t MIN_FREE_HEAP = 60UL * 1024UL;
constexpr uint32_t FIRST_ATTEMPT_DELAY_MS = 60000;
constexpr uint32_t OFFSET_REFRESH_S = 86400;
constexpr uint32_t PAUSE_WAIT_MS = 30000;

enum class Attempt : uint8_t { Done, Failed, RateLimited, Paused, Off };

TaskHandle_t netTask = nullptr;
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
NetConfig sharedConfig = {};  // written by loop(), guarded by configMux
NetConfig activeConfig = {};  // netlink task copy

// ESP32-C3 is single-core and aligned 32-bit loads and stores are atomic,
// so these flags are plain volatiles.
volatile bool paused = false;
volatile bool busy = false;
volatile bool suppressed = false;
volatile bool tzOverrideActive = false;
volatile bool offsetKnown = false;
volatile int32_t offsetSeconds = 0;
volatile uint8_t onlineState = static_cast<uint8_t>(cs::OnlineState::Off);
volatile uint32_t lastAttemptUnix = 0;

// Netlink task only.
uint32_t offsetFetchedUnix = 0;
uint8_t failures = 0;
bool backoffActive = false;
uint32_t backoffUntilMs = 0;
char bodyBuffer[cs::BATCH_JSON_MAX];
char envelopeBuffer[cs::ENVELOPE_MAX];
char responseBuffer[2048];
cs::Record batchRecords[cs::RECORDS_PER_BATCH];
cs::Message messageBuffer;

void applyTimeZone(const char *tz) {
  setenv("TZ", tz[0] ? tz : "UTC0", 1);
  tzset();
}

int httpRequestWith(WiFiClient &client, const char *url, const char *postBody, char *response, size_t responseCap) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    return -1;
  }
  int code;
  if (postBody) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(postBody)), strlen(postBody));
  } else {
    code = http.GET();
  }
  if (response && responseCap) {
    response[0] = '\0';
    if (code > 0) {
      String text = http.getString();
      snprintf(response, responseCap, "%s", text.c_str());
    }
  }
  http.end();
  return code;
}

// Returns the HTTP status, or <= 0 for a network or TLS error.
int httpRequest(const char *url, const char *postBody, char *response, size_t responseCap) {
  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;
    client.setInsecure();  // no certificate check, see the spec non-goals
    return httpRequestWith(client, url, postBody, response, responseCap);
  }
  WiFiClient client;
  return httpRequestWith(client, url, postBody, response, responseCap);
}

bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  for (int n = 0; n < 2; n++) {
    if (!activeConfig.ssid[n][0]) {
      continue;
    }
    WiFi.begin(activeConfig.ssid[n], activeConfig.pass[n][0] ? activeConfig.pass[n] : nullptr);
    uint32_t start = millis();
    while (millis() - start < WIFI_CONNECT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Online: joined %s\n", activeConfig.ssid[n]);
        return true;
      }
      if (paused) {
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    WiFi.disconnect();
    Serial.printf("Online: could not join %s\n", activeConfig.ssid[n]);
  }
  return false;
}

void wifiOff() {
  sntp_stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void syncClock() {
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  applyTimeZone(activeConfig.tz);  // configTime() rewrites TZ
  uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < NTP_WAIT_MS && !paused) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (cs::clockValid(now)) {
    storeFillTimes(now, millis());
  } else {
    Serial.println("Online: NTP sync failed");
  }
}

bool fetchOffsetTimeapi(int32_t &offset) {
  char ip[46];
  char url[128];
  if (httpRequest(cs::IP_LOOKUP_URL, nullptr, responseBuffer, sizeof(responseBuffer)) != 200 ||
      !cs::parsePlainIp(responseBuffer, ip, sizeof(ip)) || !cs::buildTimeapiUrl(ip, url, sizeof(url))) {
    return false;
  }
  return httpRequest(url, nullptr, responseBuffer, sizeof(responseBuffer)) == 200 &&
         cs::parseTimeapiOffset(responseBuffer, offset);
}

bool fetchOffsetIpApi(int32_t &offset) {
  return httpRequest(cs::IPAPI_URL, nullptr, responseBuffer, sizeof(responseBuffer)) == 200 &&
         cs::parseIpApiOffset(responseBuffer, offset);
}

void refreshOffset() {
  if (activeConfig.tz[0]) {
    return;
  }
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (!cs::clockValid(now) || (offsetKnown && now - offsetFetchedUnix < OFFSET_REFRESH_S)) {
    return;
  }
  int32_t offset = 0;
  if (fetchOffsetTimeapi(offset) || fetchOffsetIpApi(offset)) {
    offsetSeconds = offset;
    offsetKnown = true;
    offsetFetchedUnix = now;
    Serial.printf("Online: UTC offset %ld s\n", static_cast<long>(offset));
  } else {
    Serial.println("Online: time zone lookup failed");
  }
}

void logHeap(const char *what, int code) {
  Serial.printf("Online: %s -> HTTP %d, free heap %u, min %u, stack left %u\n",
                what,
                code,
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void addDelayedSuffix(cs::Message &msg) {
  cs::ClockNow clock = clockNow();
  char suffix[48];
  cs::formatDelayedSuffix(suffix,
                          sizeof(suffix),
                          clock.utcValid ? clock.unixUtc : 0,
                          msg.eventUnix,
                          clock.localValid,
                          clock.offsetS,
                          msg.boot == storeBootId(),
                          millis(),
                          msg.eventMs);
  cs::insertAfterFirstLine(msg.body, sizeof(msg.body), suffix);
}

Attempt resultOf(cs::NetResult result) {
  return result == cs::NetResult::RateLimited ? Attempt::RateLimited : Attempt::Failed;
}

Attempt sendAll() {
  char base[96];
  char topic[64];
  bool ntfy = cs::sinkType(activeConfig.url) == cs::SinkType::Ntfy;
  if (ntfy && !cs::ntfyTarget(activeConfig.url, base, sizeof(base), topic, sizeof(topic))) {
    Serial.println("Online: ntfy URL has no topic");
    return Attempt::Failed;
  }
  const char *postUrl = ntfy ? base : activeConfig.url;

  uint32_t id = 0;
  while (storePeekMessage(messageBuffer, id)) {
    if (paused) {
      return Attempt::Paused;
    }
    cs::MsgKind kind = static_cast<cs::MsgKind>(messageBuffer.kind);
    addDelayedSuffix(messageBuffer);
    uint32_t when = messageBuffer.eventUnix ? messageBuffer.eventUnix : static_cast<uint32_t>(time(nullptr));
    size_t len = ntfy ? cs::buildNtfyEnvelope(envelopeBuffer, sizeof(envelopeBuffer), topic, messageBuffer.title,
                                              messageBuffer.body, messageBuffer.priority, cs::kindTag(kind))
                      : cs::buildGenericEnvelope(envelopeBuffer, sizeof(envelopeBuffer), kind, messageBuffer.title,
                                                 messageBuffer.body, messageBuffer.priority, when, nullptr);
    if (len == 0) {
      storePopMessage(id);
      continue;
    }
    int code = httpRequest(postUrl, envelopeBuffer, nullptr, 0);
    logHeap(cs::kindName(kind), code);
    cs::NetResult result = cs::classifyHttp(code);
    if (result == cs::NetResult::Sent || result == cs::NetResult::Drop) {
      storePopMessage(id);
      continue;
    }
    return resultOf(result);
  }

  for (uint8_t batch = 0; batch < cs::BATCHES_PER_CONNECTION; batch++) {
    if (paused) {
      return Attempt::Paused;
    }
    uint32_t endSeq = 0;
    uint8_t count = storeReadBatch(batchRecords, cs::RECORDS_PER_BATCH, endSeq);
    if (count == 0) {
      storeMarkRecordsSent(endSeq);  // skips undatable records from an earlier boot
      break;
    }
    size_t len = cs::buildBatchJson(bodyBuffer, sizeof(bodyBuffer), batchRecords, count);
    if (len > 0) {
      if (ntfy) {
        len = cs::buildNtfyEnvelope(envelopeBuffer, sizeof(envelopeBuffer), topic, cs::kindTitle(cs::MsgKind::Batch),
                                    bodyBuffer, cs::kindPriority(cs::MsgKind::Batch), "");
      } else {
        char summary[24];
        snprintf(summary, sizeof(summary), "%u records", static_cast<unsigned>(count));
        len = cs::buildGenericEnvelope(envelopeBuffer, sizeof(envelopeBuffer), cs::MsgKind::Batch,
                                       cs::kindTitle(cs::MsgKind::Batch), summary,
                                       cs::kindPriority(cs::MsgKind::Batch), batchRecords[0].t, bodyBuffer);
      }
    }
    if (len == 0) {
      storeMarkRecordsSent(endSeq);
      continue;
    }
    int code = httpRequest(postUrl, envelopeBuffer, nullptr, 0);
    logHeap("batch", code);
    cs::NetResult result = cs::classifyHttp(code);
    if (result == cs::NetResult::Sent || result == cs::NetResult::Drop) {
      storeMarkRecordsSent(endSeq);
      continue;
    }
    return resultOf(result);
  }
  return Attempt::Done;
}

Attempt runAttempt() {
  busy = true;  // set before checking paused, see netlinkPause()
  if (paused) {
    busy = false;
    return Attempt::Paused;
  }
  if (suppressed || !activeConfig.enabled) {
    busy = false;
    return Attempt::Off;
  }
  uint32_t heap = ESP.getFreeHeap();
  if (heap < MIN_FREE_HEAP) {
    Serial.printf("Online: skipped, free heap %u\n", heap);
    busy = false;
    return Attempt::Failed;
  }

  Attempt result;
  if (!connectWifi()) {
    result = paused ? Attempt::Paused : Attempt::Failed;
    if (!paused) {
      onlineState = static_cast<uint8_t>(cs::OnlineState::NoWifi);
      lastAttemptUnix = static_cast<uint32_t>(time(nullptr));
    }
  } else {
    syncClock();
    refreshOffset();
    result = sendAll();
    if (result != Attempt::Paused) {
      onlineState = static_cast<uint8_t>(result == Attempt::Done ? cs::OnlineState::Sent : cs::OnlineState::Error);
      lastAttemptUnix = static_cast<uint32_t>(time(nullptr));
    }
  }
  wifiOff();
  busy = false;
  return result;
}

void netTaskMain(void *) {
  uint32_t nextPeriodicMs = millis() + FIRST_ATTEMPT_DELAY_MS;
  bool urgent = false;
  for (;;) {
    uint32_t now = millis();
    uint32_t waitMs;
    if (backoffActive && static_cast<int32_t>(backoffUntilMs - now) > 0) {
      waitMs = backoffUntilMs - now;
    } else if (urgent) {
      waitMs = 0;
    } else {
      int32_t untilPeriodic = static_cast<int32_t>(nextPeriodicMs - now);
      waitMs = untilPeriodic > 0 ? static_cast<uint32_t>(untilPeriodic) : 0;
    }
    if (waitMs > 0) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs)) > 0) {
        urgent = true;
      }
      continue;
    }

    ulTaskNotifyTake(pdTRUE, 0);  // notifications so far are covered by this attempt
    urgent = false;
    taskENTER_CRITICAL(&configMux);
    activeConfig = sharedConfig;
    taskEXIT_CRITICAL(&configMux);
    nextPeriodicMs = millis() + static_cast<uint32_t>(activeConfig.batchMin) * 60000UL;

    Attempt result = runAttempt();
    if (result == Attempt::Done) {
      failures = 0;
      backoffActive = false;
    } else if (result == Attempt::Failed || result == Attempt::RateLimited) {
      if (result == Attempt::RateLimited) {
        failures = cs::BACKOFF_MAX_FAILURES;
      } else if (failures < cs::BACKOFF_MAX_FAILURES) {
        failures++;
      }
      backoffActive = true;
      backoffUntilMs = millis() + cs::backoffSeconds(failures) * 1000UL;
      urgent = storeMessageCount() > 0;  // retry pending alerts when the backoff ends
      Serial.printf("Online: attempt failed, retry in %lu s\n", static_cast<unsigned long>(cs::backoffSeconds(failures)));
    } else if (result == Attempt::Paused) {
      urgent = true;
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

}  // namespace

cs::ClockNow clockNow() {
  cs::ClockNow clock;
  time_t now = time(nullptr);
  clock.unixUtc = now > 0 ? static_cast<uint32_t>(now) : 0;
  clock.utcValid = cs::clockValid(clock.unixUtc);
  if (!clock.utcValid) {
    return clock;
  }
  if (tzOverrideActive) {
    struct tm local;
    struct tm utc;
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
    clock.offsetS = cs::tmOffsetSeconds(local.tm_year, local.tm_yday,
                                        local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec, utc.tm_year,
                                        utc.tm_yday, utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec);
    clock.localValid = true;
  } else if (offsetKnown) {
    clock.offsetS = offsetSeconds;
    clock.localValid = true;
  }
  return clock;
}

void netlinkBegin() {
  if (!netTask) {
    xTaskCreate(netTaskMain, "netlink", NET_TASK_STACK, nullptr, NET_TASK_PRIORITY, &netTask);
  }
}

void netlinkConfigure(const NetConfig &config) {
  taskENTER_CRITICAL(&configMux);
  sharedConfig = config;
  taskEXIT_CRITICAL(&configMux);
  applyTimeZone(config.tz);
  tzOverrideActive = config.tz[0] != '\0';
  if (!config.enabled) {
    onlineState = static_cast<uint8_t>(cs::OnlineState::Off);
  } else if (onlineState == static_cast<uint8_t>(cs::OnlineState::Off)) {
    onlineState = static_cast<uint8_t>(cs::OnlineState::Waiting);
  }
}

void netlinkNotify() {
  if (netTask) {
    xTaskNotifyGive(netTask);
  }
}

void netlinkPause() {
  paused = true;
  uint32_t start = millis();
  while (busy && millis() - start < PAUSE_WAIT_MS) {
    delay(20);
  }
}

void netlinkResume() {
  paused = false;
}

void netlinkSetSuppressed(bool value) {
  suppressed = value;
}

OnlineStatus netlinkStatus() {
  OnlineStatus status;
  status.state = suppressed ? cs::OnlineState::Off : static_cast<cs::OnlineState>(onlineState);
  status.lastUnix = lastAttemptUnix;
  return status;
}
```

- [ ] **Step 3: Build**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`. If it fails with `CreateProcess: No such file or directory`, the command line hit the Windows limit: stop and report, don't remove other libraries. Then run `git checkout -- include/build_version.h`.

- [ ] **Step 4: Commit**

```bash
git add src/netlink.h src/netlink.cpp
git commit -m "Add netlink task for WiFi, NTP, offset lookup and HTTPS posting

WiFi client mode is only on while sending. Certificate checks are off
(nothing private is sent); revisit before an authenticated endpoint.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Telemetry sampling and triggers

**Files:**
- Modify: `src/telemetry.h`
- Modify: `src/telemetry.cpp`

**Interfaces:**
- Consumes: Tasks 1–5 (`OnlineSettings`, `SAMPLE_PERIOD_MS`, `SAMPLES_PER_MINUTE`, `RECORD_PERIOD_MS`, `NO_DAY`, `ClockNow`, `localDayNumber`, `localMinuteOfDay`, `DrainMonitor`, `DrainEvent`, `drainAddSample`, `drainCloseMinute`, `drainWindowAverage`, `Sample`, `socKnown`, `DayHistory`, `dayHistoryRoll`, `dayAddSample`, `dayHistoryFind`, `dayComplete`, `CapacityLearner`, `capacityUpdate`, `capacityShouldPersist`, `remainingAh`, `DaysEstimate`, `daysRemaining`, `morningDue`, `sunsetDue`, `Message`, `MsgKind`, `messageInit`, `kindName`, `TEST_BODY`, `buildAlertBody`, `buildClearBody`, `buildMorningBody`, `buildEveningBody`, `RecordBuilder`, `recordStart`, `recordAdd`, `recordClose`); Task 6 (store functions); Task 7 (`clockNow`, `netlinkNotify`).
- Produces (`telemetry.h`, called from `loop()` / `setup()` only):
  - `void telemetryBegin(Preferences &prefs)` — loads `cap_ah`, calls `storeBegin()`, loads `/days.bin`
  - `void telemetryConfigure(const cs::OnlineSettings &settings, bool enabled)`
  - `bool telemetrySampleDue(uint32_t nowMs)`
  - `void telemetryLoop(const cs::Sample &sample, bool demoMode, bool hasSolarKey, bool hasOrionKey)`
  - `bool telemetryQueueTest()`

Behaviour implemented here, every 10 s while enabled and not in demo mode:
- feed the open 5-minute `RecordBuilder` and the drain monitor;
- with valid local time: roll the day at local midnight and integrate the sample into today's `DayStats`;
- every 6th sample (60 s): close the drain minute, update the capacity (save `cap_ah` on a > 2 % change), queue an alert or all-clear;
- with valid local time: queue the morning report (needs a valid SOC) and the evening report, each once per local day (the sent day is kept in `/days.bin`);
- every 5 min: append the record to the cache and save `/days.bin`.
- Each queued message calls `netlinkNotify()`.

- [ ] **Step 1: Declare the loop API**

At the end of `src/telemetry.h`, after the line `bool storeSaveDays(const cs::DayHistory &days);`, add:

```cpp

// Sampling and triggers. Called from loop() only.
void telemetryBegin(Preferences &prefs);
void telemetryConfigure(const cs::OnlineSettings &settings, bool enabled);
bool telemetrySampleDue(uint32_t nowMs);
void telemetryLoop(const cs::Sample &sample, bool demoMode, bool hasSolarKey, bool hasOrionKey);
bool telemetryQueueTest();
```

- [ ] **Step 2: Include netlink**

In `src/telemetry.cpp` replace

```cpp
#include <freertos/semphr.h>
```

with

```cpp
#include <freertos/semphr.h>

#include "netlink.h"
```

- [ ] **Step 3: Add sampling and triggers**

At the end of `src/telemetry.cpp` (after the closing brace of `storeSaveDays`), add:

```cpp

// ---------------------------------------------------------------- sampling and triggers (loop() only)

namespace {

Preferences *telemetryPrefs = nullptr;
cs::OnlineSettings telemetrySettings;
bool telemetryEnabled = false;
cs::DrainMonitor drainMonitor;
cs::CapacityLearner capacityLearner;
float storedCapacityAh = NAN;
cs::DayHistory dayHistory;
cs::RecordBuilder recordBuilder;
bool recordOpen = false;
uint32_t recordStartMs = 0;
uint32_t lastSampleMs = 0;
uint8_t samplesThisMinute = 0;

void queueMessage(const cs::Message &msg) {
  if (storeEnqueueMessage(msg)) {
    Serial.printf("Online: queued %s\n", cs::kindName(static_cast<cs::MsgKind>(msg.kind)));
    netlinkNotify();
  }
}

void onMinute(const cs::Sample &sample, uint32_t unixOr0, uint32_t nowMs) {
  cs::DrainEvent event = cs::drainCloseMinute(drainMonitor, telemetrySettings.disAmps, telemetrySettings.disWinMin);

  if (sample.shuntValid && cs::capacityUpdate(capacityLearner, sample.soc, sample.consumedAh) &&
      cs::capacityShouldPersist(capacityLearner.capacityAh, storedCapacityAh)) {
    telemetryPrefs->putFloat("cap_ah", capacityLearner.capacityAh);
    storedCapacityAh = capacityLearner.capacityAh;
    Serial.printf("Online: capacity %.0f Ah saved\n", capacityLearner.capacityAh);
  }

  if (event == cs::DrainEvent::None) {
    return;
  }
  float average = cs::drainWindowAverage(drainMonitor, telemetrySettings.disWinMin);
  cs::Message msg;
  if (event == cs::DrainEvent::Trigger) {
    cs::messageInit(msg, cs::MsgKind::Alert, storeBootId(), unixOr0, nowMs);
    cs::buildAlertBody(msg.body, sizeof(msg.body), average, telemetrySettings.disWinMin, telemetrySettings.disAmps,
                       sample.soc, cs::remainingAh(capacityLearner, sample.soc));
  } else {
    cs::messageInit(msg, cs::MsgKind::Clear, storeBootId(), unixOr0, nowMs);
    cs::buildClearBody(msg.body, sizeof(msg.body), average, sample.soc);
  }
  queueMessage(msg);
}

void checkReports(const cs::Sample &sample, int32_t today, uint16_t minute, uint32_t unixOr0, uint32_t nowMs,
                  bool hasSolarKey, bool hasOrionKey) {
  if (cs::morningDue(dayHistory.morningSentDay, today, minute, telemetrySettings.reportHour) && sample.shuntValid &&
      cs::socKnown(sample.soc)) {
    float remaining = cs::remainingAh(capacityLearner, sample.soc);
    cs::DaysEstimate estimate = cs::daysRemaining(dayHistory, telemetrySettings.daysAvg, remaining, hasSolarKey);
    const cs::DayStats *yesterday = cs::dayHistoryFind(dayHistory, today - 1);
    if (yesterday && !cs::dayComplete(*yesterday)) {
      yesterday = nullptr;
    }
    cs::Message msg;
    cs::messageInit(msg, cs::MsgKind::Morning, storeBootId(), unixOr0, nowMs);
    if (cs::buildMorningBody(msg.body, sizeof(msg.body), sample.soc, remaining, estimate, yesterday, hasSolarKey,
                             hasOrionKey) > 0) {
      queueMessage(msg);
    }
    dayHistory.morningSentDay = today;
    storeSaveDays(dayHistory);
  }

  if (cs::sunsetDue(dayHistory.eveningSentDay, dayHistory.current, today, minute, telemetrySettings.eveningHour)) {
    cs::Message msg;
    cs::messageInit(msg, cs::MsgKind::Evening, storeBootId(), unixOr0, nowMs);
    if (cs::buildEveningBody(msg.body, sizeof(msg.body), dayHistory.current) > 0) {
      queueMessage(msg);
    }
    dayHistory.eveningSentDay = today;
    storeSaveDays(dayHistory);
  }
}

}  // namespace

void telemetryBegin(Preferences &prefs) {
  telemetryPrefs = &prefs;
  float stored = prefs.getFloat("cap_ah", NAN);
  capacityLearner.capacityAh = (std::isfinite(stored) && stored > 0.0f) ? stored : NAN;
  storedCapacityAh = capacityLearner.capacityAh;
  storeBegin();
  if (!storeLoadDays(dayHistory)) {
    dayHistory = cs::DayHistory();
  }
}

void telemetryConfigure(const cs::OnlineSettings &settings, bool enabled) {
  telemetrySettings = settings;
  telemetryEnabled = enabled;
}

bool telemetrySampleDue(uint32_t nowMs) {
  return lastSampleMs == 0 || nowMs - lastSampleMs >= cs::SAMPLE_PERIOD_MS;
}

void telemetryLoop(const cs::Sample &sample, bool demoMode, bool hasSolarKey, bool hasOrionKey) {
  uint32_t nowMs = millis();
  uint32_t dtS = lastSampleMs == 0 ? cs::SAMPLE_PERIOD_MS / 1000 : (nowMs - lastSampleMs + 500) / 1000;
  if (dtS > 30) {
    dtS = 30;  // the loop was blocked; do not integrate across the gap
  }
  lastSampleMs = nowMs;
  if (!telemetryEnabled || demoMode) {
    recordOpen = false;
    samplesThisMinute = 0;
    return;
  }

  cs::ClockNow clock = clockNow();
  uint32_t unixOr0 = clock.utcValid ? clock.unixUtc : 0;

  if (!recordOpen) {
    cs::recordStart(recordBuilder, unixOr0, nowMs, storeBootId());
    recordStartMs = nowMs;
    recordOpen = true;
  }
  cs::recordAdd(recordBuilder, sample);
  if (sample.shuntValid) {
    cs::drainAddSample(drainMonitor, sample.currentA);
  }

  int32_t today = cs::NO_DAY;
  uint16_t minute = 0;
  if (clock.localValid) {
    today = cs::localDayNumber(clock.unixUtc, clock.offsetS);
    minute = cs::localMinuteOfDay(clock.unixUtc, clock.offsetS);
    if (dayHistory.current.day != today) {
      cs::dayHistoryRoll(dayHistory, today);
      storeSaveDays(dayHistory);
    }
    cs::dayAddSample(dayHistory.current, sample, dtS, minute);
  }

  if (++samplesThisMinute >= cs::SAMPLES_PER_MINUTE) {
    samplesThisMinute = 0;
    onMinute(sample, unixOr0, nowMs);
  }

  if (clock.localValid) {
    checkReports(sample, today, minute, unixOr0, nowMs, hasSolarKey, hasOrionKey);
  }

  if (nowMs - recordStartMs >= cs::RECORD_PERIOD_MS) {
    storeAppendRecord(cs::recordClose(recordBuilder));
    cs::recordStart(recordBuilder, unixOr0, nowMs, storeBootId());
    recordStartMs = nowMs;
    if (clock.localValid) {
      storeSaveDays(dayHistory);
    }
  }
}

bool telemetryQueueTest() {
  cs::ClockNow clock = clockNow();
  cs::Message msg;
  cs::messageInit(msg, cs::MsgKind::Test, storeBootId(), clock.utcValid ? clock.unixUtc : 0, millis());
  snprintf(msg.body, sizeof(msg.body), "%s", cs::TEST_BODY);
  if (!storeEnqueueMessage(msg)) {
    return false;
  }
  netlinkNotify();
  return true;
}
```

- [ ] **Step 4: Build**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`. Then run `git checkout -- include/build_version.h`.

- [ ] **Step 5: Run every native suite**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`
Expected: `89 test cases: 89 succeeded`.

- [ ] **Step 6: Commit**

```bash
git add src/telemetry.h src/telemetry.cpp
git commit -m "Add telemetry sampling, drain alerts, daily reports and 5-minute records

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: main.cpp integration

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: Task 1 (`OnlineSettings`, `sanitiseDisAmps`, `sanitiseDisWinMin`, `sanitiseReportHour`, `sanitiseEveningHour`, `sanitiseBatchMin`, `sanitiseDaysAvg`, `onlineConfigured`, `ClockNow`, `clockValid`, `localMinuteOfDay`); Task 2 (`Sample`); Task 3 (`onlineStatusText`); Task 4 (`SinkType`, `sinkType`, `ntfyTarget`); Task 7 (`NetConfig`, `OnlineStatus`, `netlinkBegin`, `netlinkConfigure`, `netlinkPause`, `netlinkResume`, `netlinkSetSuppressed`, `netlinkStatus`, `clockNow`); Task 8 (`telemetryBegin`, `telemetryConfigure`, `telemetrySampleDue`, `telemetryLoop`, `telemetryQueueTest`).
- Produces (file-static in `main.cpp`): globals `onlineWifiSsid[2]`, `onlineWifiPass[2]`, `onlinePostUrl`, `onlineTzOverride`, `onlineSettings`; functions `onlineEnabled()`, `applyOnlineConfig()`, `onlineStatusLine()`, `htmlAttr()`, `appendWifiFields()`, `appendSelect()`, `appendOnlineSection()`, `readIntArg()`, `putOrRemove()`, `saveOnlineSettings()`, `handleOnlineTest()`, `telemetrySample()`; route `POST /online-test`.

- [ ] **Step 1: Includes**

Replace

```cpp
#include "chargescreen_logic.h"
```

with

```cpp
#include "chargescreen_logic.h"
#include "netlink.h"
#include "telemetry.h"
```

- [ ] **Step 2: Globals**

Replace

```cpp
static uint16_t cycleSeconds = 0;
static uint32_t lastPageChangeMs = 0;
```

with

```cpp
static uint16_t cycleSeconds = 0;
static uint32_t lastPageChangeMs = 0;
static String onlineWifiSsid[2];
static String onlineWifiPass[2];
static String onlinePostUrl;
static String onlineTzOverride;
static cs::OnlineSettings onlineSettings;
```

- [ ] **Step 3: Online helpers**

Replace

```cpp
static cs::PageAvailability pageAvailability() {
  return cs::PageAvailability{demoModeEnabled, storedSolarKey.length() > 0, storedOrionKey.length() > 0};
}
```

with

```cpp
static cs::PageAvailability pageAvailability() {
  return cs::PageAvailability{demoModeEnabled, storedSolarKey.length() > 0, storedOrionKey.length() > 0};
}

static bool onlineEnabled() {
  return cs::onlineConfigured(onlinePostUrl.c_str(), onlineWifiSsid[0].c_str(), onlineWifiSsid[1].c_str());
}

static void applyOnlineConfig() {
  NetConfig config = {};
  for (int n = 0; n < 2; n++) {
    snprintf(config.ssid[n], sizeof(config.ssid[n]), "%s", onlineWifiSsid[n].c_str());
    snprintf(config.pass[n], sizeof(config.pass[n]), "%s", onlineWifiPass[n].c_str());
  }
  snprintf(config.url, sizeof(config.url), "%s", onlinePostUrl.c_str());
  snprintf(config.tz, sizeof(config.tz), "%s", onlineTzOverride.c_str());
  config.batchMin = onlineSettings.batchMin;
  config.enabled = onlineEnabled();
  netlinkConfigure(config);
  telemetryConfigure(onlineSettings, config.enabled);
}

static String onlineStatusLine() {
  if (!onlineEnabled() || demoModeEnabled) {
    return "Off";
  }
  OnlineStatus status = netlinkStatus();
  cs::ClockNow clock = clockNow();
  int16_t minute = -1;
  if (cs::clockValid(status.lastUnix) && clock.localValid) {
    minute = static_cast<int16_t>(cs::localMinuteOfDay(status.lastUnix, clock.offsetS));
  }
  char text[16];
  cs::onlineStatusText(status.state, minute, text, sizeof(text));
  return text;
}
```

- [ ] **Step 4: Load settings at boot**

In `loadStoredSecrets`, replace

```cpp
  cycleSeconds = cs::sanitiseCycleSeconds(secrets.getUShort("cycle_secs", 0));
```

with

```cpp
  cycleSeconds = cs::sanitiseCycleSeconds(secrets.getUShort("cycle_secs", 0));
  onlineWifiSsid[0] = secrets.getString("wifi1_ssid", "");
  onlineWifiPass[0] = secrets.getString("wifi1_pass", "");
  onlineWifiSsid[1] = secrets.getString("wifi2_ssid", "");
  onlineWifiPass[1] = secrets.getString("wifi2_pass", "");
  onlinePostUrl = secrets.getString("post_url", "");
  onlineTzOverride = secrets.getString("tz_override", "");
  onlineSettings.disAmps = cs::sanitiseDisAmps(secrets.getFloat("dis_amps", 4.0f));
  onlineSettings.disWinMin = cs::sanitiseDisWinMin(secrets.getUShort("dis_win_min", 45));
  onlineSettings.reportHour = cs::sanitiseReportHour(secrets.getUChar("report_hour", 10));
  onlineSettings.eveningHour = cs::sanitiseEveningHour(secrets.getUChar("evening_hour", 20));
  onlineSettings.batchMin = cs::sanitiseBatchMin(secrets.getUChar("batch_min", 30));
  onlineSettings.daysAvg = cs::sanitiseDaysAvg(secrets.getUChar("days_avg", 3));
```

- [ ] **Step 5: Setup-page helpers**

Replace

```cpp
static String maskedPasswordText(const String &password) {
  return password.length() ? "saved" : "default 123123";
}
```

with

```cpp
static String maskedPasswordText(const String &password) {
  return password.length() ? "saved" : "default 123123";
}

static String htmlAttr(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') {
      out += F("&amp;");
    } else if (c == '\'') {
      out += F("&#39;");
    } else if (c == '"') {
      out += F("&quot;");
    } else if (c == '<') {
      out += F("&lt;");
    } else if (c == '>') {
      out += F("&gt;");
    } else {
      out += c;
    }
  }
  return out;
}

static void appendWifiFields(String &html, int number, const String &ssid, const String &pass) {
  char buffer[320];
  snprintf(buffer, sizeof(buffer),
           "<label for='wifi%d_ssid'>WiFi %d name</label><input id='wifi%d_ssid' name='wifi%d_ssid' maxlength='32' "
           "autocomplete='off' value='",
           number, number, number, number);
  html += buffer;
  html += htmlAttr(ssid);
  snprintf(buffer, sizeof(buffer),
           "'><label for='wifi%d_pass'>WiFi %d password</label><input id='wifi%d_pass' name='wifi%d_pass' "
           "type='password' maxlength='64' autocomplete='new-password' placeholder='%s'>",
           number, number, number, number, pass.length() ? "saved" : "not set");
  html += buffer;
}

static void appendSelect(String &html, const char *name, const char *label, const uint8_t *options, size_t count,
                         uint8_t selected, const char *suffix) {
  html += F("<label for='");
  html += name;
  html += F("'>");
  html += label;
  html += F("</label><select id='");
  html += name;
  html += F("' name='");
  html += name;
  html += F("'>");
  for (size_t i = 0; i < count; i++) {
    html += F("<option value='");
    html += options[i];
    html += F("'");
    html += options[i] == selected ? F(" selected") : F("");
    html += F(">");
    html += options[i];
    html += suffix;
    html += F("</option>");
  }
  html += F("</select>");
}

static void appendOnlineSection(String &html) {
  static const uint8_t reportHours[] = {4, 5, 6, 7, 8, 9, 10, 11, 12};
  static const uint8_t eveningHours[] = {16, 17, 18, 19, 20, 21, 22};
  static const uint8_t batchOptions[] = {15, 30, 60};
  static const uint8_t dayOptions[] = {1, 2, 3, 4, 5, 6, 7};
  html += F("<h2>Online</h2>");
  html += F("<p class='hint'>With a URL and a WiFi network set, ChargeScreen joins WiFi only to send drain alerts, a morning battery report, an evening solar report and a silent data batch. Use an ntfy topic such as https://ntfy.sh/your_topic, or your own endpoint. Nothing is sent in demo mode. Status: ");
  html += onlineStatusLine();
  html += F("</p>");
  appendWifiFields(html, 1, onlineWifiSsid[0], onlineWifiPass[0]);
  appendWifiFields(html, 2, onlineWifiSsid[1], onlineWifiPass[1]);
  html += F("<label for='post_url'>Send to URL</label><input id='post_url' name='post_url' maxlength='191' autocomplete='off' placeholder='https://ntfy.sh/your_topic' value='");
  html += htmlAttr(onlinePostUrl);
  html += F("'><label for='tz_override'>Time zone override</label><input id='tz_override' name='tz_override' maxlength='63' autocomplete='off' placeholder='automatic, e.g. CET-1CEST,M3.5.0,M10.5.0/3' value='");
  html += htmlAttr(onlineTzOverride);
  html += F("'><div class='grid'><label for='dis_amps'>Drain limit (A)</label><label for='dis_win_min'>Drain window (min)</label>");
  html += F("<input id='dis_amps' name='dis_amps' type='number' min='0.5' max='50' step='0.1' value='");
  html += String(onlineSettings.disAmps, 1);
  html += F("'><input id='dis_win_min' name='dis_win_min' type='number' min='15' max='120' step='1' value='");
  html += onlineSettings.disWinMin;
  html += F("'></div>");
  appendSelect(html, "report_hour", "Morning report hour", reportHours, sizeof(reportHours), onlineSettings.reportHour, ":00");
  appendSelect(html, "evening_hour", "Evening report hour", eveningHours, sizeof(eveningHours), onlineSettings.eveningHour, ":00");
  appendSelect(html, "batch_min", "Batch interval", batchOptions, sizeof(batchOptions), onlineSettings.batchMin, " min");
  appendSelect(html, "days_avg", "Days to average", dayOptions, sizeof(dayOptions), onlineSettings.daysAvg, "");
}
```

- [ ] **Step 6: Status messages for the test button**

In `settingsStatusMessage` replace

```cpp
  if (code == "capture-cancelled") {
    return "BLE capture cancelled. No CSV was saved.";
  }
  return "";
```

with

```cpp
  if (code == "capture-cancelled") {
    return "BLE capture cancelled. No CSV was saved.";
  }
  if (code == "test-queued") {
    return "Test message queued. It is sent once this hotspot turns off.";
  }
  if (code == "test-off") {
    return "Online sending is off. Save a URL and a WiFi network, and turn off demo mode.";
  }
  return "";
```

- [ ] **Step 7: Online section and test button on the setup page**

In `settingsPageHtml` replace

```cpp
  html.reserve(11000);
```

with

```cpp
  html.reserve(14000);
```

replace

```cpp
  html += F("<h2>Settings</h2>");
```

with

```cpp
  appendOnlineSection(html);
  html += F("<h2>Settings</h2>");
```

and replace

```cpp
  html += F("<button type='submit'>Save Settings</button></form>");
```

with

```cpp
  html += F("<button type='submit'>Save Settings</button></form>");
  html += F("<form method='post' action='/online-test' data-working='Queueing test message...'>");
  html += F("<button class='secondary' type='submit'>Send test message</button></form>");
```

- [ ] **Step 8: Save handling**

Replace

```cpp
static void handleSettingsSave() {
  noteSettingsServerActivity();
```

with

```cpp
// Empty field: keep the current value. Returns false when out of range.
static bool readIntArg(const char *name, int minValue, int maxValue, int &out) {
  String text = settingsServer.arg(name);
  text.trim();
  if (!text.length()) {
    return true;
  }
  int value = text.toInt();
  if (value < minValue || value > maxValue) {
    return false;
  }
  out = value;
  return true;
}

static void putOrRemove(const char *key, const String &value) {
  if (value.length()) {
    secrets.putString(key, value);
  } else {
    secrets.remove(key);
  }
}

// Returns false if any Online field was invalid; valid fields are still saved.
static bool saveOnlineSettings() {
  bool ok = true;
  for (int n = 0; n < 2; n++) {
    char ssidKey[12];
    char passKey[12];
    snprintf(ssidKey, sizeof(ssidKey), "wifi%d_ssid", n + 1);
    snprintf(passKey, sizeof(passKey), "wifi%d_pass", n + 1);
    String ssid = settingsServer.arg(ssidKey);
    ssid.trim();
    String pass = settingsServer.arg(passKey);
    if (ssid.length() > 32 || pass.length() > 64) {
      ok = false;
      continue;
    }
    bool ssidChanged = ssid != onlineWifiSsid[n];
    if (ssidChanged) {
      onlineWifiSsid[n] = ssid;
      putOrRemove(ssidKey, ssid);
    }
    if (pass.length()) {
      onlineWifiPass[n] = pass;
      secrets.putString(passKey, pass);
    } else if (ssidChanged) {
      // A new network name with a blank password means an open network.
      onlineWifiPass[n] = "";
      secrets.remove(passKey);
    }
  }

  String url = settingsServer.arg("post_url");
  url.trim();
  char base[96];
  char topic[64];
  bool urlOk = url.length() == 0 ||
               ((url.startsWith("https://") || url.startsWith("http://")) && url.length() <= 191 &&
                (cs::sinkType(url.c_str()) != cs::SinkType::Ntfy ||
                 cs::ntfyTarget(url.c_str(), base, sizeof(base), topic, sizeof(topic))));
  if (!urlOk) {
    ok = false;
  } else if (url != onlinePostUrl) {
    onlinePostUrl = url;
    putOrRemove("post_url", url);
  }

  String tz = settingsServer.arg("tz_override");
  tz.trim();
  if (tz.length() > 63) {
    ok = false;
  } else if (tz != onlineTzOverride) {
    onlineTzOverride = tz;
    putOrRemove("tz_override", tz);
  }

  String ampsText = settingsServer.arg("dis_amps");
  ampsText.trim();
  if (ampsText.length()) {
    float amps = ampsText.toFloat();
    if (amps >= 0.5f && amps <= 50.0f) {
      if (amps != onlineSettings.disAmps) {
        onlineSettings.disAmps = amps;
        secrets.putFloat("dis_amps", amps);
      }
    } else {
      ok = false;
    }
  }

  int value = onlineSettings.disWinMin;
  if (readIntArg("dis_win_min", 15, 120, value)) {
    if (value != onlineSettings.disWinMin) {
      onlineSettings.disWinMin = static_cast<uint16_t>(value);
      secrets.putUShort("dis_win_min", onlineSettings.disWinMin);
    }
  } else {
    ok = false;
  }

  value = onlineSettings.reportHour;
  if (readIntArg("report_hour", 4, 12, value)) {
    if (value != onlineSettings.reportHour) {
      onlineSettings.reportHour = static_cast<uint8_t>(value);
      secrets.putUChar("report_hour", onlineSettings.reportHour);
    }
  } else {
    ok = false;
  }

  value = onlineSettings.eveningHour;
  if (readIntArg("evening_hour", 16, 22, value)) {
    if (value != onlineSettings.eveningHour) {
      onlineSettings.eveningHour = static_cast<uint8_t>(value);
      secrets.putUChar("evening_hour", onlineSettings.eveningHour);
    }
  } else {
    ok = false;
  }

  value = onlineSettings.batchMin;
  if (readIntArg("batch_min", 15, 60, value) && cs::sanitiseBatchMin(value) == value) {
    if (value != onlineSettings.batchMin) {
      onlineSettings.batchMin = static_cast<uint8_t>(value);
      secrets.putUChar("batch_min", onlineSettings.batchMin);
    }
  } else {
    ok = false;
  }

  value = onlineSettings.daysAvg;
  if (readIntArg("days_avg", 1, 7, value)) {
    if (value != onlineSettings.daysAvg) {
      onlineSettings.daysAvg = static_cast<uint8_t>(value);
      secrets.putUChar("days_avg", onlineSettings.daysAvg);
    }
  } else {
    ok = false;
  }

  applyOnlineConfig();
  return ok;
}

static void handleSettingsSave() {
  noteSettingsServerActivity();
```

Then, in `handleSettingsSave`, replace

```cpp
  demoBatterySoc = constrain(settingsServer.arg("demo_soc").toFloat(), 0.0f, 100.0f);
```

with

```cpp
  if (!saveOnlineSettings()) {
    invalid = true;
  }

  demoBatterySoc = constrain(settingsServer.arg("demo_soc").toFloat(), 0.0f, 100.0f);
```

- [ ] **Step 9: Test button handler and route**

Replace

```cpp
static void handleCaptureDownload() {
  noteSettingsServerActivity();
```

with

```cpp
static void handleOnlineTest() {
  noteSettingsServerActivity();
  if (!onlineEnabled() || demoModeEnabled || !telemetryQueueTest()) {
    redirectToSettingsPage("test-off");
    return;
  }
  redirectToSettingsPage("test-queued");
}

static void handleCaptureDownload() {
  noteSettingsServerActivity();
```

In `startSettingsServer` replace

```cpp
  settingsServer.on("/clear-keys", HTTP_POST, handleSettingsClearKeys);
```

with

```cpp
  settingsServer.on("/clear-keys", HTTP_POST, handleSettingsClearKeys);
  settingsServer.on("/online-test", HTTP_POST, handleOnlineTest);
```

- [ ] **Step 10: Pause netlink while the settings hotspot runs**

In `startSettingsServer` replace

```cpp
  WiFi.mode(WIFI_AP);
  bool started = strlen(SETTINGS_AP_PASSWORD) >= 8
                     ? WiFi.softAP(SETTINGS_AP_SSID, SETTINGS_AP_PASSWORD)
                     : WiFi.softAP(SETTINGS_AP_SSID);

  if (!started) {
    settingsMessage = "WiFi failed";
```

with

```cpp
  settingsMessage = "Starting WiFi...";
  drawCurrentPage(true);
  netlinkPause();  // waits for any upload in progress to finish or abort

  WiFi.mode(WIFI_AP);
  bool started = strlen(SETTINGS_AP_PASSWORD) >= 8
                     ? WiFi.softAP(SETTINGS_AP_SSID, SETTINGS_AP_PASSWORD)
                     : WiFi.softAP(SETTINGS_AP_SSID);

  if (!started) {
    netlinkResume();
    settingsMessage = "WiFi failed";
```

In `stopSettingsServer` replace

```cpp
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  settingsServerActive = false;
```

with

```cpp
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  settingsServerActive = false;
  netlinkResume();
```

- [ ] **Step 11: Info-screen line**

In `drawSettingsInfoPage` replace

```cpp
  drawInfoLine(84, "WiFi", settingsServerActive ? "On" : "Off");
  drawInfoLine(104, "Demo", demoModeEnabled ? "On" : "Off");
  drawInfoLine(124, "Rotation", String(screenRotationDegrees));
  drawInfoLine(144, "Battery", batteryModeText());
  drawInfoLine(164, "Label/Grid", String(showValueLabels ? "On/" : "Off/") + (showValueGrid ? "On" : "Off"));
  drawInfoLine(184, "Keys", storedVictronKey.length() || storedSolarKey.length() || storedOrionKey.length() ? "saved" : "-");
```

with

```cpp
  drawInfoLine(78, "WiFi", settingsServerActive ? "On" : "Off");
  drawInfoLine(96, "Demo", demoModeEnabled ? "On" : "Off");
  drawInfoLine(114, "Rotation", String(screenRotationDegrees));
  drawInfoLine(132, "Battery", batteryModeText());
  drawInfoLine(150, "Label/Grid", String(showValueLabels ? "On/" : "Off/") + (showValueGrid ? "On" : "Off"));
  drawInfoLine(168, "Keys", storedVictronKey.length() || storedSolarKey.length() || storedOrionKey.length() ? "saved" : "-");
  drawInfoLine(186, "Online", onlineStatusLine());
```

(Rows move up 6 px and are 18 px apart so the seventh row ends above the Back button at y 204.)

- [ ] **Step 12: Sample snapshot**

`victronStats`, `solarStats` and `orionStats` are written by the NimBLE host task and copied without a lock by `displayStats()`, `displaySolarStats()` and `displayOrionStats()` for the dashboard. Telemetry copies them the same way, through the same functions, with the dashboard's 10 s "Signal lost" freshness rule.

Replace

```cpp
static void updateAutoCycle() {
  if (!screenAwake || cycleSeconds == 0 || captureActive || captureReadyScreenActive ||
```

with

```cpp
// The same struct copies the dashboard makes (displayStats() and friends),
// with the same 10 s freshness rule as the "Signal lost" status.
static cs::Sample telemetrySample() {
  cs::Sample sample;
  BatteryStats battery = displayStats();
  if (battery.valid && !ecoWorthyBatteryMode && static_cast<int32_t>(millis() - lastVictronDecodeMs) <= 10000) {
    sample.shuntValid = true;
    sample.currentA = battery.current;
    sample.soc = battery.soc;
    sample.consumedAh = isnan(battery.consumedAh) ? NAN : fabsf(battery.consumedAh);
  }
  SolarStats solar = displaySolarStats();
  if (storedSolarKey.length() && solar.valid && static_cast<int32_t>(millis() - lastSolarDecodeMs) <= 10000) {
    sample.solarValid = true;
    sample.solarBatA = solar.batteryCurrent;
    sample.pvW = solar.pvPower;
  }
  OrionStats orion = displayOrionStats();
  if (storedOrionKey.length() && orion.valid && static_cast<int32_t>(millis() - lastOrionDecodeMs) <= 10000) {
    sample.orionValid = true;
    sample.orionOutA = orion.outputCurrent;
  }
  return sample;
}

static void updateAutoCycle() {
  if (!screenAwake || cycleSeconds == 0 || captureActive || captureReadyScreenActive ||
```

- [ ] **Step 13: Start telemetry and netlink in `setup()`**

Replace

```cpp
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS init failed");
  }
  drawCurrentPage(true);
```

with

```cpp
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS init failed");
  }
  telemetryBegin(secrets);
  applyOnlineConfig();
  netlinkBegin();
  drawCurrentPage(true);
```

- [ ] **Step 14: Feed telemetry from `loop()`, suppressed in demo mode**

Replace

```cpp
  updateAutoCycle();
  updateScreenTimeout();
```

with

```cpp
  updateAutoCycle();
  updateScreenTimeout();
  netlinkSetSuppressed(demoModeEnabled);
  if (telemetrySampleDue(millis())) {
    telemetryLoop(telemetrySample(), demoModeEnabled, storedSolarKey.length() > 0, storedOrionKey.length() > 0);
  }
```

- [ ] **Step 15: Verify the wiring**

Run: `git grep -n -E "telemetryBegin|applyOnlineConfig\(\);|netlinkBegin|netlinkPause|netlinkResume|netlinkSetSuppressed|telemetryLoop|/online-test|\"Online\"" -- src/main.cpp`
Expected: exactly 12 lines: the `/online-test` form, `applyOnlineConfig();` in `saveOnlineSettings`, `netlinkPause();`, `netlinkResume();` in the `!started` branch, the `/online-test` route, `netlinkResume();` in `stopSettingsServer`, the `"Online"` info line, `telemetryBegin(secrets);`, `applyOnlineConfig();` and `netlinkBegin();` in `setup`, `netlinkSetSuppressed(demoModeEnabled);` and `telemetryLoop(...)` in `loop`.

- [ ] **Step 16: Build and run tests**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`. In the dry run made while writing this plan the result was `RAM: 18.4% (used 60412 bytes from 327680 bytes)` and `Flash: 45.1% (used 1417572 bytes from 3145728 bytes)`; small differences are fine. Then run `git checkout -- include/build_version.h`.
Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`
Expected: `89 test cases: 89 succeeded`.

- [ ] **Step 17: Commit**

```bash
git add src/main.cpp
git commit -m "Wire online alerts and reports into settings, setup page and loop

Adds the Online setup section, a Send test message button and an Online
line on the info screen. Nothing is sampled or sent in demo mode, and
netlink pauses while the settings hotspot runs.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: README and device verification

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: documentation and the on-device checklist result.

- [ ] **Step 1: Update README**

In `README.md`, directly before the line `## Flashing`, add:

```markdown
## Online reports

ChargeScreen can join WiFi only when it has something to send, and push to an HTTP endpoint:

- a high-drain alert when the battery discharges faster than a limit over a rolling window (default 4.0 A over 45 minutes), and an all-clear when it is back to normal;
- a morning battery report (default 10:00) with an estimate of the days remaining;
- an evening solar report (default 20:00, after production ends, 23:00 at the latest);
- a silent data batch of 5-minute records (default every 30 minutes).

Set it up in the **Online** section of the setup page: up to two WiFi networks, the URL, and optionally a POSIX time zone such as `CET-1CEST,M3.5.0,M10.5.0/3` (otherwise the time zone is looked up from the IP address once a day). Use an ntfy topic URL such as `https://ntfy.sh/your_topic` and subscribe to it in the ntfy app, or your own endpoint, which receives JSON (`{"v":1,"kind":...,"title":...,"message":...,"priority":...,"time":...,"data":{...}}`). **Send test message** checks the setup once the hotspot turns off. The info screen shows `Online: Off`, `Waiting`, `No WiFi`, `Sent HH:MM` or `Error HH:MM`.

While offline, up to 24 hours of records and 16 alerts or reports are kept on the device and sent later; late messages say when they happened. Battery capacity is learned from the shunt, so the remaining Ah and days estimates appear after a while. Nothing is sent in demo mode, and TLS certificates are not checked, so don't use it with an endpoint that needs a password.
```

- [ ] **Step 2: Final checks**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); pio test -e native`
Expected: `89 test cases: 89 succeeded`.
Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`. Then run `git checkout -- include/build_version.h`.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "Document online alerts, reports and data batches

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 4: Flash and verify on the device (user, device connected over USB)**

Run: `pio run -e esp32-c3-round-dashboard -t upload` then `pio device monitor`. Then run `git checkout -- include/build_version.h`.

Setup: on the setup page enter WiFi 1, `https://ntfy.sh/ghibli_harmony` as the URL, save, and subscribe to `ghibli_harmony` in the ntfy app. Serial lines to watch: `Online: store ready`, `Online: joined ...`, `Online: UTC offset ... s`, `Online: <kind> -> HTTP 200, free heap ..., min ..., stack left ...`.

Checklist (report PASS/FAIL for each):
1. With a URL and WiFi set, **Send test message** arrives on ntfy (`ChargeScreen test` / `Online sending works.`) after the hotspot turns off.
2. A batch arrives silently (priority 1) every 30 min.
3. With WiFi off for 1 h then back on, the backlog arrives as hourly batches (12 records each, at most 6 per connection).
4. The time and offset are correct: the serial `UTC offset` matches local time, and the report times match `report_hour` and `evening_hour`.
5. A forced drain (e.g. `dis_amps` 0.5 and `dis_win_min` 15 with a load on) sends a `High battery drain` alert, and `Drain back to normal` 15 min after the window average drops below 0.4 A.
6. The morning and evening reports arrive.
7. The UI doesn't freeze during sends (swipe and tap while `Online:` lines are being logged).
8. BLE decoding continues during sends (the `Victron shunt ... decoded` lines keep coming).
9. Free heap stays above 40 KB during a send: every `free heap` and `min` value in the `Online: ... -> HTTP` lines is above 40960.

- [ ] **Step 5: Push**

Only after the user confirms the checklist. Run: `git push origin firmware`
Expected: branch updated on `istrasoft/ChargeScreenX`.

---

## Self-review against the spec

**Spec coverage:**

| Spec section | Task |
|---|---|
| Goal: ntfy target and generic endpoint by URL only | 4 (`sinkType`, envelopes), 7 (`sendAll`) |
| Non-goals: 24 h cache + 7 days, no voltage, no TLS check, 2 WiFi, learned capacity, no demo sending | 1 (`RECORD_RING_SIZE`), 2 (`DAY_HISTORY_DAYS`), 3 (texts use % and Ah), 7 (`setInsecure`), 9 (two WiFi fields, demo suppression) |
| Messages: alert, all-clear, morning, evening, batch; titles, priorities, tags | 3 (`kindTitle`, `kindPriority`, `kindTag`, builders), 4 (batch JSON) |
| Omission rules (solar/Orion parts, single figure, Ah left, estimate line, solar covers, PV 5 W, day count wording, `more than 30 days`) | 3 (`test_morning_omissions`, `test_morning_day_counts_and_limits`), 2 (`sunsetDue` needs production) |
| Triggers: high drain (60 s minute averages, window, limit, 0.8× for 15 min, 60 min cooldown) | 1 (`DrainMonitor`), 8 (`onMinute`) |
| Triggers: morning, evening (16–22, 30 min quiet, 23:00), batch interval | 2 (`morningDue`, `sunsetDue`), 7 (batch timer), 8 (`checkReports`) |
| Offline: cache limits 288 / 16, oldest first, delayed suffix, morning replacement, 12 per batch, 6 per connection | 3 (suffix), 5 (`RingIndex`, `QueueIndex`), 6 (store), 7 (`sendAll`) |
| Time: NTP servers, IP offset lookup daily, confirmed formats, `tz_override`, behaviour before first sync | 0 (curl), 5 (parsers with real fixtures, `tmOffsetSeconds`, fill times), 7 (`syncClock`, `refreshOffset`, `clockNow`), 8 (reports need local time; alerts don't) |
| Battery capacity: formula, conditions, α = 0.1, `cap_ah` > 2 %, remaining Ah | 2 (`CapacityLearner`), 8 (`telemetryBegin`, `onMinute`) |
| Days remaining: `DayStats`, rectangle rule, missing data, `days_avg`, both figures | 2 |
| Settings: Online section, all NVS keys, feature-off rule, blank password keeps, test button | 1 (`onlineConfigured`), 9 (Steps 4, 5, 7, 8, 9) |
| Info screen line `Online` | 3 (`onlineStatusText`), 9 (Step 11) |
| Data formats: sink type, ntfy JSON, generic JSON, batch body, field omission, rounding, 4096-byte limit | 4 |
| Network results: 2xx, 429/5xx, other 4xx, network errors | 5 (`classifyHttp`), 7 |
| Architecture: header contents, telemetry files and write cadence, netlink task, mutex, status struct, backoff | 1–5, 6, 7, 8 |
| Changes in main.cpp (loop, setup, setup page, info line, hotspot pause, demo) | 9 |
| Testing: native suites, device checks | 1–5, 10 |

**Placeholder scan:** every code step contains the complete code; every `main.cpp` edit quotes the exact existing anchor text and the full replacement; every command has its expected output.

**Name consistency:** names in each task's Interfaces block match their definitions and uses: `cs::ClockNow` / `clockNow()`, `cs::OnlineState` / `onlineStatusText` / `OnlineStatus`, `storeBootId()`, `storeReadBatch(..., endSeq)` / `storeMarkRecordsSent(endSeq)`, `storePeekMessage(msg, id)` / `storePopMessage(id)`, `telemetrySampleDue` / `telemetryLoop`, `netlinkPause` / `netlinkResume`, `NetConfig.batchMin` / `OnlineSettings.batchMin`. The whole plan was applied to a scratch copy of the repository while writing it: the firmware built and all 89 native tests passed.
