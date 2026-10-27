# Partition, Victron Lock, Auto-Cycle, Orion XS — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Switch ChargeScreen to a single 3 MB USB-flashed app, lock each Victron device kind to the user's own hardware, add optional auto-cycling between dashboard screens, and add a violet Orion XS DC-DC screen.

**Architecture:** All device-independent logic (bit reading, MAC handling, the lock state machine, page visibility, cycle timing, Orion XS parsing, status texts) goes into one header-only, Arduino-free file `include/chargescreen_logic.h` (namespace `cs`) so it can be unit-tested natively with Unity. `src/main.cpp` keeps all hardware, drawing, NVS and web code, following its existing style (file-static globals, `F()` strings, `secrets` Preferences), and calls into `cs::`.

**Tech Stack:** PlatformIO, Arduino-ESP32 (espressif32 platform), NimBLE-Arduino, GFX Library for Arduino 1.4.7, mbedtls AES, Unity (native tests), MinGW-w64 gcc for native tests on Windows.

**Spec:** `design/specs/2026-10-26-lock-cycle-orion-design.md`

## Global Constraints

- Flashing is USB only. No web/OTA firmware update code may remain.
- Partition table: `nvs` 0x9000/0x5000 (unchanged), `factory` app 0x10000/0x300000, `spiffs` 0x310000/0xF0000.
- NVS namespace stays `"victron"` (the `secrets` object). New keys: `shunt_mac`, `solar_mac`, `orion_mac`, `cycle_secs`, `orion_key`, `demo_oa`, `demo_ov`, `demo_iv`, `demo_ia`. NVS key names must be ≤ 15 characters.
- Lock: 3 consecutive good decodes from one address locks it; release after 600000 ms unseen, or on key change/clear.
- Cycle options: 0 (Off), 10, 20, 30, 60 seconds. Default 0.
- Page order: 0 battery, 1 solar, 2 Orion XS. Battery always visible; solar visible with a solar key; Orion visible with an Orion key; demo mode shows all.
- Orion XS record type `0x0F`; arc full scale 50 A.
- Orion palette: panel `0x2887`, track `0x598F`, arc `0xB37F`, text `0xF75F`, dim text `0xCDBC`.
- Commits in this repo are authored by the repo's configured identity (`istrasoft`, no-reply email). Never pass `--author` or change `user.name`/`user.email`.
- End every commit message with the line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Firmware build command: `pio run -e esp32-c3-round-dashboard`. Native test command: `pio test -e native`.
- Spec deviation (deliberate, approved in plan review): shunt plausibility also accepts SOC 102.3 % (Victron's "SOC not available" value 0x3FF), otherwise an unsynchronised shunt would never display or lock.

## File Structure

| File | Responsibility |
|---|---|
| `partitions.csv` (modify) | Single factory app layout |
| `platformio.ini` (modify) | `default_envs`, new `[env:native]` for Unity tests |
| `include/chargescreen_logic.h` (create) | Pure logic in namespace `cs` |
| `test/test_lock/test_main.cpp` (create) | Bits, MAC, lock, plausibility tests |
| `test/test_pages/test_main.cpp` (create) | Page visibility and cycle tests |
| `test/test_orion/test_main.cpp` (create) | Orion XS parse, off-reason and state-text tests |
| `src/main.cpp` (modify) | Wiring, drawing, NVS, web page |
| `README.md` (modify) | Document new features and USB-only flashing |

---

### Task 0: Host compiler for native tests

**Files:** none (machine setup).

**Interfaces:**
- Consumes: nothing.
- Produces: `gcc`/`g++` on PATH so `pio test -e native` can build.

- [ ] **Step 1: Check whether gcc already exists**

Run (PowerShell): `where.exe gcc`
Expected: either a path (skip to Step 4) or "Could not find files".

- [ ] **Step 2: Install MinGW-w64 (WinLibs) — ask the user before running**

Run (PowerShell): `winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT --accept-source-agreements --accept-package-agreements`
Expected: "Successfully installed".

- [ ] **Step 3: Refresh PATH in the current shell**

Run (PowerShell): `$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine'); where.exe gcc`
Expected: a path ending in `\mingw64\bin\gcc.exe`. If not found, ask the user to restart Claude Code so it inherits the new PATH.

- [ ] **Step 4: Confirm version**

Run: `gcc --version`
Expected: first line starts with `gcc.exe (MinGW-W64`.

No commit.

---

### Task 1: Baseline build

**Files:** none modified (the pre-build script rewrites `include/build_version.h` with identical content).

**Interfaces:**
- Consumes: nothing.
- Produces: confirmation that unmodified beta4.16 builds, and its flash/RAM numbers for comparison.

- [ ] **Step 1: Build the unmodified firmware**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: ends with `[SUCCESS]`. The first run downloads the espressif32 platform and libraries (several minutes). Note the two lines `RAM:   [...] x% (used N bytes ...)` and `Flash: [...] y% (used N bytes from 1310720 bytes)`.

If the build fails, stop and report the full error. Do not change library versions without asking.

- [ ] **Step 2: Confirm the tree is clean**

Run: `git status --short`
Expected: no output.

No commit.

---

### Task 2: Single-app partition and OTA removal

**Files:**
- Modify: `partitions.csv`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: a 3 MB app slot. No functions used by later tasks.

- [ ] **Step 1: Replace `partitions.csv`**

Full new contents:

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
factory,  app,  factory, 0x10000,  0x300000,
spiffs,   data, spiffs,  0x310000, 0xF0000,
```

- [ ] **Step 2: Remove OTA includes**

In `src/main.cpp` delete these two lines:

```cpp
#include <Update.h>
```

```cpp
#include <mbedtls/sha256.h>
```

- [ ] **Step 3: Remove OTA globals**

Delete this block (just after `DNSServer captiveDnsServer;`):

```cpp
static bool firmwareUploadFailed = false;
static bool firmwareUploadFinished = false;
static bool firmwareHeaderChecked = false;
static bool firmwareHashActive = false;
static size_t firmwareUploadSize = 0;
static uint8_t firmwareDigest[32] = {};
static mbedtls_sha256_context firmwareSha;
```

- [ ] **Step 4: Remove OTA status messages**

In `settingsStatusMessage` delete:

```cpp
  if (code == "ota-ok") {
    return "Firmware uploaded. Device is restarting.";
  }
  if (code == "ota-failed") {
    return "Firmware update failed. Try the correct firmware.bin.";
  }
```

- [ ] **Step 5: Remove the firmware header check**

Delete the whole function `static bool looksLikeEsp32FirmwareChunk(const uint8_t *data, size_t len) { ... }`.

- [ ] **Step 6: Remove the upload form and fix the hint**

In `settingsPageHtml` delete these three lines:

```cpp
  html += F("<form method='post' action='/firmware' enctype='multipart/form-data' data-working='Uploading firmware...'>");
  html += F("<label for='firmware'>Firmware update</label><input id='firmware' name='firmware' type='file' accept='.bin,application/octet-stream'>");
  html += F("<button class='secondary' type='submit'>Upload Firmware</button></form>");
```

and replace

```cpp
  html += F("<p class='hint'>Leave a key field blank to keep its current value. Firmware updates must be a valid ESP32-C3 firmware.bin built for this project. The hotspot turns off after 5 minutes with no page activity.</p>");
```

with

```cpp
  html += F("<p class='hint'>Leave a key field blank to keep its current value. The hotspot turns off after 5 minutes with no page activity.</p>");
```

- [ ] **Step 7: Remove OTA handlers**

Delete the three whole functions `drawFirmwareUpdateWarning`, `handleFirmwareUpload`, and `handleFirmwareUpdateDone`.

- [ ] **Step 8: Remove the route**

In `startSettingsServer` delete:

```cpp
  settingsServer.on("/firmware", HTTP_POST, handleFirmwareUpdateDone, handleFirmwareUpload);
```

- [ ] **Step 9: Verify no OTA references remain**

Run: `git grep -n -E "firmwareUpload|firmwareSha|firmwareDigest|firmwareHash|firmwareHeader|Update\.|looksLikeEsp32|drawFirmwareUpdateWarning|/firmware|ota-ok|ota-failed" -- src/main.cpp`
Expected: no output.

- [ ] **Step 10: Build**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`, and the Flash line now reads `... from 3145728 bytes`.

- [ ] **Step 11: Commit**

```bash
git add partitions.csv src/main.cpp
git commit -m "Switch to single 3 MB app partition and remove web OTA

Firmware is flashed over USB only, so the dual OTA slots and the
unauthenticated /firmware upload route are no longer needed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Logic header — bits, MAC, plausibility, lock state machine

**Files:**
- Modify: `platformio.ini`
- Create: `include/chargescreen_logic.h`
- Create: `test/test_lock/test_main.cpp`
- Modify: `src/main.cpp` (move `signExtend`/`readBits` to the header)

**Interfaces:**
- Consumes: nothing.
- Produces (namespace `cs`, all `inline`):
  - `uint32_t readBits(const uint8_t *data, size_t dataLen, uint16_t startBit, uint8_t bitCount)`
  - `int32_t signExtend(uint32_t value, uint8_t width)`
  - `bool normaliseMac(const char *in, char out[13])` — 12 lowercase hex chars, NUL-terminated
  - `void formatMac(const char *normalised, char out[18])` — `AA:BB:CC:DD:EE:FF`
  - `bool plausibleSystemVoltage(float v)`, `bool plausibleOptionalVoltage(float v)`, `bool plausibleShunt(float voltage, float soc)`
  - `constexpr uint8_t LOCK_CONFIRM_DECODES = 3`
  - `struct DeviceLock { char locked[13]; char candidate[13]; uint8_t candidateCount; uint32_t lastLockedSeenMs; }`
  - `bool lockIsLocked(const DeviceLock &)`, `bool lockAccepts(const DeviceLock &, const char *mac)`, `void lockNoteSeen(DeviceLock &, const char *mac, uint32_t now)`, `bool lockNoteGoodDecode(DeviceLock &, const char *mac, uint32_t now)` (true only when it just locked), `bool lockExpired(const DeviceLock &, uint32_t now, uint32_t releaseMs)`, `void lockClear(DeviceLock &)`, `bool lockRestore(DeviceLock &, const char *storedMac, uint32_t now)`

- [ ] **Step 1: Add the native test environment**

Replace the whole of `platformio.ini` with:

```ini
[platformio]
default_envs = esp32-c3-round-dashboard

[env:esp32-c3-round-dashboard]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
monitor_speed = 115200
upload_speed = 921600

board_build.mcu = esp32c3
board_build.f_cpu = 160000000L
board_build.flash_size = 4MB
board_build.flash_mode = qio
board_build.filesystem = littlefs
board_build.partitions = partitions.csv
extra_scripts = pre:scripts/bump_build.py

build_flags =
  -D ARDUINO_USB_MODE=1
  -D ARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
  h2zero/NimBLE-Arduino
  moononournation/GFX Library for Arduino@1.4.7

[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17
```

- [ ] **Step 2: Write the failing tests**

Create `test/test_lock/test_main.cpp`:

```cpp
#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_logic.h"

void setUp() {}
void tearDown() {}

static void test_read_bits_spans_bytes() {
  const uint8_t data[] = {0xAB, 0xCD};
  TEST_ASSERT_EQUAL_HEX32(0xDA, cs::readBits(data, sizeof(data), 4, 8));
  TEST_ASSERT_EQUAL_HEX32(0xCDAB, cs::readBits(data, sizeof(data), 0, 16));
}

static void test_read_bits_stops_at_end() {
  const uint8_t data[] = {0xFF};
  TEST_ASSERT_EQUAL_HEX32(0xFF, cs::readBits(data, sizeof(data), 0, 16));
}

static void test_sign_extend() {
  TEST_ASSERT_EQUAL_INT32(-1, cs::signExtend(0x3FFFFF, 22));
  TEST_ASSERT_EQUAL_INT32(-2097152, cs::signExtend(0x200000, 22));
  TEST_ASSERT_EQUAL_INT32(5, cs::signExtend(5, 22));
}

static void test_normalise_mac() {
  char out[13];
  TEST_ASSERT_TRUE(cs::normaliseMac("AA:bb:CC:dd:EE:ff", out));
  TEST_ASSERT_EQUAL_STRING("aabbccddeeff", out);
  TEST_ASSERT_FALSE(cs::normaliseMac("aa:bb:cc", out));
  TEST_ASSERT_FALSE(cs::normaliseMac("aa:bb:cc:dd:ee:ff:00", out));
  TEST_ASSERT_FALSE(cs::normaliseMac("", out));
}

static void test_format_mac() {
  char out[18];
  cs::formatMac("aabbccddeeff", out);
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", out);
}

static void test_plausibility() {
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 80.0f));
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 102.3f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(4.0f, 80.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(71.0f, 80.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, 150.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, -1.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(NAN, 80.0f));
  TEST_ASSERT_TRUE(cs::plausibleOptionalVoltage(NAN));
  TEST_ASSERT_TRUE(cs::plausibleOptionalVoltage(14.4f));
  TEST_ASSERT_FALSE(cs::plausibleOptionalVoltage(2.0f));
}

static void test_unlocked_accepts_any_address() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  TEST_ASSERT_TRUE(cs::lockAccepts(lock, "aabbccddeeff"));
  TEST_ASSERT_TRUE(cs::lockAccepts(lock, "112233445566"));
}

