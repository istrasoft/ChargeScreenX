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

// Batch send interval in minutes; 0 = Off (no periodic batches).
inline uint8_t sanitiseBatchMin(int v) {
  return (v == 0 || v == 15 || v == 30 || v == 60 || v == 120 || v == 240) ? static_cast<uint8_t>(v) : 30;
}

// On-device Send row: 15m -> 30m -> 1h -> 2h -> 4h -> Off -> 15m.
inline uint8_t nextBatchMin(uint8_t v) {
  switch (sanitiseBatchMin(v)) {
    case 15:
      return 30;
    case 30:
      return 60;
    case 60:
      return 120;
    case 120:
      return 240;
    case 240:
      return 0;
    default:
      return 15;
  }
}

// "15m", "30m", "1h", "2h", "4h", or "-" for Off.
inline void batchMinText(uint8_t v, char *out, size_t cap) {
  uint8_t m = sanitiseBatchMin(v);
  if (m == 0) {
    snprintf(out, cap, "-");
  } else if (m < 60) {
    snprintf(out, cap, "%um", static_cast<unsigned>(m));
  } else {
    snprintf(out, cap, "%uh", static_cast<unsigned>(m / 60));
  }
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
  uint8_t orionState = 0xFF;  // Orion XS device state, 0 = Off
  float orionInV = NAN;       // Orion XS input voltage (starter battery or alternator)
};

constexpr float STARTER_MIN_V = 5.0f;   // below: input disconnected
constexpr float STARTER_MAX_V = 20.0f;

inline bool starterVoltageUsable(float v) {
  return std::isfinite(v) && v >= STARTER_MIN_V && v <= STARTER_MAX_V;
}

// The Orion XS follows the ignition: any known state other than Off means
// the engine (alternator) is running.
inline bool orionEngineRunning(uint8_t state) {
  return state != 0 && state != 0xFF;
}

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

enum class MsgKind : uint8_t {
  Alert = 0,
  Clear = 1,
  Morning = 2,
  Evening = 3,
  Batch = 4,
  Test = 5,
  Park = 6,
  Starter = 7,
  Moved = 8,
  Status = 9
};

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
    case MsgKind::Park:
      return "park";
    case MsgKind::Starter:
      return "starter";
    case MsgKind::Moved:
      return "moved";
    case MsgKind::Status:
      return "status";
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
    case MsgKind::Park:
      return "Parked";
    case MsgKind::Starter:
      return "Starter battery low";
    case MsgKind::Moved:
      return "Van moved?";
    case MsgKind::Status:
      return "Status report";
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
    case MsgKind::Park:
      return "parking";
    case MsgKind::Starter:
      return "battery";
    case MsgKind::Moved:
      return "rotating_light";
    case MsgKind::Status:
      return "clipboard";
    default:
      return "";
  }
}

// ntfy levels: 1 = min (silent), 2 = low (no sound), 3 = default, 4 = high.
inline uint8_t kindPriority(MsgKind k) {
  switch (k) {
    case MsgKind::Alert:
    case MsgKind::Starter:
    case MsgKind::Moved:
      return 4;
    case MsgKind::Batch:
      return 1;
    case MsgKind::Park:
      return 2;
    default:
      return 3;
  }
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
  return std::isfinite(v) && v >= 0.5f;  // printed with %.0f, so show only when it reads as 1 Ah or more
}

// yesterday may be null (no complete record of yesterday).
inline size_t buildMorningBody(char *out, size_t cap, float soc, float remaining, const DaysEstimate &est,
                               const DayStats *yesterday, bool hasSolarKey, bool hasOrionKey) {
  if (!socKnown(soc)) {
    return 0;
  }
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
    w.putf(" (delayed, happened at %s)", hm);
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

// ---------------------------------------------------------------- records

constexpr size_t BATCH_JSON_MAX = 2304;
constexpr size_t ENVELOPE_MAX = 4608;

constexpr uint16_t NO_SECONDS = 0xFFFF;  // engS / pvS: device had no valid sample

// One 5-minute record, stored as-is in /tx_cache.bin. NAN = no valid data.
struct Record {
  uint32_t t = 0;        // start, unix seconds; 0 until the clock is known
  uint32_t startMs = 0;  // millis() at the start, meaningful while boot matches
  uint16_t boot = 0;
  uint16_t engS = NO_SECONDS;  // seconds with the engine running
  float soc = NAN;
  float used = NAN;
  float i = NAN;
  float imin = NAN;
  float imax = NAN;
  float pv = NAN;
  float pvmax = NAN;
  float orion = NAN;
  float sv = NAN;             // average Orion input voltage
  uint16_t pvS = NO_SECONDS;  // seconds with PV above PV_PRODUCING_WATTS
  uint16_t reserved = 0;
  float sa = NAN;             // average solar charger battery current
};
static_assert(sizeof(Record) == 56, "Record layout is stored on flash");

struct RecordBuilder {
  Record rec;
  uint16_t shuntCount = 0;
  uint16_t pvCount = 0;
  uint16_t orionCount = 0;
  uint16_t orionSeen = 0;    // valid Orion samples (state known)
  uint16_t engSamples = 0;   // of those, engine running
  uint16_t pvOnSamples = 0;  // valid PV samples above PV_PRODUCING_WATTS
  uint16_t svCount = 0;
  uint16_t saCount = 0;
  float saSum = 0.0f;
  float currentSum = 0.0f;
  float pvSum = 0.0f;
  float orionSum = 0.0f;
  float svSum = 0.0f;
};

inline uint16_t samplesToSeconds(uint16_t samples) {
  uint32_t s = static_cast<uint32_t>(samples) * (SAMPLE_PERIOD_MS / 1000U);
  return static_cast<uint16_t>(s < NO_SECONDS ? s : NO_SECONDS - 1);
}

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
    if (s.pvW > PV_PRODUCING_WATTS) {
      b.pvOnSamples++;
    }
  }
  if (s.solarValid && std::isfinite(s.solarBatA)) {
    b.saSum += s.solarBatA;
    b.saCount++;
  }
  if (s.orionValid && std::isfinite(s.orionOutA)) {
    b.orionSum += s.orionOutA;
    b.orionCount++;
  }
  if (s.orionValid) {
    b.orionSeen++;
    if (orionEngineRunning(s.orionState)) {
      b.engSamples++;
    }
    if (starterVoltageUsable(s.orionInV)) {
      b.svSum += s.orionInV;
      b.svCount++;
    }
  }
}

