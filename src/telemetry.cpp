#include "telemetry.h"

#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "netlink.h"

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
  if (!LittleFS.exists(path)) {
    return 0;
  }
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
            h.recordSize == sizeof(cs::Record) && h.capacity == cs::RECORD_RING_SIZE &&
            h.ring.sentSeq <= h.ring.nextSeq;
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
            h.messageSize == sizeof(cs::Message) && h.capacity == cs::OUTBOX_SIZE &&
            h.queue.nextSeq - h.queue.headSeq <= cs::OUTBOX_SIZE && h.queue.nextId != 0;
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
  if (!storeMutex) {
    Serial.println("Online: store mutex failed");
    return false;
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
  const cs::RingIndex before = cacheHeader.ring;
  uint32_t seq = cs::ringPush(cacheHeader.ring);
  if (!writeAt(CACHE_PATH, cacheOffset(seq), &rec, sizeof(rec))) {
    cacheHeader.ring = before;
    return false;
  }
  return writeAt(CACHE_PATH, 0, &cacheHeader, sizeof(cacheHeader));
}

bool storeEnqueueMessage(const cs::Message &msg) {
  if (!storeReady) {
    return false;
  }
  StoreLock lock;
  uint32_t id = 0;
  bool dropped = false;
  const cs::QueueIndex before = outboxHeader.queue;
  uint8_t slot = cs::queuePush(outboxHeader.queue, static_cast<cs::MsgKind>(msg.kind), id, dropped);
  if (dropped) {
    Serial.println("Online: outbox full, dropped the oldest message");
  }
  if (!writeAt(OUTBOX_PATH, outboxOffset(slot), &msg, sizeof(msg))) {
    outboxHeader.queue = before;
    return false;
  }
  return writeAt(OUTBOX_PATH, 0, &outboxHeader, sizeof(outboxHeader));
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
    if (!writeAt(CACHE_PATH, 0, &cacheHeader, sizeof(cacheHeader))) {
      Serial.println("store: mark records sent failed");
    }
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
        if (!cache.seek(offset) || cache.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec)) != sizeof(rec)) {
          Serial.println("store: fill record time failed");
          break;
        }
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
        if (!outbox.seek(offset) || outbox.write(reinterpret_cast<const uint8_t *>(&msg), sizeof(msg)) != sizeof(msg)) {
          Serial.println("store: fill message time failed");
          break;
        }
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
  if (!LittleFS.exists(DAYS_PATH)) {
    return false;
  }
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