static void test_locks_after_three_good_decodes() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 100));
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 200));
  TEST_ASSERT_TRUE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 300));
  TEST_ASSERT_TRUE(cs::lockIsLocked(lock));
  TEST_ASSERT_EQUAL_STRING("aabbccddeeff", lock.locked);
  TEST_ASSERT_EQUAL_UINT32(300, lock.lastLockedSeenMs);
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 400));
}

static void test_other_address_resets_candidate() {
  cs::DeviceLock lock;
  cs::lockNoteGoodDecode(lock, "aabbccddeeff", 100);
  cs::lockNoteGoodDecode(lock, "aabbccddeeff", 200);
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "112233445566", 300));
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 400));
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  TEST_ASSERT_EQUAL_UINT8(1, lock.candidateCount);
}

static void test_locked_rejects_other_addresses() {
  cs::DeviceLock lock;
  TEST_ASSERT_TRUE(cs::lockRestore(lock, "AA:BB:CC:DD:EE:FF", 0));
  TEST_ASSERT_TRUE(cs::lockAccepts(lock, "aabbccddeeff"));
  TEST_ASSERT_FALSE(cs::lockAccepts(lock, "112233445566"));
}

static void test_note_seen_refreshes_only_locked_address() {
  cs::DeviceLock lock;
  cs::lockRestore(lock, "aabbccddeeff", 1000);
  cs::lockNoteSeen(lock, "112233445566", 5000);
  TEST_ASSERT_EQUAL_UINT32(1000, lock.lastLockedSeenMs);
  cs::lockNoteSeen(lock, "aabbccddeeff", 6000);
  TEST_ASSERT_EQUAL_UINT32(6000, lock.lastLockedSeenMs);
}

static void test_lock_expiry_with_wraparound() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 999999, 600000));
  cs::lockRestore(lock, "aabbccddeeff", 1000);
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 600999, 600000));
  TEST_ASSERT_TRUE(cs::lockExpired(lock, 601000, 600000));
  cs::lockRestore(lock, "aabbccddeeff", 0xFFFFFF00UL);
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 0x100UL, 600000));
}

static void test_restore_rejects_bad_value_and_clear_unlocks() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockRestore(lock, "", 0));
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  cs::lockRestore(lock, "aabbccddeeff", 0);
  cs::lockClear(lock);
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  TEST_ASSERT_EQUAL_UINT8(0, lock.candidateCount);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_read_bits_spans_bytes);
  RUN_TEST(test_read_bits_stops_at_end);
  RUN_TEST(test_sign_extend);
  RUN_TEST(test_normalise_mac);
  RUN_TEST(test_format_mac);
  RUN_TEST(test_plausibility);
  RUN_TEST(test_unlocked_accepts_any_address);
  RUN_TEST(test_locks_after_three_good_decodes);
  RUN_TEST(test_other_address_resets_candidate);
  RUN_TEST(test_locked_rejects_other_addresses);
  RUN_TEST(test_note_seen_refreshes_only_locked_address);
  RUN_TEST(test_lock_expiry_with_wraparound);
  RUN_TEST(test_restore_rejects_bad_value_and_clear_unlocks);
  return UNITY_END();
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `pio test -e native -f test_lock`
Expected: FAIL, compile error `chargescreen_logic.h: No such file or directory`.

- [ ] **Step 4: Create the header**

Create `include/chargescreen_logic.h`:

```cpp
#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Device-independent ChargeScreen logic. No Arduino dependencies so it can be
// unit-tested natively (pio test -e native).
namespace cs {

inline int32_t signExtend(uint32_t value, uint8_t width) {
  uint32_t signBit = 1UL << (width - 1);
  uint32_t mask = (1UL << width) - 1;
  value &= mask;
  return static_cast<int32_t>((value ^ signBit) - signBit);
}

inline uint32_t readBits(const uint8_t *data, size_t dataLen, uint16_t startBit, uint8_t bitCount) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < bitCount; i++) {
    uint16_t bit = startBit + i;
    if ((bit / 8) >= dataLen) {
      break;
    }
    if (data[bit / 8] & (1 << (bit % 8))) {
      value |= (1UL << i);
    }
  }
  return value;
}

inline bool normaliseMac(const char *in, char out[13]) {
  size_t count = 0;
  for (const char *p = in; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (!isxdigit(c)) {
      continue;
    }
    if (count == 12) {
      out[0] = '\0';
      return false;
    }
    out[count++] = static_cast<char>(tolower(c));
  }
  out[count] = '\0';
  if (count != 12) {
    out[0] = '\0';
    return false;
  }
  return true;
}

inline void formatMac(const char *normalised, char out[18]) {
  size_t o = 0;
  for (size_t i = 0; i < 12; i++) {
    if (i > 0 && i % 2 == 0) {
      out[o++] = ':';
    }
    out[o++] = static_cast<char>(toupper(static_cast<unsigned char>(normalised[i])));
  }
  out[o] = '\0';
}

inline bool plausibleSystemVoltage(float v) {
  return !std::isnan(v) && v >= 5.0f && v <= 70.0f;
}

inline bool plausibleOptionalVoltage(float v) {
  return std::isnan(v) || plausibleSystemVoltage(v);
}

// SOC 102.3 % is Victron's "not available" (raw 0x3FF), sent before the
// shunt has synchronised; it must still count as a real shunt.
inline bool plausibleShunt(float voltage, float soc) {
  bool socOk = (soc >= 0.0f && soc <= 100.05f) || std::fabs(soc - 102.3f) < 0.05f;
  return plausibleSystemVoltage(voltage) && socOk;
}

constexpr uint8_t LOCK_CONFIRM_DECODES = 3;

struct DeviceLock {
  char locked[13] = {};
  char candidate[13] = {};
  uint8_t candidateCount = 0;
  uint32_t lastLockedSeenMs = 0;
};

inline bool lockIsLocked(const DeviceLock &lock) {
  return lock.locked[0] != '\0';
}

inline bool lockAccepts(const DeviceLock &lock, const char *mac) {
  return !lockIsLocked(lock) || strcmp(lock.locked, mac) == 0;
}

inline void lockNoteSeen(DeviceLock &lock, const char *mac, uint32_t now) {
  if (lockIsLocked(lock) && strcmp(lock.locked, mac) == 0) {
    lock.lastLockedSeenMs = now;
  }
}

// Returns true only on the decode that creates the lock.
inline bool lockNoteGoodDecode(DeviceLock &lock, const char *mac, uint32_t now) {
  if (lockIsLocked(lock)) {
    lockNoteSeen(lock, mac, now);
    return false;
  }
  if (strcmp(lock.candidate, mac) == 0) {
    if (lock.candidateCount < 255) {
      lock.candidateCount++;
    }
  } else {
    strncpy(lock.candidate, mac, 12);
    lock.candidate[12] = '\0';
    lock.candidateCount = 1;
  }
  if (lock.candidateCount < LOCK_CONFIRM_DECODES) {
    return false;
  }
  memcpy(lock.locked, lock.candidate, sizeof(lock.locked));
  lock.candidate[0] = '\0';
  lock.candidateCount = 0;
  lock.lastLockedSeenMs = now;
  return true;
}

inline bool lockExpired(const DeviceLock &lock, uint32_t now, uint32_t releaseMs) {
  return lockIsLocked(lock) && static_cast<uint32_t>(now - lock.lastLockedSeenMs) >= releaseMs;
}

inline void lockClear(DeviceLock &lock) {
  lock = DeviceLock();
}

inline bool lockRestore(DeviceLock &lock, const char *storedMac, uint32_t now) {
  char normalised[13];
  if (!normaliseMac(storedMac, normalised)) {
    return false;
  }
  lockClear(lock);
  memcpy(lock.locked, normalised, sizeof(lock.locked));
  lock.lastLockedSeenMs = now;
  return true;
}

}  // namespace cs
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pio test -e native -f test_lock`
Expected: `13 Tests 0 Failures 0 Ignored` and `PASSED`.

