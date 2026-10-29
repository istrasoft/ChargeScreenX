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

}  // namespace cs