inline Record recordClose(const RecordBuilder &b) {
  Record r = b.rec;
  r.i = b.shuntCount ? b.currentSum / b.shuntCount : NAN;
  r.pv = b.pvCount ? b.pvSum / b.pvCount : NAN;
  r.orion = b.orionCount ? b.orionSum / b.orionCount : NAN;
  r.engS = b.orionSeen ? samplesToSeconds(b.engSamples) : NO_SECONDS;
  r.pvS = b.pvCount ? samplesToSeconds(b.pvOnSamples) : NO_SECONDS;
  r.sv = b.svCount ? b.svSum / b.svCount : NAN;
  r.sa = b.saCount ? b.saSum / b.saCount : NAN;
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

// netJson: optional {"type":..} object from buildNetJson, added as "net" after the records.
inline size_t buildBatchJson(char *out, size_t cap, const Record *recs, uint8_t count,
                             const char *netJson = nullptr) {
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
    if (r.engS != NO_SECONDS) {
      w.putf(",\"eng\":%u", static_cast<unsigned>(r.engS));
    }
    if (r.pvS != NO_SECONDS) {
      w.putf(",\"pvs\":%u", static_cast<unsigned>(r.pvS));
    }
    putField1(w, "sv", r.sv);
    putField1(w, "sa", r.sa);
    w.putChar('}');
  }
  w.putChar(']');
  if (netJson && netJson[0]) {
    w.putStr(",\"net\":");
    w.putStr(netJson);
  }
  w.putChar('}');
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

// {"topic":..,"title":..,"message":..,"priority":N[,"tags":[tag]][,"click":url]}
inline size_t buildNtfyEnvelope(char *out, size_t cap, const char *topic, const char *title, const char *message,
                                uint8_t priority, const char *tag, const char *click = nullptr) {
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
  if (click && click[0]) {
    w.putStr(",\"click\":\"");
    w.putEscaped(click);
    w.putChar('"');
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
  if (code == 401 || code == 403) {
    return NetResult::Fail;  // missing or wrong access token: keep the message and retry
  }
  if (code >= 400 && code < 500) {
    return NetResult::Drop;
  }
  return NetResult::Fail;
}

constexpr size_t POST_TOKEN_MAX = 127;

// Sent as "Authorization: Bearer <token>". Empty means no token. Only printable
// ASCII without spaces is accepted, so the token cannot break out of the header.
inline bool validPostToken(const char *token) {
  size_t len = 0;
  for (; token[len]; len++) {
    unsigned char c = static_cast<unsigned char>(token[len]);
    if (c <= ' ' || c >= 0x7f || len >= POST_TOKEN_MAX) {
      return false;
    }
  }
  return true;
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
  // Bound the search to this object (no nested braces in real responses).
  const char *close = strchr(current, '}');
  if (!close) {
    return false;
  }
  const char *seconds = jsonValueAfterKey(current, "seconds");
  if (!seconds || seconds > close) {
    return false;
  }
  return jsonReadOffset(seconds, offsetS);
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

// ---------------------------------------------------------------- engine and parking

constexpr uint32_t ENGINE_RUN_MIN_MS = 10UL * 60000UL;  // shorter runs are not a drive
constexpr uint32_t PARK_SETTLE_MS = 3UL * 60000UL;      // engine off this long ends the drive
constexpr uint32_t ENGINE_INVALID_DROP_MS = 60000UL;    // Orion lost this long drops a drive

struct EngineTracker {
  bool inRun = false;
  uint32_t runStartMs = 0;
  uint32_t lastRunningMs = 0;
  uint32_t lastValidMs = 0;
};

struct ParkTrigger {
  bool fired = false;
  uint32_t runS = 0;   // first to last running sample
  uint32_t offMs = 0;  // millis() of the last running sample
};

// Feed every 10 s sample. Off periods shorter than PARK_SETTLE_MS stay part
// of the drive; a drive of ENGINE_RUN_MIN_MS or more fires once when the
// engine has been off for PARK_SETTLE_MS.
inline ParkTrigger engineUpdate(EngineTracker &e, bool valid, bool running, uint32_t nowMs) {
  ParkTrigger t;
  if (!valid) {
    if (e.inRun && nowMs - e.lastValidMs > ENGINE_INVALID_DROP_MS) {
      e.inRun = false;
    }
    return t;
  }
  e.lastValidMs = nowMs;
  if (running) {
    if (!e.inRun) {
      e.inRun = true;
      e.runStartMs = nowMs;
    }
    e.lastRunningMs = nowMs;
    return t;
  }
  if (e.inRun && nowMs - e.lastRunningMs >= PARK_SETTLE_MS) {
    e.inRun = false;
    uint32_t runMs = e.lastRunningMs - e.runStartMs;
    if (runMs >= ENGINE_RUN_MIN_MS) {
      t.fired = true;
      t.runS = runMs / 1000U;
      t.offMs = e.lastRunningMs;
    }
  }
  return t;
}

// Handed from telemetry (loop()) to netlink.
struct ParkRequest {
  uint32_t offUnix = 0;  // 0 = clock unknown at engine off
  uint32_t offMs = 0;
  uint32_t runS = 0;
  uint16_t boot = 0;
  uint8_t attempts = 0;
};

inline void putDuration(TextWriter &w, uint32_t seconds) {
  uint32_t minutes = (seconds + 30U) / 60U;
  if (minutes >= 60U) {
    w.putf("%lu h %lu min", static_cast<unsigned long>(minutes / 60U), static_cast<unsigned long>(minutes % 60U));
  } else {
    w.putf("%lu min", static_cast<unsigned long>(minutes));
  }
}

// ---------------------------------------------------------------- starter battery

constexpr uint32_t STARTER_SETTLE_MS = 60UL * 60000UL;  // ignore the first hour after engine off
constexpr uint32_t STARTER_LOW_MS = 30UL * 60000UL;     // low (or recovered) this long
constexpr float STARTER_LOW_V = 12.2f;
constexpr float STARTER_RECOVER_V = 12.6f;

struct StarterWatch {
  bool offKnown = false;
  uint32_t offSinceMs = 0;
  bool engineSeen = false;  // offSinceMs is a real engine-off time
  bool lowActive = false;
  uint32_t lowSinceMs = 0;
  float lowMin = NAN;
  bool highActive = false;
  uint32_t highSinceMs = 0;
  bool alerted = false;
};

struct StarterAlert {
  bool fired = false;
  float volts = NAN;  // lowest voltage in the low period
  bool offForKnown = false;
  uint32_t offForS = 0;
};

inline StarterAlert starterUpdate(StarterWatch &w, bool valid, uint8_t state, float inputV, uint32_t nowMs) {
  StarterAlert a;
  if (!valid) {
    return a;
  }
  if (orionEngineRunning(state)) {
    w = StarterWatch();  // re-arm and restart the settle time
    w.engineSeen = true;
    return a;
  }
  if (!w.offKnown) {
    w.offKnown = true;
    w.offSinceMs = nowMs;
  }
  if (!starterVoltageUsable(inputV) || nowMs - w.offSinceMs < STARTER_SETTLE_MS) {
    return a;
  }
  if (inputV < STARTER_LOW_V) {
    w.highActive = false;
    if (!w.lowActive) {
      w.lowActive = true;
      w.lowSinceMs = nowMs;
      w.lowMin = inputV;
    } else if (inputV < w.lowMin) {
      w.lowMin = inputV;
    }
    if (!w.alerted && nowMs - w.lowSinceMs >= STARTER_LOW_MS) {
      w.alerted = true;
      a.fired = true;
      a.volts = w.lowMin;
      a.offForKnown = w.engineSeen;
      a.offForS = (nowMs - w.offSinceMs) / 1000U;
    }
    return a;
  }
  w.lowActive = false;
  if (inputV >= STARTER_RECOVER_V) {
    if (!w.highActive) {
      w.highActive = true;
      w.highSinceMs = nowMs;
    } else if (w.alerted && nowMs - w.highSinceMs >= STARTER_LOW_MS) {
      w.alerted = false;
    }
  } else {
    w.highActive = false;
  }
  return a;
}

inline size_t buildStarterBody(char *out, size_t cap, const StarterAlert &a) {
  TextWriter w(out, cap);
  w.putf("Starter battery at %.1f V", static_cast<double>(a.volts));
  if (a.offForKnown) {
    w.putStr(", engine off for ");
    putDuration(w, a.offForS);
  }
  w.putStr(". Start the engine or charge it soon.");
  return w.result();
}

constexpr const char *MOVED_BODY =
    "The Wi-Fi networks around the van changed while the engine was off. It may have been moved.";

// ---------------------------------------------------------------- Wi-Fi geolocation

constexpr uint8_t GEO_SCAN_MAX = 32;   // usable access points kept from one scan
constexpr uint8_t GEO_KEEP_APS = 15;   // sent to Google and saved as the reference
constexpr uint8_t GEO_COMPARE_APS = 10;
constexpr uint8_t GEO_MIN_APS = 2;
constexpr uint8_t GEO_MAX_CALLS_PER_DAY = 20;
constexpr uint8_t PARK_MAX_ATTEMPTS = 3;
constexpr uint8_t WATCH_APS = 10;
constexpr uint8_t WATCH_MIN_APS = 3;
constexpr uint8_t GEO_STATE_VERSION = 1;
constexpr size_t GEO_KEY_MAX = 64;
constexpr const char *GEO_URL_PREFIX = "https://www.googleapis.com/geolocation/v1/geolocate?key=";

struct ApInfo {
  uint8_t bssid[6] = {};
  int8_t rssi = -127;
  uint8_t channel = 0;
};

// Own networks (they travel with the van), locally administered MACs (phone
// hotspots, randomised MACs) and "_nomap" networks are never used.
inline bool apUsable(const char *ssid, const uint8_t *bssid, const char *own1, const char *own2) {
  if (bssid[0] & 0x02) {
    return false;
  }
  if (ssid && ssid[0]) {
    if (own1 && own1[0] && strcmp(ssid, own1) == 0) {
      return false;
    }
    if (own2 && own2[0] && strcmp(ssid, own2) == 0) {
      return false;
    }
    size_t len = strlen(ssid);
    if (len >= 6 && strcmp(ssid + len - 6, "_nomap") == 0) {
      return false;
    }
  }
  return true;
}

// Sorts strongest first; returns how many to keep.
inline uint8_t apKeepStrongest(ApInfo *aps, uint8_t n, uint8_t keep) {
  for (uint8_t i = 1; i < n; i++) {
    ApInfo v = aps[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && aps[j].rssi < v.rssi) {
      aps[j + 1] = aps[j];
      j--;
    }
    aps[j + 1] = v;
  }
  return n < keep ? n : keep;
}

// How many of the first `compare` access points appear in ref.
inline uint8_t apOverlapCount(const ApInfo *aps, uint8_t n, const uint8_t (*ref)[6], uint8_t refCount,
                              uint8_t compare) {
  uint8_t k = n < compare ? n : compare;
  uint8_t matches = 0;
  for (uint8_t i = 0; i < k; i++) {
    for (uint8_t j = 0; j < refCount; j++) {
      if (memcmp(aps[i].bssid, ref[j], 6) == 0) {
        matches++;
        break;
      }
    }
  }
  return matches;
}

// Stored as-is in /lastfix.bin.
struct GeoState {
  uint8_t version = GEO_STATE_VERSION;
  uint8_t refCount = 0;
  uint8_t calls = 0;
  uint8_t refused = 0;
  int32_t budgetDay = NO_DAY;
  float lat = NAN;
  float lon = NAN;
  uint32_t acc = 0;
  uint32_t fixUnix = 0;
  uint8_t ref[GEO_KEEP_APS][6] = {};
  uint8_t watchCount = 0;
  uint8_t watchActive = 0;
  uint8_t watchRef[WATCH_APS][6] = {};
};

inline bool geoStateValid(const GeoState &g) {
  return g.version == GEO_STATE_VERSION && g.refCount <= GEO_KEEP_APS && g.watchCount <= WATCH_APS;
}

// At least half of the strongest (rounded up) were in the last Google fix.
inline bool samePlace(const GeoState &g, const ApInfo *aps, uint8_t n) {
  if (g.refCount == 0 || n == 0 || !std::isfinite(g.lat) || !std::isfinite(g.lon)) {
    return false;
  }
  uint8_t k = n < GEO_COMPARE_APS ? n : GEO_COMPARE_APS;
  return apOverlapCount(aps, n, g.ref, g.refCount, GEO_COMPARE_APS) * 2U >= k;
}

inline bool geoBudgetAllows(GeoState &g, int32_t today) {
  if (g.budgetDay != today) {
    g.budgetDay = today;
    g.calls = 0;
    g.refused = 0;
  }
  return !g.refused && g.calls < GEO_MAX_CALLS_PER_DAY;
}

enum class ParkSrc : uint8_t { Google, Same, None, Quota, Error };
enum class GeoStep : uint8_t { Saved, Skip, Call };

inline const char *parkSrcName(ParkSrc s) {
  switch (s) {
    case ParkSrc::Google:
      return "google";
    case ParkSrc::Same:
      return "same";
    case ParkSrc::Quota:
      return "quota";
    case ParkSrc::Error:
      return "error";
    default:
      return "none";
  }
}

// Same place first (no call), then too few networks or no key, then the budget.
inline GeoStep geoDecide(GeoState &g, const ApInfo *aps, uint8_t n, bool haveKey, int32_t today, ParkSrc &skipSrc) {
  if (samePlace(g, aps, n)) {
    return GeoStep::Saved;
  }
  if (n < GEO_MIN_APS || !haveKey) {
    skipSrc = ParkSrc::None;
    return GeoStep::Skip;
  }
  if (!geoBudgetAllows(g, today)) {
    skipSrc = ParkSrc::Quota;
    return GeoStep::Skip;
  }
  return GeoStep::Call;
}

inline void geoRemember(GeoState &g, const ApInfo *aps, uint8_t n, double lat, double lon, double acc,
                        uint32_t unixNow) {
  uint8_t k = n < GEO_KEEP_APS ? n : GEO_KEEP_APS;
  for (uint8_t i = 0; i < k; i++) {
    memcpy(g.ref[i], aps[i].bssid, 6);
  }
  g.refCount = k;
  g.lat = static_cast<float>(lat);
  g.lon = static_cast<float>(lon);
  g.acc = acc > 0.0 ? static_cast<uint32_t>(acc + 0.5) : 0;
  g.fixUnix = unixNow;
}

inline void watchSetReference(GeoState &g, const ApInfo *aps, uint8_t n) {
  uint8_t k = n < WATCH_APS ? n : WATCH_APS;
  for (uint8_t i = 0; i < k; i++) {
    memcpy(g.watchRef[i], aps[i].bssid, 6);
  }
  g.watchCount = k;
  g.watchActive = k >= WATCH_MIN_APS ? 1 : 0;
}

// Enough networks now and none of the strongest seen at the parking.
inline bool watchSuspicious(const GeoState &g, const ApInfo *aps, uint8_t n) {
  return g.watchActive && g.watchCount >= WATCH_MIN_APS && n >= WATCH_MIN_APS &&
         apOverlapCount(aps, n, g.watchRef, g.watchCount, WATCH_APS) == 0;
}

// Empty is valid (no key). Google keys use letters, digits, '-' and '_'.
inline bool validGeoKey(const char *key) {
  size_t len = 0;
  for (; key[len]; len++) {
    char c = key[len];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok || len >= GEO_KEY_MAX) {
      return false;
    }
  }
  return true;
}

inline bool buildGeoUrl(const char *key, char *out, size_t cap) {
  if (!key[0] || !validGeoKey(key)) {
    return false;
  }
  TextWriter w(out, cap);
  w.putStr(GEO_URL_PREFIX);
  w.putStr(key);
  return w.result() > 0;
}

inline size_t buildGeoRequest(char *out, size_t cap, const ApInfo *aps, uint8_t n) {
  TextWriter w(out, cap);
  w.putStr("{\"considerIp\":false,\"wifiAccessPoints\":[");
  for (uint8_t k = 0; k < n; k++) {
    const uint8_t *b = aps[k].bssid;
    w.putf("%s{\"macAddress\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"signalStrength\":%d,\"channel\":%u}", k ? "," : "",
           b[0], b[1], b[2], b[3], b[4], b[5], static_cast<int>(aps[k].rssi), static_cast<unsigned>(aps[k].channel));
  }
  w.putStr("]}");
  return w.result();
}

inline bool jsonReadNumber(const char *p, double &out) {
  if (!p) {
    return false;
  }
  if (*p == '"') {
    p++;  // some firmwares quote numbers
  }
  char *end = nullptr;
  double v = strtod(p, &end);
  if (end == p || !std::isfinite(v)) {
    return false;
  }
  out = v;
  return true;
}

inline bool jsonStringIs(const char *p, const char *value) {
  if (!p || *p != '"') {
    return false;
  }
  size_t n = strlen(value);
  return strncmp(p + 1, value, n) == 0 && p[1 + n] == '"';
}

// Points just past the '}' that closes the object starting at p, or nullptr.
inline const char *jsonObjectEnd(const char *p) {
  if (!p || *p != '{') {
    return nullptr;
  }
  int depth = 0;
  bool inString = false;
  for (; *p; ++p) {
    if (inString) {
      if (*p == '\\' && p[1]) {
        ++p;
      } else if (*p == '"') {
        inString = false;
      }
    } else if (*p == '"') {
      inString = true;
    } else if (*p == '{') {
      depth++;
    } else if (*p == '}') {
      if (--depth == 0) {
        return p + 1;
      }
    }
  }
  return nullptr;
}

// Value of key inside the object [obj, end), or nullptr.
inline const char *jsonValueIn(const char *obj, const char *end, const char *key) {
  const char *v = jsonValueAfterKey(obj, key);
  return (v && v < end) ? v : nullptr;
}

enum class GeoOutcome : uint8_t { Fix, NotFound, Quota, Error, Retry };

inline GeoOutcome parseGeoResponse(int code, const char *body, double &lat, double &lon, double &acc) {
  if (code <= 0 || code >= 500) {
    return GeoOutcome::Retry;
  }
  if (code == 200) {
    const char *loc = jsonValueAfterKey(body, "location");
    const char *locEnd = jsonObjectEnd(loc);
    if (locEnd && jsonReadNumber(jsonValueIn(loc, locEnd, "lat"), lat) &&
        jsonReadNumber(jsonValueIn(loc, locEnd, "lng"), lon) && jsonReadNumber(jsonValueAfterKey(body, "accuracy"), acc) &&
        lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0 && acc >= 0.0) {
      return GeoOutcome::Fix;
    }
    return GeoOutcome::Error;
  }
  const char *reason = jsonValueAfterKey(body, "reason");
  if (code == 429 || jsonStringIs(reason, "dailyLimitExceeded") || jsonStringIs(reason, "userRateLimitExceeded") ||
      jsonStringIs(reason, "rateLimitExceeded")) {
    return GeoOutcome::Quota;
  }
  if (code == 404 || jsonStringIs(reason, "notFound")) {
    return GeoOutcome::NotFound;
  }
  return GeoOutcome::Error;
}

// ---------------------------------------------------------------- Deco 4G signal

constexpr size_t DECO_PASS_MAX = 64;

struct DecoSignal {
  bool valid = false;
  char net[16] = {};  // Deco network_type, e.g. "lte_plus"
  float rsrp = NAN;
  float rsrq = NAN;
  float snr = NAN;
  float rssi = NAN;
  float lvl = NAN;   // Deco's own signal_strength, 0-100
  double du = NAN;   // Deco data_usage counter (unit unconfirmed, resettable)
};

// Empty is valid (feature off). Printable ASCII only.
inline bool validDecoPassword(const char *pass) {
  size_t len = 0;
  for (; pass[len]; len++) {
    unsigned char c = static_cast<unsigned char>(pass[len]);
    if (c < 0x20 || c >= 0x7f || len >= DECO_PASS_MAX) {
      return false;
    }
  }
  return true;
}

inline float jsonNumberOrNan(const char *p) {
  double v = 0;
  return jsonReadNumber(p, v) ? static_cast<float>(v) : NAN;
}

// Decrypted reply of admin/network?form=internet:
// {"error_code":0,"result":{"mobile_cpe":{"network_type":"lte","rsrp":-95,...}}}
inline bool parseDecoSignal(const char *json, DecoSignal &sig) {
  sig = DecoSignal();
  double code = -1;
  if (!jsonReadNumber(jsonValueAfterKey(json, "error_code"), code) || code != 0) {
    return false;
  }
  const char *cpe = jsonValueAfterKey(json, "mobile_cpe");
  const char *end = jsonObjectEnd(cpe);
  if (!end) {
    return false;
  }
  const char *net = jsonValueIn(cpe, end, "network_type");
  if (net && *net == '"') {
    size_t i = 0;
    for (const char *p = net + 1; *p && *p != '"' && i + 1 < sizeof(sig.net); ++p) {
      char c = *p;
      bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      if (ok) {
        sig.net[i++] = c;
      }
    }
    sig.net[i] = '\0';
  }
  sig.rsrp = jsonNumberOrNan(jsonValueIn(cpe, end, "rsrp"));
  sig.rsrq = jsonNumberOrNan(jsonValueIn(cpe, end, "rsrq"));
  sig.snr = jsonNumberOrNan(jsonValueIn(cpe, end, "snr"));
  sig.rssi = jsonNumberOrNan(jsonValueIn(cpe, end, "rssi"));
  sig.lvl = jsonNumberOrNan(jsonValueIn(cpe, end, "signal_strength"));
  double du = 0;
  if (jsonReadNumber(jsonValueIn(cpe, end, "data_usage"), du) && du >= 0) {
    sig.du = du;
  }
  sig.valid = sig.net[0] || std::isfinite(sig.rsrp) || std::isfinite(sig.rssi) || std::isfinite(sig.lvl);
  return sig.valid;
}

// ---------------------------------------------------------------- network quality

// "5G", "4G+", "4G", "3G", "2G", or "" when unknown.
inline const char *netTypeLabel(const char *t) {
  if (!t || !t[0]) {
    return "";
  }
  if (strncmp(t, "5g", 2) == 0 || strncmp(t, "nr", 2) == 0) {
    return "5G";
  }
  if (strcmp(t, "lte_plus") == 0 || strcmp(t, "lte_a") == 0 || strcmp(t, "lte-a") == 0) {
    return "4G+";
  }
  if (strncmp(t, "lte", 3) == 0) {
    return "4G";
  }
  const char *g3[] = {"wcdma", "umts", "hspa", "hsdpa", "hsupa", "td_scdma", "3g"};
  for (const char *p : g3) {
    if (strncmp(t, p, strlen(p)) == 0) {
      return "3G";
    }
  }
  const char *g2[] = {"gsm", "gprs", "edge", "2g"};
  for (const char *p : g2) {
    if (strncmp(t, p, strlen(p)) == 0) {
      return "2G";
    }
  }
  return "";
}

inline float scale01(float v, float lo, float hi) {
  float s = (v - lo) / (hi - lo);
  return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

// Coverage 0-100: how well the van reaches the mast (RSRP; RSSI on 3G/2G). -1 = unknown.
inline int coverageScore(const DecoSignal &s) {
  if (std::isfinite(s.rsrp)) {
    return static_cast<int>(std::lround(100.0f * scale01(s.rsrp, -120.0f, -80.0f)));
  }
  if (std::isfinite(s.rssi)) {
    return static_cast<int>(std::lround(100.0f * scale01(s.rssi, -110.0f, -60.0f)));
  }
  return -1;
}

// Quality 0-100: interference and cell load (60% SNR, 40% RSRQ). -1 = unknown.
inline int qualityScore(const DecoSignal &s) {
  bool hasSnr = std::isfinite(s.snr);
  bool hasRsrq = std::isfinite(s.rsrq);
  float snr = hasSnr ? scale01(s.snr, -5.0f, 20.0f) : 0.0f;
  float rsrq = hasRsrq ? scale01(s.rsrq, -20.0f, -5.0f) : 0.0f;
  float q;
  if (hasSnr && hasRsrq) {
    q = 0.6f * snr + 0.4f * rsrq;
  } else if (hasSnr) {
    q = snr;
  } else if (hasRsrq) {
    q = rsrq;
  } else {
    return -1;
  }
  return static_cast<int>(std::lround(100.0f * q));
}

constexpr const char *LATENCY_URL = "http://connectivitycheck.gstatic.com/generate_204";
constexpr const char *SPEED_URL = "https://speed.cloudflare.com/__down?bytes=102400";
constexpr uint32_t SPEED_BYTES = 102400;
constexpr uint32_t SPEED_INTERVAL_MS = 4UL * 3600000UL;  // while parked

// One reading of the mobile connection: Deco radio values plus measured lag and speed.
struct NetReading {
  DecoSignal sig;
  int32_t ms = -1;    // median HTTP round trip, -1 = not measured
  int32_t kbps = -1;  // 100 KB download speed, -1 = not measured
};

inline bool netReadingAny(const NetReading &r) {
  return r.sig.valid || r.ms >= 0 || r.kbps >= 0;
}

// Median of the non-negative values; -1 when fewer than 2 succeeded.
inline int32_t medianMs(const int32_t *v, uint8_t n) {
  int32_t ok[8];
  uint8_t k = 0;
  for (uint8_t i = 0; i < n && k < 8; i++) {
    if (v[i] >= 0) {
      ok[k++] = v[i];
    }
  }
  if (k < 2) {
    return -1;
  }
  for (uint8_t i = 1; i < k; i++) {
    int32_t x = ok[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && ok[j] > x) {
      ok[j + 1] = ok[j];
      j--;
    }
    ok[j + 1] = x;
  }
  return k % 2 ? ok[k / 2] : (ok[k / 2 - 1] + ok[k / 2]) / 2;
}

// bytes in ms -> kbit/s (bits per millisecond).
inline int32_t throughputKbps(uint32_t bytes, uint32_t ms) {
  if (ms == 0 || bytes == 0) {
    return -1;
  }
  return static_cast<int32_t>((static_cast<uint64_t>(bytes) * 8U) / ms);
}

// {"type":"4G+","cov":60,"qual":25,"ms":48,"kbps":2400,"rsrp":-96,...}; 0 when there is nothing.
inline size_t buildNetJson(char *out, size_t cap, const NetReading &r) {
  TextWriter w(out, cap);
  if (!netReadingAny(r)) {
    return 0;
  }
  w.putChar('{');
  bool first = true;
  auto sep = [&]() {
    if (!first) {
      w.putChar(',');
    }
    first = false;
  };
  const char *type = netTypeLabel(r.sig.net);
  if (type[0]) {
    sep();
    w.putf("\"type\":\"%s\"", type);
  }
  int cov = r.sig.valid ? coverageScore(r.sig) : -1;
  int qual = r.sig.valid ? qualityScore(r.sig) : -1;
  if (cov >= 0) {
    sep();
    w.putf("\"cov\":%d", cov);
  }
  if (qual >= 0) {
    sep();
    w.putf("\"qual\":%d", qual);
  }
  if (r.ms >= 0) {
    sep();
    w.putf("\"ms\":%ld", static_cast<long>(r.ms));
  }
  if (r.kbps >= 0) {
    sep();
    w.putf("\"kbps\":%ld", static_cast<long>(r.kbps));
  }
  if (r.sig.valid) {
    const char *keys[] = {"rsrp", "rsrq", "snr", "rssi", "lvl"};
    const float vals[] = {r.sig.rsrp, r.sig.rsrq, r.sig.snr, r.sig.rssi, r.sig.lvl};
    for (int k = 0; k < 5; k++) {
      if (std::isfinite(vals[k])) {
        sep();
        if (k == 2) {
          w.putf("\"%s\":%.1f", keys[k], static_cast<double>(roundTo(vals[k], 10.0f)));
        } else {
          w.putf("\"%s\":%.0f", keys[k], static_cast<double>(roundTo(vals[k], 1.0f)));
        }
      }
    }
    if (std::isfinite(r.sig.du)) {
      sep();
      w.putf("\"du\":%.0f", r.sig.du);
    }
  }
  w.putChar('}');
  return w.result();
}

// " · 4G+ cov 60 qual 25 · 48 ms · 2.4 Mbit/s" from a net JSON object [obj, end).
inline void putNetText(TextWriter &w, const char *obj, const char *end) {
  const char *type = jsonValueIn(obj, end, "type");
  double cov = 0, qual = 0, ms = 0, kbps = 0;
  bool hasCov = jsonReadNumber(jsonValueIn(obj, end, "cov"), cov);
  bool hasQual = jsonReadNumber(jsonValueIn(obj, end, "qual"), qual);
  if ((type && *type == '"') || hasCov || hasQual) {
    w.putStr(" \xC2\xB7 ");
    if (type && *type == '"') {
      for (const char *p = type + 1; *p && *p != '"'; ++p) {
        w.putChar(*p);
      }
    } else {
      w.putStr("mobile");
    }
    if (hasCov) {
      w.putf(" cov %.0f", cov);
    }
    if (hasQual) {
      w.putf(" qual %.0f", qual);
    }
  }
  if (jsonReadNumber(jsonValueIn(obj, end, "ms"), ms)) {
    w.putf(" \xC2\xB7 %.0f ms", ms);
  }
  if (jsonReadNumber(jsonValueIn(obj, end, "kbps"), kbps)) {
    if (kbps >= 1000.0) {
      w.putf(" \xC2\xB7 %.1f Mbit/s", kbps / 1000.0);
    } else {
      w.putf(" \xC2\xB7 %.0f kbit/s", kbps);
    }
  }
}

// Deco request signature text; is the plaintext RSA-encrypted in 53-byte pieces.
inline size_t buildDecoSignText(char *out, size_t cap, bool login, const char *aesKey, const char *aesIv,
                                const char *md5Hex, uint32_t seq, size_t dataLen) {
  TextWriter w(out, cap);
  if (login) {
    w.putf("k=%s&i=%s&", aesKey, aesIv);
  }
  w.putf("h=%s&s=%lu", md5Hex, static_cast<unsigned long>(seq + dataLen));
  return w.result();
}

// application/x-www-form-urlencoded value (base64 needs + / = escaped).
inline size_t urlEncode(char *out, size_t cap, const char *in) {
  TextWriter w(out, cap);
  for (; *in && !w.overflow; ++in) {
    unsigned char c = static_cast<unsigned char>(*in);
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                 c == '.' || c == '~';
    if (plain) {
      w.putChar(static_cast<char>(c));
    } else {
      w.putf("%%%02X", c);
    }
  }
  return w.result();
}

// ---------------------------------------------------------------- park message

struct ParkResult {
  uint32_t t = 0;
  uint32_t runS = 0;
  ParkSrc src = ParkSrc::None;
  float lat = NAN;
  float lon = NAN;
  uint32_t acc = 0;
  uint8_t aps = 0;
  NetReading net;
};

inline bool parkHasPosition(const ParkResult &r) {
  return (r.src == ParkSrc::Google || r.src == ParkSrc::Same) && std::isfinite(r.lat) && std::isfinite(r.lon);
}

// The park message body is this JSON; netlink builds the ntfy text from it.
inline size_t buildParkData(char *out, size_t cap, const ParkResult &r) {
  TextWriter w(out, cap);
  w.putf("{\"v\":1,\"t\":%lu,\"run\":%lu,\"src\":\"%s\"", static_cast<unsigned long>(r.t),
         static_cast<unsigned long>(r.runS), parkSrcName(r.src));
  if (parkHasPosition(r)) {
    w.putf(",\"lat\":%.5f,\"lon\":%.5f,\"acc\":%lu", static_cast<double>(r.lat), static_cast<double>(r.lon),
           static_cast<unsigned long>(r.acc));
  }
  w.putf(",\"aps\":%u", static_cast<unsigned>(r.aps));
  char net[192];
  if (buildNetJson(net, sizeof(net), r.net) > 0) {
    w.putStr(",\"net\":");
    w.putStr(net);
  }
  w.putChar('}');
  return w.result();
}

// Human text for ntfy and the generic "message", from the park data JSON.
// click gets a Google Maps link when there is a position.
inline size_t buildParkText(char *out, size_t cap, const char *data, char *click, size_t clickCap) {
  if (click && clickCap) {
    click[0] = '\0';
  }
  double run = 0, lat = 0, lon = 0, acc = 0;
  jsonReadNumber(jsonValueAfterKey(data, "run"), run);
  bool pos = jsonReadNumber(jsonValueAfterKey(data, "lat"), lat) && jsonReadNumber(jsonValueAfterKey(data, "lon"), lon) &&
             jsonReadNumber(jsonValueAfterKey(data, "acc"), acc);
  TextWriter w(out, cap);
  if (pos) {
    w.putf("%.5f, %.5f \xC2\xB1%.0f m", lat, lon, acc);
    if (click && clickCap) {
      snprintf(click, clickCap, "https://maps.google.com/?q=%.5f,%.5f", lat, lon);
    }
  } else {
    w.putStr("Position unknown (");
    const char *src = jsonValueAfterKey(data, "src");
    if (src && *src == '"') {
      for (const char *p = src + 1; *p && *p != '"'; ++p) {
        w.putChar(*p);
      }
    }
    w.putChar(')');
  }
  if (run > 0) {
    w.putStr(" \xC2\xB7 drove ");
    putDuration(w, static_cast<uint32_t>(run));
  }
  const char *net = jsonValueAfterKey(data, "net");
  const char *netEnd = jsonObjectEnd(net);
  if (netEnd) {
    putNetText(w, net, netEnd);
  }
  return w.result();
}

}  // namespace cs
