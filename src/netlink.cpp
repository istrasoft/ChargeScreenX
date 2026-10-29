#include "netlink.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <time.h>

#include "telemetry.h"

namespace {

constexpr uint32_t NET_TASK_STACK = 8192;
constexpr UBaseType_t NET_TASK_PRIORITY = 1;
constexpr uint32_t WIFI_CONNECT_MS = 15000;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_WAIT_MS = 10000;
constexpr uint32_t MIN_FREE_HEAP = 60UL * 1024UL;
constexpr uint32_t FIRST_ATTEMPT_DELAY_MS = 60000;
constexpr uint32_t OFFSET_REFRESH_S = 86400;
// Worst case for one bounded request: 15 s join + 10 s TLS handshake + 10 s HTTP.
constexpr uint32_t PAUSE_WAIT_MS = 45000;
constexpr uint32_t MIN_LOOP_MS = 1000;

enum class Attempt : uint8_t { Done, Failed, RateLimited, Paused, Off };

TaskHandle_t netTask = nullptr;
portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
NetConfig sharedConfig = {};  // written by loop(), guarded by configMux
NetConfig activeConfig = {};  // netlink task copy

// ESP32-C3 is single-core and aligned 32-bit loads and stores are atomic,
// so these flags are plain volatiles.
volatile bool paused = false;
volatile bool busy = false;
volatile bool suppressed = false;
volatile bool tzOverrideActive = false;
volatile bool offsetKnown = false;
volatile int32_t offsetSeconds = 0;
volatile uint8_t onlineState = static_cast<uint8_t>(cs::OnlineState::Off);
volatile uint32_t lastAttemptUnix = 0;

// Netlink task only.
uint32_t offsetFetchedUnix = 0;
uint8_t failures = 0;
bool backoffActive = false;
uint32_t backoffUntilMs = 0;
char bodyBuffer[cs::BATCH_JSON_MAX];
char envelopeBuffer[cs::ENVELOPE_MAX];
char responseBuffer[2048];
cs::Record batchRecords[cs::RECORDS_PER_BATCH];
cs::Message messageBuffer;

void applyTimeZone(const char *tz) {
  setenv("TZ", tz[0] ? tz : "UTC0", 1);
  tzset();
}

int httpRequestWith(WiFiClient &client, const char *url, const char *postBody, char *response, size_t responseCap) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    return -1;
  }
  int code;
  if (postBody) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(postBody)), strlen(postBody));
  } else {
    code = http.GET();
  }
  if (response && responseCap) {
    response[0] = '\0';
    if (code > 0) {
      String text = http.getString();
      snprintf(response, responseCap, "%s", text.c_str());
    }
  }
  http.end();
  return code;
}

// Returns the HTTP status, or <= 0 for a network or TLS error.
int httpRequest(const char *url, const char *postBody, char *response, size_t responseCap) {
  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;
    client.setInsecure();  // no certificate check, see the spec non-goals
    client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);  // seconds; the default is 120 s
    return httpRequestWith(client, url, postBody, response, responseCap);
  }
  WiFiClient client;
  return httpRequestWith(client, url, postBody, response, responseCap);
}

bool connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  for (int n = 0; n < 2; n++) {
    if (!activeConfig.ssid[n][0]) {
      continue;
    }
    WiFi.begin(activeConfig.ssid[n], activeConfig.pass[n][0] ? activeConfig.pass[n] : nullptr);
    uint32_t start = millis();
    while (millis() - start < WIFI_CONNECT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Online: joined %s\n", activeConfig.ssid[n]);
        return true;
      }
      if (paused) {
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    WiFi.disconnect();
    Serial.printf("Online: could not join %s\n", activeConfig.ssid[n]);
  }
  return false;
}

bool apActive() {
  return (WiFi.getMode() & WIFI_AP) != 0;
}

