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
