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

}  // namespace cs