void wifiOff() {
  sntp_stop();
  if (apActive()) {
    // The settings hotspot is up: drop only the client side, never the AP.
    WiFi.disconnect(true);
    WiFi.enableSTA(false);
    return;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void syncClock() {
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  applyTimeZone(activeConfig.tz);  // configTime() rewrites TZ
  uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < NTP_WAIT_MS && !paused) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (cs::clockValid(now)) {
    storeFillTimes(now, millis());
  } else {
    Serial.println("Online: NTP sync failed");
  }
}

bool fetchOffsetTimeapi(int32_t &offset) {
  char ip[46];
  char url[128];
  if (httpRequest(cs::IP_LOOKUP_URL, nullptr, responseBuffer, sizeof(responseBuffer)) != 200 ||
      !cs::parsePlainIp(responseBuffer, ip, sizeof(ip)) || !cs::buildTimeapiUrl(ip, url, sizeof(url)) || paused) {
    return false;
  }
  return httpRequest(url, nullptr, responseBuffer, sizeof(responseBuffer)) == 200 &&
         cs::parseTimeapiOffset(responseBuffer, offset);
}

bool fetchOffsetIpApi(int32_t &offset) {
  return httpRequest(cs::IPAPI_URL, nullptr, responseBuffer, sizeof(responseBuffer)) == 200 &&
         cs::parseIpApiOffset(responseBuffer, offset);
}

void refreshOffset() {
  if (activeConfig.tz[0]) {
    return;
  }
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (!cs::clockValid(now) || (offsetKnown && now - offsetFetchedUnix < OFFSET_REFRESH_S)) {
    return;
  }
  int32_t offset = 0;
  if (paused) {
    return;
  }
  bool ok = fetchOffsetTimeapi(offset);
  if (!ok && paused) {
    return;  // runAttempt() reports the pause
  }
  if (ok || fetchOffsetIpApi(offset)) {
    offsetSeconds = offset;
    offsetKnown = true;
    offsetFetchedUnix = now;
    Serial.printf("Online: UTC offset %ld s\n", static_cast<long>(offset));
  } else if (!paused) {
    Serial.println("Online: time zone lookup failed");
  }
}

void logHeap(const char *what, int code) {
  Serial.printf("Online: %s -> HTTP %d, free heap %u, min %u, largest %u, stack left %u\n",
                what,
                code,
                ESP.getFreeHeap(),
                ESP.getMinFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void addDelayedSuffix(cs::Message &msg) {
  cs::ClockNow clock = clockNow();
  char suffix[48];
  cs::formatDelayedSuffix(suffix,
                          sizeof(suffix),
                          clock.utcValid ? clock.unixUtc : 0,
                          msg.eventUnix,
                          clock.localValid,
                          clock.offsetS,
                          msg.boot == storeBootId(),
                          millis(),
                          msg.eventMs);
  cs::insertAfterFirstLine(msg.body, sizeof(msg.body), suffix);
}

// True when the configured URL can be posted to without joining WiFi first.
bool urlUsable() {
  if (cs::sinkType(activeConfig.url) == cs::SinkType::Ntfy) {
    char base[96];
    char topic[64];
    return cs::ntfyTarget(activeConfig.url, base, sizeof(base), topic, sizeof(topic));
  }
  return strncmp(activeConfig.url, "https://", 8) == 0 || strncmp(activeConfig.url, "http://", 7) == 0;
}

void stampAttempt() {
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (cs::clockValid(now)) {
    lastAttemptUnix = now;
  }
}

Attempt resultOf(cs::NetResult result) {
  return result == cs::NetResult::RateLimited ? Attempt::RateLimited : Attempt::Failed;
}

Attempt sendAll() {
  char base[96];
  char topic[64];
  bool ntfy = cs::sinkType(activeConfig.url) == cs::SinkType::Ntfy;
  if (ntfy && !cs::ntfyTarget(activeConfig.url, base, sizeof(base), topic, sizeof(topic))) {
    Serial.println("Online: ntfy URL has no topic");
    return Attempt::Failed;
  }
  const char *postUrl = ntfy ? base : activeConfig.url;

  uint32_t id = 0;
  while (storePeekMessage(messageBuffer, id)) {
    if (paused) {
      return Attempt::Paused;
    }
    cs::MsgKind kind = static_cast<cs::MsgKind>(messageBuffer.kind);
    addDelayedSuffix(messageBuffer);
    uint32_t when = messageBuffer.eventUnix ? messageBuffer.eventUnix : static_cast<uint32_t>(time(nullptr));
    size_t len = ntfy ? cs::buildNtfyEnvelope(envelopeBuffer, sizeof(envelopeBuffer), topic, messageBuffer.title,
                                              messageBuffer.body, messageBuffer.priority, cs::kindTag(kind))
                      : cs::buildGenericEnvelope(envelopeBuffer, sizeof(envelopeBuffer), kind, messageBuffer.title,
                                                 messageBuffer.body, messageBuffer.priority, when, nullptr);
    if (len == 0) {
      storePopMessage(id);
      continue;
    }
    int code = httpRequest(postUrl, envelopeBuffer, nullptr, 0);
    logHeap(cs::kindName(kind), code);
    cs::NetResult result = cs::classifyHttp(code);
    if (result == cs::NetResult::Sent || result == cs::NetResult::Drop) {
      storePopMessage(id);
      continue;
    }
    return resultOf(result);
  }

  for (uint8_t batch = 0; batch < cs::BATCHES_PER_CONNECTION; batch++) {
    if (paused) {
      return Attempt::Paused;
    }
    uint32_t endSeq = 0;
    uint8_t count = storeReadBatch(batchRecords, cs::RECORDS_PER_BATCH, endSeq);
    if (count == 0) {
      storeMarkRecordsSent(endSeq);  // skips undatable records from an earlier boot
      break;
    }
    size_t len = cs::buildBatchJson(bodyBuffer, sizeof(bodyBuffer), batchRecords, count);
    if (len > 0) {
      if (ntfy) {
        len = cs::buildNtfyEnvelope(envelopeBuffer, sizeof(envelopeBuffer), topic, cs::kindTitle(cs::MsgKind::Batch),
                                    bodyBuffer, cs::kindPriority(cs::MsgKind::Batch), "");
      } else {
        char summary[24];
        snprintf(summary, sizeof(summary), "%u records", static_cast<unsigned>(count));
        len = cs::buildGenericEnvelope(envelopeBuffer, sizeof(envelopeBuffer), cs::MsgKind::Batch,
                                       cs::kindTitle(cs::MsgKind::Batch), summary,
                                       cs::kindPriority(cs::MsgKind::Batch), batchRecords[0].t, bodyBuffer);
      }
    }
    if (len == 0) {
      storeMarkRecordsSent(endSeq);
      continue;
    }
    int code = httpRequest(postUrl, envelopeBuffer, nullptr, 0);
    logHeap("batch", code);
    cs::NetResult result = cs::classifyHttp(code);
    if (result == cs::NetResult::Sent || result == cs::NetResult::Drop) {
      storeMarkRecordsSent(endSeq);
      continue;
    }
    return resultOf(result);
  }
  return Attempt::Done;
}

Attempt runAttempt() {
  busy = true;  // set before checking paused, see netlinkPause()
  if (paused || apActive()) {  // never disturb the settings hotspot
    busy = false;
    return Attempt::Paused;
  }
  if (suppressed || !activeConfig.enabled) {
    busy = false;
    return Attempt::Off;
  }
  if (!urlUsable()) {
    // A configuration problem, not a network failure: no WiFi join and no
    // backoff. OnlineState has no "bad URL" value, so this reports Off.
    Serial.println("Online: URL is not usable, not sending");
    onlineState = static_cast<uint8_t>(cs::OnlineState::Off);
    busy = false;
    return Attempt::Off;
  }
  uint32_t heap = ESP.getFreeHeap();
  if (heap < MIN_FREE_HEAP) {
    Serial.printf("Online: skipped, free heap %u\n", heap);
    busy = false;
    return Attempt::Failed;
  }

  Attempt result;
  if (!connectWifi()) {
    result = paused ? Attempt::Paused : Attempt::Failed;
    if (!paused) {
      onlineState = static_cast<uint8_t>(cs::OnlineState::NoWifi);
      stampAttempt();
    }
  } else {
    syncClock();
    if (!paused) {
      refreshOffset();
    }
    result = paused ? Attempt::Paused : sendAll();
    if (result != Attempt::Paused) {
      onlineState = static_cast<uint8_t>(result == Attempt::Done ? cs::OnlineState::Sent : cs::OnlineState::Error);
      stampAttempt();
    }
  }
  wifiOff();
  busy = false;
  return result;
}

void netTaskMain(void *) {
  uint32_t nextPeriodicMs = millis() + FIRST_ATTEMPT_DELAY_MS;
  bool urgent = false;
  for (;;) {
    uint32_t now = millis();
    if (backoffActive && static_cast<int32_t>(backoffUntilMs - now) <= 0) {
      backoffActive = false;  // expired; a stale deadline would compare wrongly after ~24.8 days
    }
    uint32_t waitMs;
    if (backoffActive) {
      waitMs = backoffUntilMs - now;
    } else if (urgent) {
      waitMs = 0;
    } else {
      int32_t untilPeriodic = static_cast<int32_t>(nextPeriodicMs - now);
      waitMs = untilPeriodic > 0 ? static_cast<uint32_t>(untilPeriodic) : 0;
    }
    if (waitMs > 0) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs)) > 0) {
        urgent = true;
      }
      continue;
    }

    ulTaskNotifyTake(pdTRUE, 0);  // notifications so far are covered by this attempt
    urgent = false;
    taskENTER_CRITICAL(&configMux);
    activeConfig = sharedConfig;
    taskEXIT_CRITICAL(&configMux);
    // 0 (unconfigured) or any invalid value becomes 30 minutes.
    nextPeriodicMs = millis() + static_cast<uint32_t>(cs::sanitiseBatchMin(activeConfig.batchMin)) * 60000UL;

    Attempt result = runAttempt();
    if (result == Attempt::Done) {
      failures = 0;
      backoffActive = false;
    } else if (result == Attempt::Failed || result == Attempt::RateLimited) {
      if (result == Attempt::RateLimited) {
        failures = cs::BACKOFF_MAX_FAILURES;
      } else if (failures < cs::BACKOFF_MAX_FAILURES) {
        failures++;
      }
      backoffActive = true;
      backoffUntilMs = millis() + cs::backoffSeconds(failures) * 1000UL;
      urgent = storeMessageCount() > 0;  // retry pending alerts when the backoff ends
      Serial.printf("Online: attempt failed, retry in %lu s\n", static_cast<unsigned long>(cs::backoffSeconds(failures)));
    } else if (result == Attempt::Paused) {
      urgent = true;
    }
    vTaskDelay(pdMS_TO_TICKS(MIN_LOOP_MS));  // at most one attempt per second, whatever happens

  }
}

}  // namespace

