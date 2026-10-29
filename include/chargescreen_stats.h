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

}  // namespace cs
