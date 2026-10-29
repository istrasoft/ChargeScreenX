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
