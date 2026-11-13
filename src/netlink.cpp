#include "netlink.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <time.h>

#include "deco.h"
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
constexpr uint32_t OFF_RECHECK_MS = 60000;         // Send = Off: recheck the setting this often
constexpr uint32_t WATCH_INTERVAL_MS = 60UL * 60000UL;
constexpr uint32_t WATCH_CONFIRM_MS = 5UL * 60000UL;
constexpr uint32_t SCAN_MS_PER_CHANNEL = 300;          // active pass
constexpr uint32_t SCAN_PASSIVE_MS_PER_CHANNEL = 600;  // passive pass: about 6 beacon intervals

enum class Attempt : uint8_t { Done, Failed, RateLimited, Paused, Off };
enum class ParkStep : uint8_t { Nothing, Queued, Retry };

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
cs::ApInfo scanAps[cs::GEO_SCAN_MAX];
cs::GeoState geoState;
char geoUrl[160];
bool watchActive = false;
uint32_t nextWatchMs = 0;
uint8_t watchSuspicions = 0;

// Park request from loop(), guarded by configMux.
cs::ParkRequest pendingPark;
bool parkPending = false;
uint32_t parkSeq = 0;
volatile bool engineRanFlag = false;
volatile uint32_t lastEngineMs = 0;  // millis() of the last running sample, 0 = none this boot
volatile bool radioWanted = false;   // netlink wants BLE scanning paused for a Wi-Fi scan
volatile bool bleIdle = false;       // loop() has stopped BLE scanning
constexpr uint32_t BLE_PAUSE_WAIT_MS = 1500;

// Network quality (netlink task only).
bool speedTimerSet = false;  // false: measure the download at the next parked connection
uint32_t nextSpeedMs = 0;
bool speedWanted = false;    // decided at the start of each attempt
char netJsonBuffer[192];     // this connection's reading, sent with the first batch

void setPendingPark(const cs::ParkRequest &req) {
  taskENTER_CRITICAL(&configMux);
  pendingPark = req;
  parkPending = true;
  parkSeq++;
  taskEXIT_CRITICAL(&configMux);
}

bool takePark(cs::ParkRequest &req, uint32_t &seq) {
  taskENTER_CRITICAL(&configMux);
  bool have = parkPending;
  req = pendingPark;
  seq = parkSeq;
  taskEXIT_CRITICAL(&configMux);
  return have;
}

// Done: clear it, unless a newer request replaced it meanwhile.
void finishPark(uint32_t seq, bool done, uint8_t attempts) {
  taskENTER_CRITICAL(&configMux);
  if (parkSeq == seq) {
    if (done) {
      parkPending = false;
    } else {
      pendingPark.attempts = attempts;
    }
  }
  taskEXIT_CRITICAL(&configMux);
}

bool parkIsPending() {
  taskENTER_CRITICAL(&configMux);
  bool have = parkPending;
  taskEXIT_CRITICAL(&configMux);
  return have;
}

uint8_t configuredBatchMin() {
  taskENTER_CRITICAL(&configMux);
  uint8_t v = sharedConfig.batchMin;
  taskEXIT_CRITICAL(&configMux);
  return cs::sanitiseBatchMin(v);
}

uint32_t untilMs(uint32_t target, uint32_t now) {
  int32_t diff = static_cast<int32_t>(target - now);
  return diff > 0 ? static_cast<uint32_t>(diff) : 0;
}

// No engine-running sample for PARK_SETTLE_MS (or none since boot).
bool vanParked(uint32_t now) {
  uint32_t last = lastEngineMs;
  return last == 0 || now - last >= cs::PARK_SETTLE_MS;
}

void applyTimeZone(const char *tz) {
  setenv("TZ", tz[0] ? tz : "UTC0", 1);
  tzset();
}

