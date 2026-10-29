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