- [ ] **Step 6: Move `signExtend`/`readBits` out of `main.cpp`**

In `src/main.cpp`, after `#include "build_version.h"` add:

```cpp
#include "chargescreen_logic.h"
```

Delete the two whole functions `static int32_t signExtend(uint32_t value, uint8_t width) { ... }` and `static uint32_t readBits(const uint8_t *data, size_t dataLen, uint16_t startBit, uint8_t bitCount) { ... }`, and put in their place:

```cpp
using cs::readBits;
using cs::signExtend;
```

- [ ] **Step 7: Build firmware**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`.

- [ ] **Step 8: Commit**

```bash
git add platformio.ini include/chargescreen_logic.h test/test_lock/test_main.cpp src/main.cpp
git commit -m "Add testable logic header with Victron device lock state machine

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Wire the lock into shunt and solar decoding

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `cs::DeviceLock`, `cs::normaliseMac`, `cs::formatMac`, `cs::lockAccepts`, `cs::lockNoteSeen`, `cs::lockNoteGoodDecode`, `cs::lockExpired`, `cs::lockClear`, `cs::lockRestore`, `cs::lockIsLocked`, `cs::plausibleShunt`, `cs::plausibleOptionalVoltage` (Task 3).
- Produces (in `main.cpp`, used by Task 6):
  - `static constexpr uint32_t VICTRON_LOCK_RELEASE_MS = 600000;`
  - `static cs::DeviceLock shuntLock; static cs::DeviceLock solarLock;`
  - `static void releaseVictronLock(cs::DeviceLock &lock, const char *nvsKey, const char *kind, const char *reason)`
  - `static String lockStatusText(const cs::DeviceLock &lock)`
  - `static void noteVictronGoodDecode(cs::DeviceLock &lock, const char *nvsKey, const char *kind, const char *mac, const String &address)`
  - Decoder signature pattern: `static bool decodeVictronBatteryMonitor(const std::string &manufacturerData, BatteryStats &out, String &error)` and `static bool decodeVictronSolarCharger(const std::string &manufacturerData, SolarStats &out, String &error)` — decode into `out`, never touch globals.

- [ ] **Step 1: Add the release constant**

After `static constexpr uint32_t SETTINGS_SERVER_IDLE_MS = 300000;` add:

```cpp
static constexpr uint32_t VICTRON_LOCK_RELEASE_MS = 600000;
```

- [ ] **Step 2: Add lock globals**

After `SolarStats solarStats;` add:

```cpp
static cs::DeviceLock shuntLock;
static cs::DeviceLock solarLock;
```

- [ ] **Step 3: Remove the address stubs**

Delete the three whole functions `normaliseAddress`, `matchesConfiguredVictronAddress`, and `matchesConfiguredSolarAddress`.

- [ ] **Step 4: Restore locks at boot**

In `loadStoredSecrets`, directly after `storedEcoWorthyPassword = secrets.getString("eco_key", "");` add:

```cpp
  cs::lockRestore(shuntLock, secrets.getString("shunt_mac", "").c_str(), millis());
  cs::lockRestore(solarLock, secrets.getString("solar_mac", "").c_str(), millis());
```

- [ ] **Step 5: Make the shunt decoder pure**

Replace the whole function `decodeVictronBatteryMonitor` with:

```cpp
static bool decodeVictronBatteryMonitor(const std::string &manufacturerData, BatteryStats &out, String &error) {
  if (manufacturerData.size() < 11) {
    error = "short advert";
    return false;
  }

  const uint8_t *data = reinterpret_cast<const uint8_t *>(manufacturerData.data());
  const uint8_t *payload = data + 2;
  size_t payloadLen = manufacturerData.size() - 2;

  if (payloadLen < 9 || payload[0] != VICTRON_PRODUCT_ADVERTISEMENT) {
    error = "not instant";
    return false;
  }

  if (payload[4] != VICTRON_BATTERY_MONITOR_RECORD) {
    error = "not shunt record";
    return false;
  }

  uint8_t plain[24] = {};
  size_t encryptedLen = sizeof(plain);

  if (!decryptVictronPayload(payload, payloadLen, activeVictronKey(), plain, encryptedLen, error)) {
    return false;
  }

  if (encryptedLen < 14) {
    error = "short data";
    return false;
  }

  int32_t currentRaw = signExtend(readBits(plain, encryptedLen, 66, 22), 22);
  uint32_t consumedRaw = readBits(plain, encryptedLen, 88, 20);

  out = BatteryStats();
  out.timeToGoMinutes = readBits(plain, encryptedLen, 0, 16);
  out.voltage = static_cast<int16_t>(readBits(plain, encryptedLen, 16, 16)) * 0.01f;
  out.alarmReason = readBits(plain, encryptedLen, 32, 16);
  out.auxValue = static_cast<int16_t>(readBits(plain, encryptedLen, 48, 16)) * 0.01f;
  out.auxMode = readBits(plain, encryptedLen, 64, 2);
  out.current = currentRaw * 0.001f;
  out.power = out.voltage * out.current;
  out.consumedAh = consumedRaw == 0xFFFFF ? NAN : -static_cast<float>(consumedRaw) * 0.1f;
  out.soc = readBits(plain, encryptedLen, 108, 10) * 0.1f;
  out.valid = true;
  out.error = "";

  Serial.printf("Victron decoded V=%.2fV I=%.3fA P=%.0fW consumed=%.1fAh soc=%.1f%% auxMode=%u aux=%.2f alarm=0x%04X\n",
                out.voltage,
                out.current,
                out.power,
                out.consumedAh,
                out.soc,
                out.auxMode,
                out.auxValue,
                out.alarmReason);
  return true;
}
```

- [ ] **Step 6: Make the solar decoder pure**

Replace the whole function `decodeVictronSolarCharger` with:

```cpp
static bool decodeVictronSolarCharger(const std::string &manufacturerData, SolarStats &out, String &error) {
  if (manufacturerData.size() < 11) {
    error = "short advert";
    return false;
  }

  const uint8_t *data = reinterpret_cast<const uint8_t *>(manufacturerData.data());
  const uint8_t *payload = data + 2;
  size_t payloadLen = manufacturerData.size() - 2;

  if (payloadLen < 9 || payload[0] != VICTRON_PRODUCT_ADVERTISEMENT) {
    error = "not instant";
    return false;
  }

  if (payload[4] != VICTRON_SOLAR_CHARGER_RECORD) {
    error = "not solar record";
    return false;
  }

  uint8_t plain[16] = {};
  size_t encryptedLen = sizeof(plain);

  if (!decryptVictronPayload(payload, payloadLen, activeSolarKey(), plain, encryptedLen, error)) {
    return false;
  }

  if (encryptedLen < 12) {
    error = "short data";
    return false;
  }

  uint16_t batteryVoltageRaw = 0;
  uint16_t batteryCurrentRaw = 0;
  uint16_t yieldRaw = 0;
  uint16_t pvPowerRaw = 0;
  if (!readLeU16(plain, encryptedLen, 2, batteryVoltageRaw) ||
      !readLeU16(plain, encryptedLen, 4, batteryCurrentRaw) ||
      !readLeU16(plain, encryptedLen, 6, yieldRaw) ||
      !readLeU16(plain, encryptedLen, 8, pvPowerRaw)) {
    error = "short data";
    return false;
  }
  uint16_t loadCurrentRaw = readBits(plain, encryptedLen, 80, 9);

  out = SolarStats();
  out.deviceState = plain[0];
  out.errorCode = plain[1];
  out.batteryVoltage = batteryVoltageRaw == 0x7FFF
                           ? NAN
                           : static_cast<int16_t>(batteryVoltageRaw) * 0.01f;
  out.batteryCurrent = batteryCurrentRaw == 0x7FFF
                           ? NAN
                           : static_cast<int16_t>(batteryCurrentRaw) * 0.1f;
  out.yieldTodayKwh = yieldRaw == 0xFFFF ? NAN : yieldRaw * 0.01f;
  out.pvPower = pvPowerRaw == 0xFFFF ? NAN : static_cast<float>(pvPowerRaw);
  out.loadCurrent = loadCurrentRaw == 0x1FF ? NAN : loadCurrentRaw * 0.1f;
  out.valid = true;
  out.error = "";

  logDecryptedBytes("Victron solar", plain, encryptedLen);
  Serial.printf("Victron solar decoded state=%u err=%u V=%.2fV I=%.1fA PV=%.0fW yield=%.2fkWh load=%.1fA\n",
                out.deviceState,
                out.errorCode,
                out.batteryVoltage,
                out.batteryCurrent,
                out.pvPower,
                out.yieldTodayKwh,
                out.loadCurrent);
  return true;
}
```

- [ ] **Step 7: Add lock helpers and advert handlers**

Directly before `class ScanCallbacks : public NimBLEScanCallbacks {` add:

