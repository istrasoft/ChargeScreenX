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

}  // namespace cs