int httpRequestWith(WiFiClient &client, const char *url, const char *postBody, char *response, size_t responseCap,
                    const char *token) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    return -1;
  }
  if (token && token[0]) {
    char auth[cs::POST_TOKEN_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    http.addHeader("Authorization", auth);
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

// Returns the HTTP status, or <= 0 for a network or TLS error. Pass a token
// only for the configured URL, never for the time zone lookups.
int httpRequest(const char *url, const char *postBody, char *response, size_t responseCap,
                const char *token = nullptr) {
  if (strncmp(url, "https://", 8) == 0) {
    WiFiClientSecure client;
    client.setInsecure();  // no certificate check, see the spec non-goals
    client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);  // seconds; the default is 120 s
    return httpRequestWith(client, url, postBody, response, responseCap, token);
  }
  WiFiClient client;
  return httpRequestWith(client, url, postBody, response, responseCap, token);
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

// STA scan without joining. Keeps the usable access points, strongest first.
uint8_t scanAccessPoints() {
  // Pause BLE scanning (owned by loop()) so the Wi-Fi scan gets the radio.
  radioWanted = true;
  uint32_t waitStart = millis();
  while (!bleIdle && millis() - waitStart < BLE_PAUSE_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  bool blePaused = bleIdle;
  if (!blePaused) {
    Serial.println("Online: BLE scan not paused for the Wi-Fi scan");
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Pass 1: active (probe requests, quick answers). Pass 2: passive, listening
  // long enough on each channel for several beacons of weak networks.
  // Both include hidden networks; results merge by BSSID, strongest RSSI kept.
  const bool passive[2] = {false, true};
  const uint32_t dwellMs[2] = {SCAN_MS_PER_CHANNEL, SCAN_PASSIVE_MS_PER_CHANNEL};
  uint8_t n = 0;
  int total = 0;
  uint8_t own[4][6];  // BSSIDs of the configured networks, to drop their sibling networks
  uint8_t ownCount = 0;
  uint32_t scanStart = millis();
  for (int pass = 0; pass < 2 && !paused; pass++) {
    int16_t found = WiFi.scanNetworks(false, true, passive[pass], dwellMs[pass]);
    for (int16_t i = 0; i < found; i++) {
      const uint8_t *bssid = WiFi.BSSID(i);
      if (!bssid) {
        continue;
      }
      int32_t rssi = WiFi.RSSI(i);
      int8_t rssi8 = static_cast<int8_t>(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
      int known = -1;
      for (uint8_t k = 0; k < n; k++) {
        if (memcmp(scanAps[k].bssid, bssid, 6) == 0) {
          known = k;
          break;
        }
      }
      String ssid = WiFi.SSID(i);
      bool usable = cs::apUsable(ssid.c_str(), bssid, activeConfig.ssid[0], activeConfig.ssid[1]);
      bool isOwn = ssid.length() && ((activeConfig.ssid[0][0] && ssid == activeConfig.ssid[0]) ||
                                     (activeConfig.ssid[1][0] && ssid == activeConfig.ssid[1]));
      if (isOwn && ownCount < 4) {
        memcpy(own[ownCount++], bssid, 6);
      }
      if (known < 0) {
        total++;
        Serial.printf("  wifi %s %02x:%02x:%02x:..:%02x ch %2d %4d dBm %s %.20s\n", passive[pass] ? "passive" : "active ",
                      bssid[0], bssid[1], bssid[2], bssid[5], static_cast<int>(WiFi.channel(i)), static_cast<int>(rssi),
                      usable ? "use " : "skip", ssid.length() ? ssid.c_str() : "(hidden)");
      }
      if (!usable) {
        continue;
      }
      if (known >= 0) {
        if (rssi8 > scanAps[known].rssi) {
          scanAps[known].rssi = rssi8;
        }
        continue;
      }
      if (n < cs::GEO_SCAN_MAX) {
        memcpy(scanAps[n].bssid, bssid, 6);
        scanAps[n].rssi = rssi8;
        scanAps[n].channel = static_cast<uint8_t>(WiFi.channel(i));
        n++;
      }
    }
    WiFi.scanDelete();
  }
  radioWanted = false;
  uint8_t before = n;
  n = cs::apDropOwnDevices(scanAps, n, own, ownCount);
  if (n != before) {
    Serial.printf("Online: dropped %u networks from the same box as WiFi 1/2 (guest, mesh backhaul)\n",
                  static_cast<unsigned>(before - n));
  }
  uint8_t kept = cs::apKeepStrongest(scanAps, n, cs::GEO_KEEP_APS);
  Serial.printf("Online: scan found %d networks in %lu ms, %u usable\n", total,
                static_cast<unsigned long>(millis() - scanStart), kept);
  return kept;
}

int32_t budgetDay(const cs::ClockNow &clock) {
  return clock.localValid ? cs::localDayNumber(clock.unixUtc, clock.offsetS)
                          : static_cast<int32_t>(clock.unixUtc / 86400U);
}

// Median of 3 small HTTP requests on one kept-alive connection (the first also
// pays DNS and TCP setup). Plain HTTP, so no TLS handshake in the timing.
int32_t measureLatency() {
  WiFiClient client;
  HTTPClient http;
  http.setReuse(true);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, cs::LATENCY_URL)) {
    return -1;
  }
  int32_t ms[3];
  for (int i = 0; i < 3; i++) {
    uint32_t start = millis();
    int code = paused ? -1 : http.GET();
    ms[i] = (code == 204 || code == 200) ? static_cast<int32_t>(millis() - start) : -1;
  }
  http.end();
  return cs::medianMs(ms, 3);
}

// 100 KB from Cloudflare's speed test endpoint. The clock starts once the
// response headers are in, so TLS setup and server delay are not counted.
int32_t measureDownload() {
  WiFiClientSecure client;
  client.setInsecure();  // no certificate check, see the spec non-goals
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000);
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, cs::SPEED_URL)) {
    return -1;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Online: speed test HTTP %d\n", code);
    http.end();
    return -1;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t chunk[512];
  uint32_t got = 0;
  uint32_t start = millis();
  uint32_t lastData = start;
  while (got < cs::SPEED_BYTES && millis() - lastData < HTTP_TIMEOUT_MS && !paused) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->read(chunk, avail < sizeof(chunk) ? avail : sizeof(chunk));
      if (n > 0) {
        got += static_cast<uint32_t>(n);
        lastData = millis();
      }
    } else if (!stream->connected()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  uint32_t elapsed = millis() - start;
  http.end();
  return got >= cs::SPEED_BYTES ? cs::throughputKbps(got, elapsed) : -1;
}

// Deco radio values (if the password is set), lag, and optionally the download.
void readNetwork(cs::NetReading &n, bool withSpeed) {
  n = cs::NetReading();
  if (activeConfig.decoPass[0] && !paused) {
    String gateway = WiFi.gatewayIP().toString();
    decoReadSignal(gateway.c_str(), activeConfig.decoPass, n.sig);
  }
  if (!paused) {
    n.ms = measureLatency();
  }
  if (withSpeed && !paused) {
    n.kbps = measureDownload();
  }
  Serial.printf("Online: network %s cov %d qual %d, %ld ms, %ld kbit/s\n", cs::netTypeLabel(n.sig.net),
                n.sig.valid ? cs::coverageScore(n.sig) : -1, n.sig.valid ? cs::qualityScore(n.sig) : -1,
                static_cast<long>(n.ms), static_cast<long>(n.kbps));
}

// After joining: position (Google or saved), network reading, then the park message.
ParkStep handlePark(uint8_t apCount) {
  cs::ParkRequest req;
  uint32_t seq = 0;
  if (!takePark(req, seq)) {
    return ParkStep::Nothing;
  }
  cs::ClockNow clock = clockNow();
  if (!storeLoadGeo(geoState)) {
    geoState = cs::GeoState();
  }
  cs::ParkResult r;
  r.runS = req.runS;
  r.aps = apCount;
  r.t = req.offUnix;
  if (r.t == 0 && req.boot == storeBootId() && clock.utcValid) {
    r.t = clock.unixUtc - (millis() - req.offMs) / 1000U;
  }

  cs::ParkSrc skipSrc = cs::ParkSrc::None;
  cs::GeoStep step = cs::geoDecide(geoState, scanAps, apCount, activeConfig.geoKey[0] != '\0', budgetDay(clock), skipSrc);
  if (step == cs::GeoStep::Saved) {
    r.src = cs::ParkSrc::Same;
    r.lat = geoState.lat;
    r.lon = geoState.lon;
    r.acc = geoState.acc;
  } else if (step == cs::GeoStep::Skip) {
    r.src = skipSrc;
  } else if (!cs::buildGeoUrl(activeConfig.geoKey, geoUrl, sizeof(geoUrl)) ||
             cs::buildGeoRequest(bodyBuffer, sizeof(bodyBuffer), scanAps, apCount) == 0) {
    r.src = cs::ParkSrc::Error;
  } else {
    geoState.calls++;
    storeSaveGeo(geoState);
    int code = httpRequest(geoUrl, bodyBuffer, responseBuffer, sizeof(responseBuffer));
    logHeap("geolocate", code);
    double lat = 0;
    double lon = 0;
    double acc = 0;
    switch (cs::parseGeoResponse(code, responseBuffer, lat, lon, acc)) {
      case cs::GeoOutcome::Fix:
        cs::geoRemember(geoState, scanAps, apCount, lat, lon, acc, clock.utcValid ? clock.unixUtc : 0);
        r.src = cs::ParkSrc::Google;
        r.lat = geoState.lat;
        r.lon = geoState.lon;
        r.acc = geoState.acc;
        break;
      case cs::GeoOutcome::NotFound:
        r.src = cs::ParkSrc::None;
        break;
      case cs::GeoOutcome::Quota:
        geoState.refused = 1;
        r.src = cs::ParkSrc::Quota;
        break;
      case cs::GeoOutcome::Retry:
        if (req.attempts + 1 < cs::PARK_MAX_ATTEMPTS) {
          finishPark(seq, false, static_cast<uint8_t>(req.attempts + 1));
          Serial.println("Online: geolocation failed, will retry");
          return ParkStep::Retry;
        }
        r.src = cs::ParkSrc::Error;
        break;
      default:
        Serial.printf("Online: geolocation error: %.120s\n", responseBuffer);
        r.src = cs::ParkSrc::Error;
        break;
    }
  }

  readNetwork(r.net, true);  // a new place: always measure the download too
  speedTimerSet = true;
  nextSpeedMs = millis() + cs::SPEED_INTERVAL_MS;

  engineRanFlag = false;  // left over from the drive that just ended
  cs::watchSetReference(geoState, scanAps, apCount);
  storeSaveGeo(geoState);
  watchActive = geoState.watchActive != 0;
  watchSuspicions = 0;
  nextWatchMs = millis() + WATCH_INTERVAL_MS;

  cs::Message msg;
  cs::messageInit(msg, cs::MsgKind::Park, req.boot, r.t, req.offMs);
  if (cs::buildParkData(msg.body, sizeof(msg.body), r) == 0 || !storeEnqueueMessage(msg)) {
    Serial.println("Online: park message failed");
  }
  finishPark(seq, true, 0);
  Serial.printf("Online: park %s, %u networks\n", cs::parkSrcName(r.src), apCount);
  return ParkStep::Queued;
}

// The engine ran: the van is being driven, so stop watching for a move.
void processEngineFlag() {
  if (!engineRanFlag) {
    return;
  }
  engineRanFlag = false;
  speedTimerSet = false;  // measure again at the next parking
  if (!watchActive) {
    return;
  }
  watchActive = false;
  watchSuspicions = 0;
  if (storeLoadGeo(geoState)) {
    geoState.watchActive = 0;
    storeSaveGeo(geoState);
  }
  Serial.println("Online: engine running, moved-van watch off");
}

// Hourly scan while parked. Returns true when it queued a "moved" alert and
// a new park request (an attempt should follow now).
bool runWatchScan() {
  taskENTER_CRITICAL(&configMux);
  activeConfig = sharedConfig;
  taskEXIT_CRITICAL(&configMux);
  nextWatchMs = millis() + WATCH_INTERVAL_MS;
  if (paused || suppressed || !activeConfig.enabled) {
    return false;
  }
  busy = true;  // set before checking paused, see netlinkPause()
  if (paused || apActive()) {
    busy = false;
    return false;
  }
  uint8_t n = scanAccessPoints();
  wifiOff();
  busy = false;
  if (!storeLoadGeo(geoState)) {
    watchActive = false;
    return false;
  }
  uint8_t overlap = cs::apOverlapCount(scanAps, n, geoState.watchRef, geoState.watchCount, cs::WATCH_APS);
  Serial.printf("Online: watch scan, %u usable, %u known\n", n, overlap);
  if (!cs::watchSuspicious(geoState, scanAps, n)) {
    watchSuspicions = 0;
    return false;
  }
  if (++watchSuspicions < 2) {
    nextWatchMs = millis() + WATCH_CONFIRM_MS;
    return false;
  }
  watchSuspicions = 0;
  watchActive = false;
  geoState.watchActive = 0;
  storeSaveGeo(geoState);

  cs::ClockNow clock = clockNow();
  uint32_t nowUnix = clock.utcValid ? clock.unixUtc : 0;
  cs::Message msg;
  cs::messageInit(msg, cs::MsgKind::Moved, storeBootId(), nowUnix, millis());
  snprintf(msg.body, sizeof(msg.body), "%s", cs::MOVED_BODY);
  storeEnqueueMessage(msg);
  cs::ParkRequest req;
  req.offUnix = nowUnix;
  req.offMs = millis();
  req.runS = 0;
  req.boot = storeBootId();
  setPendingPark(req);
  Serial.println("Online: van moved, looking up the new position");
  return true;
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
    uint32_t when = messageBuffer.eventUnix ? messageBuffer.eventUnix : static_cast<uint32_t>(time(nullptr));
    size_t len;
    if (kind == cs::MsgKind::Park) {
      // The body is the data JSON; the human text is built from it.
      char text[192];
      char click[96];
      cs::buildParkText(text, sizeof(text), messageBuffer.body, click, sizeof(click));
      len = ntfy ? cs::buildNtfyEnvelope(envelopeBuffer, sizeof(envelopeBuffer), topic, messageBuffer.title, text,
                                         messageBuffer.priority, cs::kindTag(kind), click)
                 : cs::buildGenericEnvelope(envelopeBuffer, sizeof(envelopeBuffer), kind, messageBuffer.title, text,
                                            messageBuffer.priority, when, messageBuffer.body);
    } else {
      addDelayedSuffix(messageBuffer);
      len = ntfy ? cs::buildNtfyEnvelope(envelopeBuffer, sizeof(envelopeBuffer), topic, messageBuffer.title,
                                         messageBuffer.body, messageBuffer.priority, cs::kindTag(kind))
                 : cs::buildGenericEnvelope(envelopeBuffer, sizeof(envelopeBuffer), kind, messageBuffer.title,
                                            messageBuffer.body, messageBuffer.priority, when, nullptr);
    }
    if (len == 0) {
      storePopMessage(id);
      continue;
    }
    int code = httpRequest(postUrl, envelopeBuffer, nullptr, 0, activeConfig.token);
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
    // The network reading rides on the first batch; drop it if it doesn't fit.
    size_t len = cs::buildBatchJson(bodyBuffer, sizeof(bodyBuffer), batchRecords, count, netJsonBuffer);
    if (len == 0 && netJsonBuffer[0]) {
      netJsonBuffer[0] = '\0';
      len = cs::buildBatchJson(bodyBuffer, sizeof(bodyBuffer), batchRecords, count);
    }
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
    int code = httpRequest(postUrl, envelopeBuffer, nullptr, 0, activeConfig.token);
    logHeap("batch", code);
    cs::NetResult result = cs::classifyHttp(code);
    if (result == cs::NetResult::Sent || result == cs::NetResult::Drop) {
      storeMarkRecordsSent(endSeq);
      netJsonBuffer[0] = '\0';
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

  // A parking needs the scan before joining: a connected scan would drop the link.
  bool parkWaiting = parkIsPending();
  uint8_t apCount = parkWaiting ? scanAccessPoints() : 0;

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
    ParkStep park = (paused || !parkWaiting) ? ParkStep::Nothing : handlePark(apCount);
    // While parked, every connection records the network; the park message already has one.
    netJsonBuffer[0] = '\0';
    if (!paused && park != ParkStep::Queued && vanParked(millis())) {
      cs::NetReading reading;
      readNetwork(reading, speedWanted);
      cs::buildNetJson(netJsonBuffer, sizeof(netJsonBuffer), reading);
    }
    result = paused ? Attempt::Paused : sendAll();
    if (result == Attempt::Done && park == ParkStep::Retry) {
      result = Attempt::Failed;  // back off, then retry the position lookup
    }
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
  uint32_t lastAttemptMs = millis();
  bool firstDone = false;
  bool urgent = false;
  if (storeLoadGeo(geoState) && geoState.watchActive) {
    watchActive = true;  // still parked since before the reboot
    nextWatchMs = millis() + WATCH_INTERVAL_MS;
  }
  for (;;) {
    uint32_t now = millis();
    processEngineFlag();
    if (backoffActive && static_cast<int32_t>(backoffUntilMs - now) <= 0) {
      backoffActive = false;  // expired; a stale deadline would compare wrongly after ~24.8 days
    }
    if (watchActive && untilMs(nextWatchMs, now) == 0) {
      if (runWatchScan()) {
        urgent = true;
      }
      continue;
    }
    // Read every wake, so a change on the settings screen applies at once.
    uint8_t batchMin = configuredBatchMin();
    uint32_t waitMs;
    if (backoffActive) {
      waitMs = backoffUntilMs - now;
    } else if (urgent) {
      waitMs = 0;
    } else if (!firstDone) {
      waitMs = untilMs(lastAttemptMs + FIRST_ATTEMPT_DELAY_MS, now);  // clock and time zone after boot
    } else if (batchMin == 0) {
      waitMs = OFF_RECHECK_MS;  // Send = Off: only "send now" starts an attempt
    } else {
      waitMs = untilMs(lastAttemptMs + static_cast<uint32_t>(batchMin) * 60000UL, now);
    }
    if (watchActive && waitMs > 0) {
      uint32_t untilWatch = untilMs(nextWatchMs, now);
      if (untilWatch < waitMs) {
        waitMs = untilWatch > 0 ? untilWatch : 1;
      }
    }
    // The 4 h download test while parked starts its own attempt, whatever the Send setting.
    if (speedTimerSet && vanParked(now) && waitMs > 0) {
      uint32_t untilSpeed = untilMs(nextSpeedMs, now);
      if (untilSpeed < waitMs) {
        waitMs = untilSpeed;
      }
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
    lastAttemptMs = millis();
    firstDone = true;
    // Download at the first parked connection after a drive or boot, then every 4 h.
    speedWanted = vanParked(lastAttemptMs) && (!speedTimerSet || untilMs(nextSpeedMs, lastAttemptMs) == 0);
    if (speedWanted) {
      speedTimerSet = true;
      nextSpeedMs = lastAttemptMs + cs::SPEED_INTERVAL_MS;
    }

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

void netlinkRequestPark(const cs::ParkRequest &req) {
  setPendingPark(req);
  netlinkNotify();
}

bool netlinkWantsRadio() {
  return radioWanted;
}

void netlinkSetBleIdle(bool idle) {
  bleIdle = idle;
}

void netlinkEngineRunning() {
  uint32_t now = millis();
  lastEngineMs = now ? now : 1;
  engineRanFlag = true;
}

OnlineStatus netlinkStatus() {
  OnlineStatus status;
  status.state = suppressed ? cs::OnlineState::Off : static_cast<cs::OnlineState>(onlineState);
  status.lastUnix = lastAttemptUnix;
  return status;
}