cs::ClockNow clockNow() {
  cs::ClockNow clock;
  time_t now = time(nullptr);
  clock.unixUtc = now > 0 ? static_cast<uint32_t>(now) : 0;
  clock.utcValid = cs::clockValid(clock.unixUtc);
  if (!clock.utcValid) {
    return clock;
  }
  if (tzOverrideActive) {
    struct tm local;
    struct tm utc;
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
    clock.offsetS = cs::tmOffsetSeconds(local.tm_year, local.tm_yday,
                                        local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec, utc.tm_year,
                                        utc.tm_yday, utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec);
    clock.localValid = true;
  } else if (offsetKnown) {
    clock.offsetS = offsetSeconds;
    clock.localValid = true;
  }
  return clock;
}

void netlinkBegin() {
  if (!netTask) {
    xTaskCreate(netTaskMain, "netlink", NET_TASK_STACK, nullptr, NET_TASK_PRIORITY, &netTask);
  }
}

void netlinkConfigure(const NetConfig &config) {
  taskENTER_CRITICAL(&configMux);
  sharedConfig = config;
  taskEXIT_CRITICAL(&configMux);
  applyTimeZone(config.tz);
  tzOverrideActive = config.tz[0] != '\0';
  if (!config.enabled) {
    onlineState = static_cast<uint8_t>(cs::OnlineState::Off);
  } else if (onlineState == static_cast<uint8_t>(cs::OnlineState::Off)) {
    onlineState = static_cast<uint8_t>(cs::OnlineState::Waiting);
  }
}

void netlinkNotify() {
  if (netTask) {
    xTaskNotifyGive(netTask);
  }
}

bool netlinkPause() {
  paused = true;
  uint32_t start = millis();
  while (busy && millis() - start < PAUSE_WAIT_MS) {
    delay(20);
  }
  // runAttempt() clears busy only after wifiOff(), so idle means STA is off.
  return !busy && (WiFi.getMode() & WIFI_STA) == 0;
}

void netlinkResume() {
  paused = false;
}

void netlinkSetSuppressed(bool value) {
  suppressed = value;
}

OnlineStatus netlinkStatus() {
  OnlineStatus status;
  status.state = suppressed ? cs::OnlineState::Off : static_cast<cs::OnlineState>(onlineState);
  status.lastUnix = lastAttemptUnix;
  return status;
}