```cpp
static void releaseVictronLock(cs::DeviceLock &lock, const char *nvsKey, const char *kind, const char *reason) {
  bool wasLocked = cs::lockIsLocked(lock);
  cs::lockClear(lock);
  secrets.remove(nvsKey);
  if (wasLocked) {
    Serial.printf("Victron %s lock released (%s)\n", kind, reason);
  }
}

static String lockStatusText(const cs::DeviceLock &lock) {
  if (!cs::lockIsLocked(lock)) {
    return "Searching";
  }
  char mac[18];
  cs::formatMac(lock.locked, mac);
  return String("Locked to ") + mac;
}

static void noteVictronGoodDecode(cs::DeviceLock &lock,
                                  const char *nvsKey,
                                  const char *kind,
                                  const char *mac,
                                  const String &address) {
  if (cs::lockNoteGoodDecode(lock, mac, millis())) {
    secrets.putString(nvsKey, mac);
    Serial.printf("Victron %s locked to %s\n", kind, address.c_str());
  }
}

static void handleShuntAdvert(const String &name,
                              const String &address,
                              const char *mac,
                              int rssi,
                              const std::string &manufacturerData) {
  if (!cs::lockAccepts(shuntLock, mac)) {
    return;
  }

  uint32_t now = millis();
  cs::lockNoteSeen(shuntLock, mac, now);
  victronSeen.name = name.length() ? name : "Victron";
  victronSeen.address = address;
  victronSeen.rssi = rssi;
  victronSeen.seenMs = now;
  lastVictronAdvertMs = now;

  BatteryStats decoded;
  String error;
  bool ok = decodeVictronBatteryMonitor(manufacturerData, decoded, error);
  if (ok && !cs::plausibleShunt(decoded.voltage, decoded.soc)) {
    ok = false;
    error = "implausible";
  }

  if (ok) {
    victronStats = decoded;
    lastVictronDecodeMs = now;
    noteVictronGoodDecode(shuntLock, "shunt_mac", "shunt", mac, address);
  } else if (!victronStats.valid) {
    victronStats.error = error;
  }

  Serial.printf("Victron shunt %s RSSI %d %s data %s\n",
                address.c_str(),
                rssi,
                ok ? "decoded" : error.c_str(),
                bytesToHex(manufacturerData).c_str());
}

static void handleSolarAdvert(const String &name,
                              const String &address,
                              const char *mac,
                              int rssi,
                              const std::string &manufacturerData) {
  if (!cs::lockAccepts(solarLock, mac)) {
    return;
  }

  uint32_t now = millis();
  cs::lockNoteSeen(solarLock, mac, now);
  solarSeen.name = name.length() ? name : "SmartSolar";
  solarSeen.address = address;
  solarSeen.rssi = rssi;
  solarSeen.seenMs = now;
  lastSolarAdvertMs = now;

  SolarStats decoded;
  String error;
  bool ok = decodeVictronSolarCharger(manufacturerData, decoded, error);
  if (ok && !cs::plausibleOptionalVoltage(decoded.batteryVoltage)) {
    ok = false;
    error = "implausible";
  }

  if (ok) {
    solarStats = decoded;
    lastSolarDecodeMs = now;
    noteVictronGoodDecode(solarLock, "solar_mac", "solar", mac, address);
  } else if (!solarStats.valid) {
    solarStats.error = error;
  }

  Serial.printf("Victron solar %s RSSI %d %s data %s\n",
                address.c_str(),
                rssi,
                ok ? "decoded" : error.c_str(),
                bytesToHex(manufacturerData).c_str());
}
```

- [ ] **Step 8: Route adverts through the handlers**

In `ScanCallbacks::onResult` replace the whole block

```cpp
    if (device->haveManufacturerData()) {
      std::string manufacturerData = device->getManufacturerData();
      if (isVictronInstantReadout(manufacturerData)) {
        ...
      }
    }
```

(everything from `if (device->haveManufacturerData()) {` down to its matching closing brace, just before `if (nameLooksEcoWorthyOrBms(name)) {`) with:

```cpp
    if (device->haveManufacturerData()) {
      std::string manufacturerData = device->getManufacturerData();
      if (isVictronInstantReadout(manufacturerData)) {
        const uint8_t *data = reinterpret_cast<const uint8_t *>(manufacturerData.data());
        uint8_t recordType = manufacturerData.size() > 6 ? data[6] : 0xFF;
        char mac[13];
        if (cs::normaliseMac(address.c_str(), mac)) {
          if (recordType == VICTRON_BATTERY_MONITOR_RECORD) {
            handleShuntAdvert(name, address, mac, rssi, manufacturerData);
          } else if (recordType == VICTRON_SOLAR_CHARGER_RECORD) {
            handleSolarAdvert(name, address, mac, rssi, manufacturerData);
          }
        }
      }
    }
```

- [ ] **Step 9: Release locks when keys change or are cleared**

In `handleSettingsSave`, inside `if (isValidKeyHex(shuntKey)) {` add as the first line:

```cpp
      if (shuntKey != storedVictronKey) {
        releaseVictronLock(shuntLock, "shunt_mac", "shunt", "key changed");
      }
```

and inside `if (isValidKeyHex(solarKey)) {` add as the first line:

```cpp
      if (solarKey != storedSolarKey) {
        releaseVictronLock(solarLock, "solar_mac", "solar", "key changed");
      }
```

In `handleSettingsClearKeys`, after `secrets.remove("eco_key");` add:

```cpp
  releaseVictronLock(shuntLock, "shunt_mac", "shunt", "keys cleared");
  releaseVictronLock(solarLock, "solar_mac", "solar", "keys cleared");
```

- [ ] **Step 10: Expire locks in the watchdog**

In `updateBleWatchdog`, directly before `if (demoModeEnabled) {` add:

```cpp
  if (cs::lockExpired(shuntLock, now, VICTRON_LOCK_RELEASE_MS)) {
    releaseVictronLock(shuntLock, "shunt_mac", "shunt", "not seen for 10 min");
  }

  if (cs::lockExpired(solarLock, now, VICTRON_LOCK_RELEASE_MS)) {
    releaseVictronLock(solarLock, "solar_mac", "solar", "not seen for 10 min");
  }
```

- [ ] **Step 11: Show lock status on the setup page**

In `settingsPageHtml`, directly after the line that closes the solar key input (`html += maskedKeyText(storedSolarKey);` followed by `html += F("'>");`), add:

```cpp
  html += F("<p class='hint'>Battery monitor: ");
  html += lockStatusText(shuntLock);
  html += F("<br>Solar controller: ");
  html += lockStatusText(solarLock);
  html += F("</p>");
```

- [ ] **Step 12: Verify no stale references**

Run: `git grep -n -E "matchesConfigured|normaliseAddress|decodeVictron(BatteryMonitor|SolarCharger)\(manufacturerData\)" -- src/main.cpp`
Expected: no output.

- [ ] **Step 13: Build and run tests**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`.
Run: `pio test -e native`
Expected: all tests pass.

- [ ] **Step 14: Commit**

```bash
git add src/main.cpp
git commit -m "Lock shunt and solar to the user's own Victron devices

Adverts from other addresses are ignored once a device is locked, and
decode failures no longer invalidate good readings.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Page visibility and auto-cycling

**Files:**
- Modify: `include/chargescreen_logic.h`
- Create: `test/test_pages/test_main.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: nothing new from earlier tasks.
- Produces:
  - In `cs`: `struct PageAvailability { bool demo; bool solar; bool orion; }`, `bool pageVisible(uint8_t page, const PageAvailability &)`, `uint8_t nextVisiblePage(uint8_t current, int8_t direction, uint8_t pageCount, const PageAvailability &)`, `uint16_t sanitiseCycleSeconds(int)`, `uint16_t nextCycleSeconds(uint16_t)`, `bool cycleDue(uint32_t now, uint32_t lastPageChangeMs, uint32_t lastTouchMs, uint16_t cycleSeconds)`
  - In `main.cpp`: `static cs::PageAvailability pageAvailability()` (Task 6 changes its `orion` field), globals `cycleSeconds`, `lastPageChangeMs`.

- [ ] **Step 1: Write the failing tests**

Create `test/test_pages/test_main.cpp`:

```cpp
#include <unity.h>

#include "chargescreen_logic.h"

void setUp() {}
void tearDown() {}

static void test_page_visibility() {
  cs::PageAvailability none{false, false, false};
  cs::PageAvailability solarOnly{false, true, false};
  cs::PageAvailability orionOnly{false, false, true};
  cs::PageAvailability demo{true, false, false};
  TEST_ASSERT_TRUE(cs::pageVisible(0, none));
  TEST_ASSERT_FALSE(cs::pageVisible(1, none));
  TEST_ASSERT_TRUE(cs::pageVisible(1, solarOnly));
  TEST_ASSERT_FALSE(cs::pageVisible(2, solarOnly));
  TEST_ASSERT_TRUE(cs::pageVisible(2, orionOnly));
  TEST_ASSERT_TRUE(cs::pageVisible(1, demo));
  TEST_ASSERT_TRUE(cs::pageVisible(2, demo));
  TEST_ASSERT_FALSE(cs::pageVisible(3, demo));
}

static void test_next_visible_page() {
  cs::PageAvailability none{false, false, false};
  cs::PageAvailability solarOnly{false, true, false};
  cs::PageAvailability orionOnly{false, false, true};
  cs::PageAvailability demo{true, false, false};
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(0, 1, 3, none));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(0, -1, 3, none));
  TEST_ASSERT_EQUAL_UINT8(1, cs::nextVisiblePage(0, 1, 2, solarOnly));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(1, 1, 2, solarOnly));
  TEST_ASSERT_EQUAL_UINT8(1, cs::nextVisiblePage(0, -1, 2, solarOnly));
  TEST_ASSERT_EQUAL_UINT8(2, cs::nextVisiblePage(0, 1, 3, orionOnly));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(2, 1, 3, orionOnly));
  TEST_ASSERT_EQUAL_UINT8(1, cs::nextVisiblePage(0, 1, 3, demo));
  TEST_ASSERT_EQUAL_UINT8(2, cs::nextVisiblePage(1, 1, 3, demo));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(2, 1, 3, demo));
  TEST_ASSERT_EQUAL_UINT8(2, cs::nextVisiblePage(0, -1, 3, demo));
}

static void test_cycle_seconds() {
  TEST_ASSERT_EQUAL_UINT16(0, cs::sanitiseCycleSeconds(0));
  TEST_ASSERT_EQUAL_UINT16(20, cs::sanitiseCycleSeconds(20));
  TEST_ASSERT_EQUAL_UINT16(60, cs::sanitiseCycleSeconds(60));
  TEST_ASSERT_EQUAL_UINT16(0, cs::sanitiseCycleSeconds(15));
  TEST_ASSERT_EQUAL_UINT16(0, cs::sanitiseCycleSeconds(-5));
  TEST_ASSERT_EQUAL_UINT16(10, cs::nextCycleSeconds(0));
  TEST_ASSERT_EQUAL_UINT16(20, cs::nextCycleSeconds(10));
  TEST_ASSERT_EQUAL_UINT16(30, cs::nextCycleSeconds(20));
  TEST_ASSERT_EQUAL_UINT16(60, cs::nextCycleSeconds(30));
  TEST_ASSERT_EQUAL_UINT16(0, cs::nextCycleSeconds(60));
}

static void test_cycle_due() {
  TEST_ASSERT_FALSE(cs::cycleDue(100000, 0, 0, 0));
  TEST_ASSERT_TRUE(cs::cycleDue(20000, 0, 0, 10));
  TEST_ASSERT_FALSE(cs::cycleDue(20000, 15000, 0, 10));
  TEST_ASSERT_FALSE(cs::cycleDue(20000, 0, 15000, 10));
  TEST_ASSERT_TRUE(cs::cycleDue(10000, 0, 0, 10));
  TEST_ASSERT_FALSE(cs::cycleDue(0x100UL, 0xFFFFFF00UL, 0xFFFFFF00UL, 10));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_page_visibility);
  RUN_TEST(test_next_visible_page);
  RUN_TEST(test_cycle_seconds);
  RUN_TEST(test_cycle_due);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e native -f test_pages`
Expected: FAIL, compile errors such as `'PageAvailability' is not a member of 'cs'`.

- [ ] **Step 3: Implement in the header**

In `include/chargescreen_logic.h`, directly before the final `}  // namespace cs`, add:

```cpp
struct PageAvailability {
  bool demo;
  bool solar;
  bool orion;
};

// Page order: 0 battery, 1 solar, 2 Orion XS.
inline bool pageVisible(uint8_t page, const PageAvailability &availability) {
  if (page > 2) {
    return false;
  }
  if (page == 0 || availability.demo) {
    return true;
  }
  return page == 1 ? availability.solar : availability.orion;
}

inline uint8_t nextVisiblePage(uint8_t current, int8_t direction, uint8_t pageCount, const PageAvailability &availability) {
  for (uint8_t step = 1; step <= pageCount; step++) {
    uint8_t index = direction >= 0
                        ? static_cast<uint8_t>((current + step) % pageCount)
                        : static_cast<uint8_t>((current + pageCount - step) % pageCount);
    if (pageVisible(index, availability)) {
      return index;
    }
  }
  return current;
}

inline uint16_t sanitiseCycleSeconds(int value) {
  return (value == 10 || value == 20 || value == 30 || value == 60) ? static_cast<uint16_t>(value) : 0;
}

inline uint16_t nextCycleSeconds(uint16_t value) {
  switch (value) {
    case 0:
      return 10;
    case 10:
      return 20;
    case 20:
      return 30;
    case 30:
      return 60;
    default:
      return 0;
  }
}

inline bool cycleDue(uint32_t now, uint32_t lastPageChangeMs, uint32_t lastTouchMs, uint16_t cycleSeconds) {
  if (cycleSeconds == 0) {
    return false;
  }
  uint32_t intervalMs = static_cast<uint32_t>(cycleSeconds) * 1000UL;
  return static_cast<uint32_t>(now - lastPageChangeMs) >= intervalMs &&
         static_cast<uint32_t>(now - lastTouchMs) >= intervalMs;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_pages`
Expected: `4 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 5: Settings row positions and globals in `main.cpp`**

Replace

```cpp
static constexpr int16_t SETTINGS_LABELS_Y = 98;
static constexpr int16_t SETTINGS_GRID_Y = 136;
```

with

```cpp
static constexpr int16_t SETTINGS_LABELS_Y = 76;
static constexpr int16_t SETTINGS_GRID_Y = 116;
static constexpr int16_t SETTINGS_CYCLE_Y = 156;
```

After `static uint16_t solarArrayWatts = DEFAULT_SOLAR_ARRAY_WATTS;` add:

```cpp
static uint16_t cycleSeconds = 0;
static uint32_t lastPageChangeMs = 0;
```

In `loadStoredSecrets`, after `showValueGrid = secrets.getBool("value_grid", true);` add:

```cpp
  cycleSeconds = cs::sanitiseCycleSeconds(secrets.getUShort("cycle_secs", 0));
```

- [ ] **Step 6: Helpers**

Directly after the function `backlightLevelText()` add:

```cpp
static String cycleText() {
  return cycleSeconds ? String(cycleSeconds) + "s" : String("-");
}

static cs::PageAvailability pageAvailability() {
  return cs::PageAvailability{demoModeEnabled, storedSolarKey.length() > 0, false};
}
```

- [ ] **Step 7: Settings page 3 row**

In `drawSettingsPage`, in the final `else` branch, after `drawRow(SETTINGS_GRID_Y, "Grid", showValueGrid ? "On" : "-", showValueGrid);` add:

```cpp
    drawRow(SETTINGS_CYCLE_Y, "Cycle", cycleText(), cycleSeconds > 0);
```

- [ ] **Step 8: Tap handling for the Cycle row**

In `handleTap`, inside `if (settingsPageIndex == 2) {`, directly before its `if (pointInRect(x, y, SETTINGS_BACK_X, ...` back-button check, add:

```cpp
      if (pointInRect(x, y, SETTINGS_ROW_X, SETTINGS_CYCLE_Y, SETTINGS_VALUE_X + SETTINGS_VALUE_W - SETTINGS_ROW_X, SETTINGS_ROW_H)) {
        cycleSeconds = cs::nextCycleSeconds(cycleSeconds);
        secrets.putUShort("cycle_secs", cycleSeconds);
        settingsMessage = String("Cycle ") + cycleText();
        settingsPageDrawn = false;
        drawCurrentPage(true);
        return;
      }
```

- [ ] **Step 9: Visibility-aware page switching**

In `switchPage`, after `currentPage = page;` add:

```cpp
  lastPageChangeMs = millis();
```

Replace the whole body of `togglePageFromSwipe` with:

```cpp
  uint8_t nextPage = cs::nextVisiblePage(currentPage, direction, PAGE_COUNT, pageAvailability());
  switchPage(nextPage, direction);
```

In `drawCurrentPage`, directly before `if (currentPage == 0) {` add:

```cpp
  if (!cs::pageVisible(currentPage, pageAvailability())) {
    currentPage = 0;
    gaugeFaceDrawn = false;
    force = true;
  }
```

- [ ] **Step 10: Auto-cycle loop**

Directly before `void setup() {` add:

```cpp
static void updateAutoCycle() {
  if (!screenAwake || cycleSeconds == 0 || captureActive || captureReadyScreenActive ||
      settingsPageActive || settingsRotationPageActive || settingsInfoPageActive) {
    return;
  }

  uint32_t now = millis();
  if (!cs::cycleDue(now, lastPageChangeMs, lastTouchActivityMs, cycleSeconds)) {
    return;
  }

  lastPageChangeMs = now;
  switchPage(cs::nextVisiblePage(currentPage, 1, PAGE_COUNT, pageAvailability()), 1);
}
```

In `loop()`, after `updateTouchSwipe();` add:

```cpp
  updateAutoCycle();
```

- [ ] **Step 11: Setup page dropdown**

In `settingsPageHtml`, directly after the rotation select is closed (`html += F("</select>");` following the `rotationOptions` loop), add:

```cpp
  html += F("<label for='cycle_secs'>Screen cycle</label><select id='cycle_secs' name='cycle_secs'>");
  const uint16_t cycleOptions[] = {0, 10, 20, 30, 60};
  for (uint16_t seconds : cycleOptions) {
    html += F("<option value='");
    html += seconds;
    html += F("'");
    html += cycleSeconds == seconds ? F(" selected") : F("");
    html += F(">");
    html += seconds ? String(seconds) + " seconds" : String("Off");
    html += F("</option>");
  }
  html += F("</select>");
```

- [ ] **Step 12: Save handling**

In `handleSettingsSave`, after `String dimmingText = settingsServer.arg("dimming");` add:

```cpp
  String cycleArg = settingsServer.arg("cycle_secs");
  cycleArg.trim();
```

and directly before `demoBatterySoc = constrain(...` add:

```cpp
  if (cycleArg.length()) {
    int requestedCycle = cycleArg.toInt();
    uint16_t nextCycle = cs::sanitiseCycleSeconds(requestedCycle);
    if (nextCycle == requestedCycle) {
      if (nextCycle != cycleSeconds) {
        cycleSeconds = nextCycle;
        secrets.putUShort("cycle_secs", cycleSeconds);
      }
      saved = true;
    } else {
      invalid = true;
    }
  }
```

- [ ] **Step 13: Build and test**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`.
Run: `pio test -e native`
Expected: all tests pass.

- [ ] **Step 14: Commit**

```bash
git add include/chargescreen_logic.h test/test_pages/test_main.cpp src/main.cpp
git commit -m "Add optional auto-cycling and hide screens without a key

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Orion XS decoding, key and lock

**Files:**
- Modify: `include/chargescreen_logic.h`
- Create: `test/test_orion/test_main.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: Task 3 lock functions; Task 4 `releaseVictronLock`, `lockStatusText`, `noteVictronGoodDecode`, `decryptVictronPayload`, decoder pattern; Task 5 `pageAvailability()`.
- Produces:
  - In `cs`: `struct OrionXsReading { uint8_t state; uint8_t error; float outputVoltage; float outputCurrent; float inputVoltage; float inputCurrent; uint32_t offReason; }`, `bool parseOrionXs(const uint8_t *plain, size_t len, OrionXsReading &out)`, `const char *orionOffReasonText(uint32_t offReason)` (nullptr when no known reason), `const char *chargerStateText(uint8_t state)`
  - In `main.cpp`: `struct OrionStats`, globals `orionStats`, `orionSeen`, `orionLock`, `storedOrionKey`, `lastOrionAdvertMs`, `lastOrionDecodeMs`, constant `VICTRON_ORION_XS_RECORD`.

- [ ] **Step 1: Write the failing tests**

Create `test/test_orion/test_main.cpp`:

```cpp
#include <unity.h>

#include <cmath>

#include "chargescreen_logic.h"

void setUp() {}
void tearDown() {}

static void test_parse_orion_xs_values() {
  // state 3 (Bulk), error 0, out 14.20 V, out 32.0 A, in 13.40 V, in 35.5 A, off 0
  const uint8_t plain[] = {0x03, 0x00, 0x8C, 0x05, 0x40, 0x01, 0x3C, 0x05,
                           0x63, 0x01, 0x00, 0x00, 0x00, 0x00};
  cs::OrionXsReading reading;
  TEST_ASSERT_TRUE(cs::parseOrionXs(plain, sizeof(plain), reading));
  TEST_ASSERT_EQUAL_UINT8(3, reading.state);
  TEST_ASSERT_EQUAL_UINT8(0, reading.error);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.20f, reading.outputVoltage);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 32.0f, reading.outputCurrent);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.40f, reading.inputVoltage);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 35.5f, reading.inputCurrent);
  TEST_ASSERT_EQUAL_HEX32(0, reading.offReason);
}

static void test_parse_orion_xs_not_available_and_off_reason() {
  const uint8_t plain[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                           0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00};
  cs::OrionXsReading reading;
  TEST_ASSERT_TRUE(cs::parseOrionXs(plain, sizeof(plain), reading));
  TEST_ASSERT_EQUAL_UINT8(0, reading.state);
  TEST_ASSERT_EQUAL_UINT8(0xFF, reading.error);
  TEST_ASSERT_TRUE(std::isnan(reading.outputVoltage));
  TEST_ASSERT_TRUE(std::isnan(reading.outputCurrent));
  TEST_ASSERT_TRUE(std::isnan(reading.inputVoltage));
  TEST_ASSERT_TRUE(std::isnan(reading.inputCurrent));
  TEST_ASSERT_EQUAL_HEX32(0x80, reading.offReason);
}

static void test_parse_orion_xs_rejects_short_data() {
  const uint8_t plain[13] = {};
  cs::OrionXsReading reading;
  TEST_ASSERT_FALSE(cs::parseOrionXs(plain, sizeof(plain), reading));
}

static void test_off_reason_text() {
  TEST_ASSERT_NULL(cs::orionOffReasonText(0));
  TEST_ASSERT_EQUAL_STRING("Engine off", cs::orionOffReasonText(0x80));
  TEST_ASSERT_EQUAL_STRING("Engine off", cs::orionOffReasonText(0x81));
  TEST_ASSERT_EQUAL_STRING("No input", cs::orionOffReasonText(0x01));
  TEST_ASSERT_EQUAL_STRING("Switched off", cs::orionOffReasonText(0x02));
  TEST_ASSERT_EQUAL_STRING("Switched off", cs::orionOffReasonText(0x04));
  TEST_ASSERT_EQUAL_STRING("Remote off", cs::orionOffReasonText(0x08));
  TEST_ASSERT_EQUAL_STRING("Protection", cs::orionOffReasonText(0x10));
  TEST_ASSERT_EQUAL_STRING("BMS off", cs::orionOffReasonText(0x40));
  TEST_ASSERT_EQUAL_STRING("Checking input", cs::orionOffReasonText(0x100));
  TEST_ASSERT_NULL(cs::orionOffReasonText(0x20));
}

static void test_charger_state_text() {
  TEST_ASSERT_EQUAL_STRING("Off", cs::chargerStateText(0));
  TEST_ASSERT_EQUAL_STRING("Bulk", cs::chargerStateText(3));
  TEST_ASSERT_EQUAL_STRING("Float", cs::chargerStateText(5));
  TEST_ASSERT_EQUAL_STRING("Power supply", cs::chargerStateText(11));
  TEST_ASSERT_EQUAL_STRING("External", cs::chargerStateText(252));
  TEST_ASSERT_EQUAL_STRING("Charging", cs::chargerStateText(99));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_orion_xs_values);
  RUN_TEST(test_parse_orion_xs_not_available_and_off_reason);
  RUN_TEST(test_parse_orion_xs_rejects_short_data);
  RUN_TEST(test_off_reason_text);
  RUN_TEST(test_charger_state_text);
  return UNITY_END();
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e native -f test_orion`
Expected: FAIL, compile errors such as `'OrionXsReading' is not a member of 'cs'`.

- [ ] **Step 3: Implement in the header**

In `include/chargescreen_logic.h`, directly before the final `}  // namespace cs`, add:

```cpp
// Victron Orion XS Instant Readout record (type 0x0F). Undocumented by Victron;
// layout matches keshavdv/victron-ble orion_xs.py.
struct OrionXsReading {
  uint8_t state = 0xFF;
  uint8_t error = 0xFF;
  float outputVoltage = NAN;
  float outputCurrent = NAN;
  float inputVoltage = NAN;
  float inputCurrent = NAN;
  uint32_t offReason = 0;
};

inline float scaledOrNan(uint32_t raw, float scale) {
  return raw == 0xFFFF ? NAN : static_cast<float>(raw) * scale;
}

inline bool parseOrionXs(const uint8_t *plain, size_t len, OrionXsReading &out) {
  if (len < 14) {
    return false;
  }
  out.state = static_cast<uint8_t>(readBits(plain, len, 0, 8));
  out.error = static_cast<uint8_t>(readBits(plain, len, 8, 8));
  out.outputVoltage = scaledOrNan(readBits(plain, len, 16, 16), 0.01f);
  out.outputCurrent = scaledOrNan(readBits(plain, len, 32, 16), 0.1f);
  out.inputVoltage = scaledOrNan(readBits(plain, len, 48, 16), 0.01f);
  out.inputCurrent = scaledOrNan(readBits(plain, len, 64, 16), 0.1f);
  out.offReason = readBits(plain, len, 80, 32);
  return true;
}

// Off-reason bits from victron-ble OffReason. Returns nullptr when no known bit is set.
inline const char *orionOffReasonText(uint32_t offReason) {
  if (offReason & 0x80) {
    return "Engine off";
  }
  if (offReason & 0x01) {
    return "No input";
  }
  if (offReason & (0x02 | 0x04)) {
    return "Switched off";
  }
  if (offReason & 0x08) {
    return "Remote off";
  }
  if (offReason & 0x10) {
    return "Protection";
  }
  if (offReason & 0x40) {
    return "BMS off";
  }
  if (offReason & 0x100) {
    return "Checking input";
  }
  return nullptr;
}

inline const char *chargerStateText(uint8_t state) {
  switch (state) {
    case 0:
      return "Off";
    case 1:
      return "Low power";
    case 2:
      return "Fault";
    case 3:
      return "Bulk";
    case 4:
      return "Absorption";
    case 5:
      return "Float";
    case 6:
      return "Storage";
    case 7:
      return "Equalize";
    case 11:
      return "Power supply";
    case 245:
      return "Starting";
    case 246:
      return "Absorb again";
    case 247:
      return "Recondition";
    case 248:
      return "BatterySafe";
    case 252:
      return "External";
    default:
      return "Charging";
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_orion`
Expected: `5 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 5: Use the shared state text for solar**

In `src/main.cpp` delete the whole function `static const char *solarStateText(uint8_t state) { ... }`. In `displaySolarStatusText` replace `return solarStateText(stats.deviceState);` with:

```cpp
  return cs::chargerStateText(stats.deviceState);
```

- [ ] **Step 6: Orion constants, struct and globals**

After `static constexpr uint8_t VICTRON_BATTERY_MONITOR_RECORD = 0x02;` add:

```cpp
static constexpr uint8_t VICTRON_ORION_XS_RECORD = 0x0F;
```

After the closing `};` of `struct SolarStats` add:

```cpp
struct OrionStats {
  bool valid = false;
  uint8_t deviceState = 0xFF;
  uint8_t errorCode = 0xFF;
  float outputVoltage = NAN;
  float outputCurrent = NAN;
  float inputVoltage = NAN;
  float inputCurrent = NAN;
  uint32_t offReason = 0;
  String error = "waiting";
};
```

After `BleSeen ecoSeen;` add `BleSeen orionSeen;`. After `SolarStats solarStats;` add `OrionStats orionStats;`. After `static cs::DeviceLock solarLock;` add `static cs::DeviceLock orionLock;`. After `static uint32_t lastSolarDecodeMs = 0;` add:

```cpp
static uint32_t lastOrionAdvertMs = 0;
static uint32_t lastOrionDecodeMs = 0;
```

After `static String storedSolarKey;` add `static String storedOrionKey;`.

- [ ] **Step 7: Load the key and lock**

In `loadStoredSecrets`, after `storedSolarKey = secrets.getString("solar_key", "");` add:

```cpp
  storedOrionKey = secrets.getString("orion_key", "");
```

and after `cs::lockRestore(solarLock, ...);` add:

```cpp
  cs::lockRestore(orionLock, secrets.getString("orion_mac", "").c_str(), millis());
```

After the function `activeSolarKey()` add:

```cpp
static const char *activeOrionKey() {
  return storedOrionKey.c_str();
}
```

- [ ] **Step 8: Orion decoder**

Directly after the function `decodeVictronSolarCharger` add:

```cpp
static bool decodeVictronOrionXs(const std::string &manufacturerData, OrionStats &out, String &error) {
  if (manufacturerData.size() < 11) {
    error = "short advert";
    return false;
  }

  const uint8_t *data = reinterpret_cast<const uint8_t *>(manufacturerData.data());
  const uint8_t *payload = data + 2;
  size_t payloadLen = manufacturerData.size() - 2;

  if (payloadLen < 9 || payload[0] != VICTRON_PRODUCT_ADVERTISEMENT) {
    error = "not instant";
    return false;
  }

  if (payload[4] != VICTRON_ORION_XS_RECORD) {
    error = "not Orion record";
    return false;
  }

  uint8_t plain[16] = {};
  size_t encryptedLen = sizeof(plain);

  if (!decryptVictronPayload(payload, payloadLen, activeOrionKey(), plain, encryptedLen, error)) {
    return false;
  }

  cs::OrionXsReading reading;
  if (!cs::parseOrionXs(plain, encryptedLen, reading)) {
    error = "short data";
    return false;
  }

  out = OrionStats();
  out.deviceState = reading.state;
  out.errorCode = reading.error;
  out.outputVoltage = reading.outputVoltage;
  out.outputCurrent = reading.outputCurrent;
  out.inputVoltage = reading.inputVoltage;
  out.inputCurrent = reading.inputCurrent;
  out.offReason = reading.offReason;
  out.valid = true;
  out.error = "";

  logDecryptedBytes("Victron Orion XS", plain, encryptedLen);
  Serial.printf("Victron Orion XS decoded state=%u err=%u out=%.2fV %.1fA in=%.2fV %.1fA off=0x%08lX\n",
                out.deviceState,
                out.errorCode,
                out.outputVoltage,
                out.outputCurrent,
                out.inputVoltage,
                out.inputCurrent,
                static_cast<unsigned long>(out.offReason));
  return true;
}
```

- [ ] **Step 9: Orion advert handler and routing**

Directly after the function `handleSolarAdvert` add:

```cpp
static void handleOrionAdvert(const String &name,
                              const String &address,
                              const char *mac,
                              int rssi,
                              const std::string &manufacturerData) {
  if (!cs::lockAccepts(orionLock, mac)) {
    return;
  }

  uint32_t now = millis();
  cs::lockNoteSeen(orionLock, mac, now);
  orionSeen.name = name.length() ? name : "Orion XS";
  orionSeen.address = address;
  orionSeen.rssi = rssi;
  orionSeen.seenMs = now;
  lastOrionAdvertMs = now;

  OrionStats decoded;
  String error;
  bool ok = decodeVictronOrionXs(manufacturerData, decoded, error);
  if (ok && !cs::plausibleOptionalVoltage(decoded.outputVoltage)) {
    ok = false;
    error = "implausible";
  }

  if (ok) {
    orionStats = decoded;
    lastOrionDecodeMs = now;
    noteVictronGoodDecode(orionLock, "orion_mac", "Orion XS", mac, address);
  } else if (!orionStats.valid) {
    orionStats.error = error;
  }

  Serial.printf("Victron Orion XS %s RSSI %d %s data %s\n",
                address.c_str(),
                rssi,
                ok ? "decoded" : error.c_str(),
                bytesToHex(manufacturerData).c_str());
}
```

In `ScanCallbacks::onResult`, replace

```cpp
          } else if (recordType == VICTRON_SOLAR_CHARGER_RECORD) {
            handleSolarAdvert(name, address, mac, rssi, manufacturerData);
          }
```

with

```cpp
          } else if (recordType == VICTRON_SOLAR_CHARGER_RECORD) {
            handleSolarAdvert(name, address, mac, rssi, manufacturerData);
          } else if (recordType == VICTRON_ORION_XS_RECORD) {
            handleOrionAdvert(name, address, mac, rssi, manufacturerData);
          }
```

- [ ] **Step 10: Setup page key field and lock status**

In `settingsPageHtml`, replace the lock-status block added in Task 4:

```cpp
  html += F("<p class='hint'>Battery monitor: ");
  html += lockStatusText(shuntLock);
  html += F("<br>Solar controller: ");
  html += lockStatusText(solarLock);
  html += F("</p>");
```

with:

```cpp
  html += F("<label for='orion_key'>Orion XS key</label><input id='orion_key' name='orion_key' autocomplete='off' placeholder='");
  html += maskedKeyText(storedOrionKey);
  html += F("'>");
  html += F("<p class='hint'>Battery monitor: ");
  html += lockStatusText(shuntLock);
  html += F("<br>Solar controller: ");
  html += lockStatusText(solarLock);
  html += F("<br>Orion XS: ");
  html += lockStatusText(orionLock);
  html += F("</p>");
```

- [ ] **Step 11: Save, clear and info page**

In `handleSettingsSave`, after `String solarKey = normaliseKeyHex(settingsServer.arg("solar_key"));` add:

```cpp
  String orionKey = normaliseKeyHex(settingsServer.arg("orion_key"));
```

After the whole `if (solarKey.length()) { ... }` block add:

```cpp
  if (orionKey.length()) {
    if (isValidKeyHex(orionKey)) {
      if (orionKey != storedOrionKey) {
        releaseVictronLock(orionLock, "orion_mac", "Orion XS", "key changed");
      }
      storedOrionKey = orionKey;
      secrets.putString("orion_key", storedOrionKey);
      saved = true;
    } else {
      invalid = true;
    }
  }
```

In the `if (!demoModeEnabled) {` block inside `else if (saved) {` near the end of `handleSettingsSave`, after `solarStats.valid = false;` add `orionStats.valid = false;`.

In `handleSettingsClearKeys`: after `storedSolarKey = "";` add `storedOrionKey = "";`; after `secrets.remove("solar_key");` add `secrets.remove("orion_key");`; after the two `releaseVictronLock` calls add:

```cpp
  releaseVictronLock(orionLock, "orion_mac", "Orion XS", "keys cleared");
```

and inside its `if (!demoModeEnabled) {` block add `orionStats.valid = false;`.

In `drawSettingsInfoPage` replace

```cpp
  drawInfoLine(184, "Keys", storedVictronKey.length() || storedSolarKey.length() ? "saved" : "-");
```

with

```cpp
  drawInfoLine(184, "Keys", storedVictronKey.length() || storedSolarKey.length() || storedOrionKey.length() ? "saved" : "-");
```

- [ ] **Step 12: Watchdog**

In `updateBleWatchdog`, replace

```cpp
  } else if (lastVictronAdvertMs == 0 &&
             lastSolarAdvertMs == 0 &&
             now - lastBleScanStartMs > BLE_NEVER_SEEN_RESTART_MS) {
```

with

```cpp
  } else if (lastVictronAdvertMs == 0 &&
             lastSolarAdvertMs == 0 &&
             lastOrionAdvertMs == 0 &&
             now - lastBleScanStartMs > BLE_NEVER_SEEN_RESTART_MS) {
```

After the solar signal-lost block (`if (solarStats.valid && lastSolarDecodeMs > 0 && ...) { ... }`) add:

```cpp
  if (orionStats.valid && lastOrionDecodeMs > 0 && now - lastOrionDecodeMs > VICTRON_SIGNAL_LOST_MS) {
    orionStats.valid = false;
    orionStats.error = "signal lost";
    drawCurrentPage(true);
  }
```

After the solar lock-expiry block add:

```cpp
  if (cs::lockExpired(orionLock, now, VICTRON_LOCK_RELEASE_MS)) {
    releaseVictronLock(orionLock, "orion_mac", "Orion XS", "not seen for 10 min");
  }
```

- [ ] **Step 13: Page availability includes Orion**

In `pageAvailability()` replace

```cpp
  return cs::PageAvailability{demoModeEnabled, storedSolarKey.length() > 0, false};
```

with

```cpp
  return cs::PageAvailability{demoModeEnabled, storedSolarKey.length() > 0, storedOrionKey.length() > 0};
```

- [ ] **Step 14: Build and test**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`.
Run: `pio test -e native`
Expected: all tests pass.

- [ ] **Step 15: Commit**

```bash
git add include/chargescreen_logic.h test/test_orion/test_main.cpp src/main.cpp
git commit -m "Decode Victron Orion XS adverts with key, lock and setup page field

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Orion XS screen

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: Task 6 `OrionStats`, `orionStats`, `orionSeen`, `storedOrionKey`, `lastOrionAdvertMs`, `lastOrionDecodeMs`, `cs::orionOffReasonText`, `cs::chargerStateText`; existing `drawSegmentedGaugeArcAt`, `formatOneDecimalOrDash`, `formatWattsOrDash`, `drawTinyLabel`, `drawDeviceLine`, `drawSettingsButton`.
- Produces: page index 2 rendered by `static void drawOrionPage(bool force = false)`; `PAGE_COUNT` = 3.

- [ ] **Step 1: Palette and scale constants**

After `static constexpr uint16_t COLOR_SOLAR_DIM_TEXT = 0xD6AB;` add:

```cpp
static constexpr uint16_t COLOR_ORION_PANEL = 0x2887;
static constexpr uint16_t COLOR_ORION_TRACK = 0x598F;
static constexpr uint16_t COLOR_ORION_ARC = 0xB37F;
static constexpr uint16_t COLOR_ORION_TEXT = 0xF75F;
static constexpr uint16_t COLOR_ORION_DIM_TEXT = 0xCDBC;
static constexpr float ORION_FULL_SCALE_AMPS = 50.0f;
```

Replace `static constexpr uint8_t PAGE_COUNT = 2;` with `static constexpr uint8_t PAGE_COUNT = 3;`.

- [ ] **Step 2: Anti-aliased text background parameter**

Replace the whole function `drawAaText` with:

```cpp
static void drawAaText(const AaFont &font,
                       const String &text,
                       int16_t x,
                       int16_t y,
                       uint16_t color,
                       uint16_t background = COLOR_PANEL_BLUE) {
  int16_t cursorX = x;
  AaGlyph glyph;
  for (size_t i = 0; i < text.length(); i++) {
    if (!readAaGlyph(font, text[i], glyph)) {
      continue;
    }

    for (uint8_t py = 0; py < glyph.h; py++) {
      for (uint8_t px = 0; px < glyph.w; px++) {
        uint8_t alpha = pgm_read_byte(font.bitmap + glyph.offset + py * glyph.w + px);
        if (alpha == 0) {
          continue;
        }
        gfx->drawPixel(cursorX + glyph.xOffset + px,
                       y + glyph.yOffset + py,
                       blend565(color, background, alpha));
      }
    }
    cursorX += glyph.xAdvance;
  }
}
```

Replace the whole function `drawAaCentered` with:

```cpp
static void drawAaCentered(const AaFont &font,
                           const String &text,
                           int16_t centerX,
                           int16_t topY,
                           uint16_t color,
                           uint16_t background = COLOR_PANEL_BLUE) {
  int16_t width = aaTextWidth(font, text);
  drawAaText(font, text, centerX - width / 2, topY, color, background);
}
```

- [ ] **Step 3: Demo values**

After `static float demoSolarYieldKwh = 1.27f;` add:

```cpp
static float demoOrionOutputCurrent = 32.0f;
static float demoOrionOutputVoltage = 14.2f;
static float demoOrionInputVoltage = 13.4f;
static float demoOrionInputCurrent = 35.5f;
```

In `loadStoredSecrets`, after `demoSolarYieldKwh = secrets.getFloat("demo_yield", 1.27f);` add:

```cpp
  demoOrionOutputCurrent = secrets.getFloat("demo_oa", 32.0f);
  demoOrionOutputVoltage = secrets.getFloat("demo_ov", 14.2f);
  demoOrionInputVoltage = secrets.getFloat("demo_iv", 13.4f);
  demoOrionInputCurrent = secrets.getFloat("demo_ia", 35.5f);
```

In `settingsPageHtml`, replace

```cpp
  html += String(demoSolarYieldKwh, 2);
  html += F("'></div>");
```

with

```cpp
  html += String(demoSolarYieldKwh, 2);
  html += F("'><label for='demo_orion_a'>Orion out A</label><label for='demo_orion_ov'>Orion out V</label><input id='demo_orion_a' name='demo_orion_a' type='number' step='0.1' value='");
  html += String(demoOrionOutputCurrent, 1);
  html += F("'><input id='demo_orion_ov' name='demo_orion_ov' type='number' step='0.1' value='");
  html += String(demoOrionOutputVoltage, 1);
  html += F("'><label for='demo_orion_iv'>Orion in V</label><label for='demo_orion_ia'>Orion in A</label><input id='demo_orion_iv' name='demo_orion_iv' type='number' step='0.1' value='");
  html += String(demoOrionInputVoltage, 1);
  html += F("'><input id='demo_orion_ia' name='demo_orion_ia' type='number' step='0.1' value='");
  html += String(demoOrionInputCurrent, 1);
  html += F("'></div>");
```

In `handleSettingsSave`, after `demoSolarYieldKwh = constrain(settingsServer.arg("demo_yield").toFloat(), 0.0f, 99.0f);` add:

```cpp
  demoOrionOutputCurrent = constrain(settingsServer.arg("demo_orion_a").toFloat(), 0.0f, 100.0f);
  demoOrionOutputVoltage = constrain(settingsServer.arg("demo_orion_ov").toFloat(), 0.0f, 80.0f);
  demoOrionInputVoltage = constrain(settingsServer.arg("demo_orion_iv").toFloat(), 0.0f, 80.0f);
  demoOrionInputCurrent = constrain(settingsServer.arg("demo_orion_ia").toFloat(), 0.0f, 100.0f);
```

and after `secrets.putFloat("demo_yield", demoSolarYieldKwh);` add:

```cpp
  secrets.putFloat("demo_oa", demoOrionOutputCurrent);
  secrets.putFloat("demo_ov", demoOrionOutputVoltage);
  secrets.putFloat("demo_iv", demoOrionInputVoltage);
  secrets.putFloat("demo_ia", demoOrionInputCurrent);
```

- [ ] **Step 4: Redraw-tracking globals**

After `static String lastSolarErrorText;` add:

```cpp
static bool orionPageDrawn = false;
static String lastOrionCurrentText;
static String lastOrionInputVoltageText;
static String lastOrionOutputVoltageText;
static String lastOrionInputCurrentText;
static String lastOrionPowerText;
static String lastOrionStatusText;
static String lastOrionErrorText;
```

In `invalidateScreens`, `switchPage`, and `closeSettingsPage`, add `orionPageDrawn = false;` next to the existing `solarValuesDrawn = false;` line in each.

- [ ] **Step 5: Display helpers**

Directly after the function `displaySolarStatusText` add:

```cpp
static OrionStats displayOrionStats() {
  if (!demoModeEnabled) {
    return orionStats;
  }

  OrionStats stats;
  stats.valid = true;
  stats.deviceState = 3;
  stats.errorCode = 0;
  stats.outputVoltage = demoOrionOutputVoltage;
  stats.outputCurrent = demoOrionOutputCurrent;
  stats.inputVoltage = demoOrionInputVoltage;
  stats.inputCurrent = demoOrionInputCurrent;
  stats.offReason = 0;
  stats.error = "";
  return stats;
}

static String displayOrionStatusText(const OrionStats &stats) {
  if (demoModeEnabled) {
    return "Demo Orion";
  }

  if (!stats.valid) {
    if (!storedOrionKey.length()) {
      return "Add key via WiFi";
    }
    if (lastOrionAdvertMs > 0 && millis() - lastOrionAdvertMs < 10000) {
      return "Decode error";
    }
    return "Searching";
  }

  if (millis() - lastOrionDecodeMs > 10000) {
    return "Signal lost";
  }

  if (stats.errorCode != 0 && stats.errorCode != 0xFF) {
    return String("Error ") + String(stats.errorCode);
  }

  if (stats.deviceState == 0) {
    const char *reason = cs::orionOffReasonText(stats.offReason);
    return reason ? reason : "Off";
  }

  return cs::chargerStateText(stats.deviceState);
}
```

- [ ] **Step 6: Orion page**

Directly after the function `drawPageTwo` add:

```cpp
static void drawOrionPage(bool force = false) {
  OrionStats stats = displayOrionStats();
  String statusText = displayOrionStatusText(stats);
  float outputPower = isnan(stats.outputVoltage) || isnan(stats.outputCurrent)
                          ? NAN
                          : stats.outputVoltage * stats.outputCurrent;
  String currentText = stats.valid ? formatOneDecimalOrDash(stats.outputCurrent, "A") : "";
  String inputVoltageText = stats.valid ? formatOneDecimalOrDash(stats.inputVoltage, "V") : "";
  String outputVoltageText = stats.valid ? formatOneDecimalOrDash(stats.outputVoltage, "V") : "";
  String inputCurrentText = stats.valid ? formatOneDecimalOrDash(stats.inputCurrent, "A") : "";
  String powerText = stats.valid ? formatWattsOrDash(outputPower) : "";
  String errorText = stats.valid ? "" : (!storedOrionKey.length()
                                             ? "add key via WiFi"
                                             : (orionStats.error.length() ? orionStats.error : "no Orion BLE"));

  bool changed = force || !orionPageDrawn ||
                 currentText != lastOrionCurrentText ||
                 inputVoltageText != lastOrionInputVoltageText ||
                 outputVoltageText != lastOrionOutputVoltageText ||
                 inputCurrentText != lastOrionInputCurrentText ||
                 powerText != lastOrionPowerText ||
                 statusText != lastOrionStatusText ||
                 errorText != lastOrionErrorText;
  if (!changed) {
    return;
  }

  float ringCurrent = stats.valid && !isnan(stats.outputCurrent)
                          ? constrain(stats.outputCurrent, 0.0f, ORION_FULL_SCALE_AMPS)
                          : 0.0f;

  gfx->fillScreen(COLOR_BLACK_SOFT);
  gfx->fillCircle(120, 120, 120, COLOR_ORION_PANEL);
  drawSegmentedGaugeArcAt(120,
                          GAUGE_ARC_SWEEP_DEG * ringCurrent / ORION_FULL_SCALE_AMPS,
                          COLOR_ORION_TRACK,
                          COLOR_ORION_ARC,
                          COLOR_ORION_PANEL);
  gfx->fillCircle(120, 120, GAUGE_CLEAR_RADIUS, COLOR_ORION_PANEL);

  if (stats.valid) {
    if (showValueGrid) {
      gfx->drawFastVLine(120, 106, 72, COLOR_ORION_TRACK);
      gfx->drawFastHLine(120 - 67, 144, 134, COLOR_ORION_TRACK);
    }
    if (showValueLabels) {
      drawTinyLabel("CHARGE", 120, 42, COLOR_ORION_DIM_TEXT);
    }
    drawAaCentered(AA_FONT_LARGE, currentText, 120, 47, COLOR_ORION_TEXT, COLOR_ORION_PANEL);
    drawAaCentered(AA_FONT_SMALL, inputVoltageText, 78, 104, COLOR_ORION_TEXT, COLOR_ORION_PANEL);
    drawAaCentered(AA_FONT_SMALL, outputVoltageText, 164, 104, COLOR_ORION_TEXT, COLOR_ORION_PANEL);
    if (showValueLabels) {
      drawTinyLabel("INPUT", 78, 130, COLOR_ORION_DIM_TEXT);
      drawTinyLabel("OUTPUT", 164, 130, COLOR_ORION_DIM_TEXT);
    }
    drawAaCentered(AA_FONT_SMALL, inputCurrentText, 78, 147, COLOR_ORION_TEXT, COLOR_ORION_PANEL);
    drawAaCentered(AA_FONT_SMALL, powerText, 164, 147, COLOR_ORION_TEXT, COLOR_ORION_PANEL);
    if (showValueLabels) {
      drawTinyLabel("IN AMPS", 78, 173, COLOR_ORION_DIM_TEXT);
      drawTinyLabel("POWER", 164, 173, COLOR_ORION_DIM_TEXT);
    }
    drawAaCentered(AA_FONT_SMALL, statusText, 120, 185, COLOR_ORION_DIM_TEXT, COLOR_ORION_PANEL);
  } else {
    drawAaCentered(AA_FONT_LARGE, "-- A", 120, 50, COLOR_ORION_TEXT, COLOR_ORION_PANEL);
    drawAaCentered(AA_FONT_SMALL, statusText, 120, 106, COLOR_ORION_DIM_TEXT, COLOR_ORION_PANEL);
    drawDeviceLine(135, "Orion XS", orionSeen);
    if (errorText.length()) {
      gfx->setTextSize(1);
      gfx->setTextColor(0xF800);
      gfx->setCursor(74, 178);
      gfx->print(errorText.substring(0, 20));
    }
  }

  lastOrionCurrentText = currentText;
  lastOrionInputVoltageText = inputVoltageText;
  lastOrionOutputVoltageText = outputVoltageText;
  lastOrionInputCurrentText = inputCurrentText;
  lastOrionPowerText = powerText;
  lastOrionStatusText = statusText;
  lastOrionErrorText = errorText;
  drawSettingsButton(COLOR_ORION_PANEL, COLOR_ORION_TEXT);
  orionPageDrawn = true;
}
```

- [ ] **Step 7: Route page 2**

In `drawCurrentPage` replace

```cpp
  } else {
    drawPageTwo(force);
  }
```

with

```cpp
  } else if (currentPage == 1) {
    drawPageTwo(force);
  } else {
    drawOrionPage(force);
  }
```

- [ ] **Step 8: Build and test**

Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`; note the Flash/RAM lines.
Run: `pio test -e native`
Expected: all tests pass (22 tests across 3 suites).

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "Add violet Orion XS dashboard screen

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: README and device verification

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: documentation and the on-device checklist result.

- [ ] **Step 1: Update README**

In `README.md`, replace the section from `## Settings Hotspot` up to (not including) `## License` with:

```markdown
## Settings Hotspot

Tap the dots at the bottom of any dashboard screen to open Settings. The WiFi row starts a temporary hotspot named `ChargeScreen`. Connect to it and open `http://192.168.4.1/` to paste Victron Instant Readout keys for the battery monitor, solar controller, and Orion XS.

Keys entered there are stored in the ESP32 flash preferences area. No Victron keys are compiled into the firmware. The setup hotspot turns itself off after 5 minutes without webpage activity.

## Screens

- Battery (always shown), Solar (shown when a solar key is saved), Orion XS DC-DC (shown when an Orion key is saved). Demo mode shows all screens.
- Screen cycle: Off, 10, 20, 30, or 60 seconds, set on settings page 3 or on the setup page. Cycling pauses after a touch.

## Device Lock

Each Victron device kind locks to the first address that decodes correctly three times in a row, so nearby vans or boats with Victron gear are ignored. The lock is released if the device is not heard for 10 minutes or its key is changed. The setup page shows the locked address.

## Flashing

This fork is flashed over USB only (`pio run -t upload`); there is no web firmware update. It uses a single 3 MB app partition. Unit tests for the device-independent logic run with `pio test -e native`.
```

- [ ] **Step 2: Final checks**

Run: `pio test -e native`
Expected: all suites `PASSED`.
Run: `pio run -e esp32-c3-round-dashboard`
Expected: `[SUCCESS]`; Flash usage well under 3145728 bytes.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "Document screens, device lock and USB-only flashing

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 4: Flash and verify on the device (user, device connected over USB)**

Run: `pio run -e esp32-c3-round-dashboard -t upload` then `pio device monitor`.

Checklist (report PASS/FAIL for each):
1. Saved keys and settings are still present after flashing the new partition table.
2. Demo mode: battery, solar and Orion screens all render; the Orion screen is violet with no blue fringes on text.
3. Demo off, only a shunt key saved: swiping stays on the battery screen; adding a solar key makes the solar screen appear.
4. Cycle 10s: screens advance every 10 s; touching pauses for one interval; no cycling on settings or while the screen is off.
5. Serial log shows `Victron shunt locked to …` (and solar/Orion if present) within a few seconds; the setup page shows the locked address.
6. Changing a key on the setup page logs `lock released (key changed)` and the device relocks.
7. The setup page has no firmware upload form.

- [ ] **Step 5: Push**

Run: `git push origin firmware`
Expected: branch updated on `istrasoft/ChargeScreenX`.
