#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static constexpr uint32_t MIN = 60000UL;

// Feeds 10 s samples from fromMs (inclusive) to toMs (exclusive); returns the last trigger that fired.
static cs::ParkTrigger feed(cs::EngineTracker &e, uint32_t fromMs, uint32_t toMs, bool valid, bool running,
                            int *fired = nullptr) {
  cs::ParkTrigger last;
  for (uint32_t t = fromMs; t < toMs; t += 10000UL) {
    cs::ParkTrigger p = cs::engineUpdate(e, valid, running, t);
    if (p.fired) {
      last = p;
      if (fired) {
        (*fired)++;
      }
    }
  }
  return last;
}

static void test_engine_short_run_no_park() {
  cs::EngineTracker e;
  int fired = 0;
  feed(e, 0, 9 * MIN, true, true, &fired);
  feed(e, 9 * MIN, 20 * MIN, true, false, &fired);
  TEST_ASSERT_EQUAL_INT(0, fired);
}

static void test_engine_drive_then_park() {
  cs::EngineTracker e;
  int fired = 0;
  feed(e, 0, 10 * MIN + 10000UL, true, true, &fired);  // last running sample at 10:00
  cs::ParkTrigger p = feed(e, 10 * MIN + 10000UL, 20 * MIN, true, false, &fired);
  TEST_ASSERT_EQUAL_INT(1, fired);
  TEST_ASSERT_TRUE(p.fired);
  TEST_ASSERT_EQUAL_UINT32(600, p.runS);
  TEST_ASSERT_EQUAL_UINT32(10 * MIN, p.offMs);
}

static void test_engine_short_stop_is_one_drive() {
  cs::EngineTracker e;
  int fired = 0;
  feed(e, 0, 6 * MIN, true, true, &fired);
  feed(e, 6 * MIN, 8 * MIN, true, false, &fired);  // 2 min stop
  feed(e, 8 * MIN, 14 * MIN, true, true, &fired);
  cs::ParkTrigger p = feed(e, 14 * MIN, 20 * MIN, true, false, &fired);
  TEST_ASSERT_EQUAL_INT(1, fired);
  TEST_ASSERT_EQUAL_UINT32(14 * 60 - 10, p.runS);
}

static void test_engine_invalid_drops_run() {
  cs::EngineTracker e;
  int fired = 0;
  feed(e, 0, 15 * MIN, true, true, &fired);
  feed(e, 15 * MIN, 17 * MIN, false, false, &fired);  // Orion lost 2 min
  feed(e, 17 * MIN, 25 * MIN, true, false, &fired);
  TEST_ASSERT_EQUAL_INT(0, fired);
}

static int feedStarter(cs::StarterWatch &w, uint32_t fromMs, uint32_t toMs, uint8_t state, float v,
                       cs::StarterAlert *out = nullptr) {
  int fired = 0;
  for (uint32_t t = fromMs; t < toMs; t += 10000UL) {
    cs::StarterAlert a = cs::starterUpdate(w, true, state, v, t);
    if (a.fired) {
      fired++;
      if (out) {
        *out = a;
      }
    }
  }
  return fired;
}

static void test_starter_settle_then_alert_once() {
  cs::StarterWatch w;
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 0, 10 * MIN, 3, 14.2f));  // driving
  // engine off at 10 min: low from the start, but the first 60 min are ignored
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 10 * MIN, 70 * MIN, 0, 12.0f));
  cs::StarterAlert a;
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 70 * MIN, 99 * MIN, 0, 12.1f));
  TEST_ASSERT_EQUAL_INT(1, feedStarter(w, 99 * MIN, 200 * MIN, 0, 11.9f, &a));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.9f, a.volts);
  TEST_ASSERT_TRUE(a.offForKnown);
  TEST_ASSERT_EQUAL_UINT32(90 * 60, a.offForS);
  char body[cs::MSG_BODY_LEN];
  TEST_ASSERT_TRUE(cs::buildStarterBody(body, sizeof body, a) > 0);
  TEST_ASSERT_EQUAL_STRING("Starter battery at 11.9 V, engine off for 1 h 30 min. Start the engine or charge it soon.",
                           body);
}

static void test_starter_high_sample_restarts_and_rearm() {
  cs::StarterWatch w;
  // no engine seen since boot: settle counts from the first sample
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 0, 60 * MIN, 0, 12.0f));
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 60 * MIN, 80 * MIN, 0, 12.0f));
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 80 * MIN, 80 * MIN + 10000UL, 0, 12.3f));  // restarts
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 80 * MIN + 10000UL, 105 * MIN, 0, 12.0f));
  cs::StarterAlert a;
  TEST_ASSERT_EQUAL_INT(1, feedStarter(w, 105 * MIN, 120 * MIN, 0, 12.0f, &a));
  TEST_ASSERT_FALSE(a.offForKnown);
  // recovered for 30 min re-arms
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w, 120 * MIN, 151 * MIN, 0, 12.7f));
  TEST_ASSERT_EQUAL_INT(1, feedStarter(w, 151 * MIN, 190 * MIN, 0, 12.0f));
  // ignores unusable voltage and invalid samples
  cs::StarterWatch w2;
  TEST_ASSERT_EQUAL_INT(0, feedStarter(w2, 0, 200 * MIN, 0, 0.0f));
}

static cs::ApInfo ap(uint8_t last, int8_t rssi) {
  cs::ApInfo a;
  const uint8_t base[6] = {0x10, 0x20, 0x30, 0x40, 0x50, last};
  memcpy(a.bssid, base, 6);
  a.rssi = rssi;
  a.channel = 6;
  return a;
}

static void test_ap_filter_and_sort() {
  const uint8_t normal[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const uint8_t local[6] = {0x12, 0x20, 0x30, 0x40, 0x50, 0x60};
  TEST_ASSERT_TRUE(cs::apUsable("Cafe", normal, "Van", "Phone"));
  TEST_ASSERT_TRUE(cs::apUsable("", normal, "Van", ""));
  TEST_ASSERT_FALSE(cs::apUsable("Van", normal, "Van", "Phone"));
  TEST_ASSERT_FALSE(cs::apUsable("Phone", normal, "Van", "Phone"));
  TEST_ASSERT_FALSE(cs::apUsable("Cafe", local, "Van", "Phone"));
  TEST_ASSERT_FALSE(cs::apUsable("Home_nomap", normal, "Van", "Phone"));
  cs::ApInfo aps[20];
  for (int i = 0; i < 20; i++) {
    aps[i] = ap(static_cast<uint8_t>(i), static_cast<int8_t>(-90 + i));
  }
  TEST_ASSERT_EQUAL_UINT8(15, cs::apKeepStrongest(aps, 20, 15));
  TEST_ASSERT_EQUAL_INT8(-71, aps[0].rssi);
  TEST_ASSERT_EQUAL_INT8(-85, aps[14].rssi);
}

static void test_same_place_and_watch() {
  cs::GeoState g;
  cs::ApInfo aps[10];
  for (int i = 0; i < 10; i++) {
    aps[i] = ap(static_cast<uint8_t>(i), -60);
  }
  TEST_ASSERT_FALSE(cs::samePlace(g, aps, 10));  // empty reference
  cs::geoRemember(g, aps, 10, 48.85661, 2.35222, 37.6, 1800000000UL);
  TEST_ASSERT_EQUAL_UINT32(38, g.acc);
  TEST_ASSERT_TRUE(cs::samePlace(g, aps, 10));
  cs::ApInfo moved[10];
  for (int i = 0; i < 10; i++) {
    moved[i] = ap(static_cast<uint8_t>(i < 5 ? i : 100 + i), -60);
  }
  TEST_ASSERT_TRUE(cs::samePlace(g, moved, 10));  // 5 of 10
  moved[4] = ap(200, -60);
  TEST_ASSERT_FALSE(cs::samePlace(g, moved, 10));  // 4 of 10

  cs::watchSetReference(g, aps, 10);
  TEST_ASSERT_EQUAL_UINT8(1, g.watchActive);
  cs::ApInfo other[4] = {ap(150, -50), ap(151, -50), ap(152, -50), ap(153, -50)};
  TEST_ASSERT_TRUE(cs::watchSuspicious(g, other, 4));
  other[3] = aps[9];
  TEST_ASSERT_FALSE(cs::watchSuspicious(g, other, 4));  // some overlap
  TEST_ASSERT_FALSE(cs::watchSuspicious(g, other, 2));  // too few
  cs::GeoState g2;
  cs::watchSetReference(g2, aps, 2);
  TEST_ASSERT_EQUAL_UINT8(0, g2.watchActive);
}

static void test_geo_decide_and_budget() {
  cs::GeoState g;
  cs::ApInfo aps[3] = {ap(1, -50), ap(2, -60), ap(3, -70)};
  cs::ParkSrc src = cs::ParkSrc::Google;
  TEST_ASSERT_EQUAL(cs::GeoStep::Skip, cs::geoDecide(g, aps, 1, true, 100, src));
  TEST_ASSERT_EQUAL(cs::ParkSrc::None, src);
  TEST_ASSERT_EQUAL(cs::GeoStep::Skip, cs::geoDecide(g, aps, 3, false, 100, src));
  TEST_ASSERT_EQUAL(cs::GeoStep::Call, cs::geoDecide(g, aps, 3, true, 100, src));
  g.calls = cs::GEO_MAX_CALLS_PER_DAY;
  TEST_ASSERT_EQUAL(cs::GeoStep::Skip, cs::geoDecide(g, aps, 3, true, 100, src));
  TEST_ASSERT_EQUAL(cs::ParkSrc::Quota, src);
  TEST_ASSERT_EQUAL(cs::GeoStep::Call, cs::geoDecide(g, aps, 3, true, 101, src));  // new day
  g.refused = 1;
  TEST_ASSERT_EQUAL(cs::GeoStep::Skip, cs::geoDecide(g, aps, 3, true, 101, src));
  cs::geoRemember(g, aps, 3, 1.0, 2.0, 30.0, 5);
  TEST_ASSERT_EQUAL(cs::GeoStep::Saved, cs::geoDecide(g, aps, 3, true, 101, src));
}

static void test_geo_request_and_key() {
  cs::ApInfo aps[2] = {ap(0xAB, -67), ap(0x01, -80)};
  aps[1].channel = 11;
  char out[512];
  TEST_ASSERT_TRUE(cs::buildGeoRequest(out, sizeof out, aps, 2) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"considerIp\":false,\"wifiAccessPoints\":["
      "{\"macAddress\":\"10:20:30:40:50:ab\",\"signalStrength\":-67,\"channel\":6},"
      "{\"macAddress\":\"10:20:30:40:50:01\",\"signalStrength\":-80,\"channel\":11}]}",
      out);
  TEST_ASSERT_TRUE(cs::validGeoKey(""));
  TEST_ASSERT_TRUE(cs::validGeoKey("AIzaSyA-12_xyz"));
  TEST_ASSERT_FALSE(cs::validGeoKey("bad key"));
  TEST_ASSERT_FALSE(cs::validGeoKey("a&b"));
  char url[160];
  TEST_ASSERT_FALSE(cs::buildGeoUrl("", url, sizeof url));
  TEST_ASSERT_TRUE(cs::buildGeoUrl("KEY1", url, sizeof url));
  TEST_ASSERT_EQUAL_STRING("https://www.googleapis.com/geolocation/v1/geolocate?key=KEY1", url);
}

static void test_geo_responses() {
  double lat = 0, lon = 0, acc = 0;
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Fix,
                    cs::parseGeoResponse(200, "{\n \"location\": {\n  \"lat\": 48.8566142,\n  \"lng\": 2.3522219\n },\n "
                                              "\"accuracy\": 38.5\n}",
                                         lat, lon, acc));
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 48.8566142, lat);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 2.3522219, lon);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 38.5, acc);
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Error, cs::parseGeoResponse(200, "{}", lat, lon, acc));
  TEST_ASSERT_EQUAL(cs::GeoOutcome::NotFound,
                    cs::parseGeoResponse(404, "{\"error\":{\"errors\":[{\"domain\":\"geolocation\",\"reason\":\"notFound\"}],"
                                              "\"code\":404}}",
                                         lat, lon, acc));
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Quota,
                    cs::parseGeoResponse(403, "{\"error\":{\"errors\":[{\"domain\":\"usageLimits\",\"reason\":"
                                              "\"dailyLimitExceeded\"}],\"code\":403}}",
                                         lat, lon, acc));
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Quota, cs::parseGeoResponse(429, "", lat, lon, acc));
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Error,
                    cs::parseGeoResponse(400, "{\"error\":{\"errors\":[{\"domain\":\"usageLimits\",\"reason\":\"keyInvalid\"}]}}",
                                         lat, lon, acc));
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Retry, cs::parseGeoResponse(503, "", lat, lon, acc));
  TEST_ASSERT_EQUAL(cs::GeoOutcome::Retry, cs::parseGeoResponse(-1, "", lat, lon, acc));
}

static void test_deco_signal_and_helpers() {
  cs::DecoSignal sig;
  const char *reply =
      "{\"error_code\":0,\"result\":{\"mobile_cpe\":{\"dial_status\":\"connected\",\"network_type\":\"lte_plus\","
      "\"rsrp\":-95,\"rsrq\":\"-11\",\"snr\":8.5,\"signal_strength\":75,\"cell\":{\"rsrp\":1}},\"rsrp\":-1}}";
  TEST_ASSERT_TRUE(cs::parseDecoSignal(reply, sig));
  TEST_ASSERT_EQUAL_STRING("lte_plus", sig.net);
  TEST_ASSERT_EQUAL_FLOAT(-95.0f, sig.rsrp);
  TEST_ASSERT_EQUAL_FLOAT(-11.0f, sig.rsrq);
  TEST_ASSERT_EQUAL_FLOAT(8.5f, sig.snr);
  TEST_ASSERT_EQUAL_FLOAT(75.0f, sig.lvl);
  TEST_ASSERT_FALSE(cs::parseDecoSignal("{\"error_code\":-40401}", sig));
  TEST_ASSERT_FALSE(cs::parseDecoSignal("{\"error_code\":0,\"result\":{}}", sig));

  TEST_ASSERT_TRUE(cs::validDecoPassword(""));
  TEST_ASSERT_TRUE(cs::validDecoPassword("p@ss word!"));
  TEST_ASSERT_FALSE(cs::validDecoPassword("tab\there"));

  char sign[128];
  TEST_ASSERT_TRUE(cs::buildDecoSignText(sign, sizeof sign, true, "0123456789abcdef", "fedcba9876543210",
                                         "5f4dcc3b5aa765d61d8327deb882cf99", 1000, 44) > 0);
  TEST_ASSERT_EQUAL_STRING("k=0123456789abcdef&i=fedcba9876543210&h=5f4dcc3b5aa765d61d8327deb882cf99&s=1044", sign);
  TEST_ASSERT_TRUE(cs::buildDecoSignText(sign, sizeof sign, false, "", "", "abc", 7, 3) > 0);
  TEST_ASSERT_EQUAL_STRING("h=abc&s=10", sign);
  char enc[32];
  TEST_ASSERT_TRUE(cs::urlEncode(enc, sizeof enc, "a+b/c=") > 0);
  TEST_ASSERT_EQUAL_STRING("a%2Bb%2Fc%3D", enc);
}

static void test_park_data_and_text() {
  cs::ParkResult r;
  r.t = 1762250000UL;
  r.runS = 4380;
  r.src = cs::ParkSrc::Google;
  r.lat = 48.85661f;
  r.lon = 2.35222f;
  r.acc = 38;
  r.aps = 9;
  r.sig.valid = true;
  snprintf(r.sig.net, sizeof r.sig.net, "lte_plus");
  r.sig.rsrp = -95.0f;
  r.sig.rsrq = -11.0f;
  r.sig.snr = 8.5f;
  r.sig.lvl = 75.0f;
  char data[cs::MSG_BODY_LEN];
  TEST_ASSERT_TRUE(cs::buildParkData(data, sizeof data, r) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"t\":1762250000,\"run\":4380,\"src\":\"google\",\"lat\":48.85661,\"lon\":2.35222,\"acc\":38,"
      "\"aps\":9,\"sig\":{\"net\":\"lte_plus\",\"rsrp\":-95,\"rsrq\":-11,\"snr\":8.5,\"lvl\":75}}",
      data);
  char text[128];
  char click[96];
  TEST_ASSERT_TRUE(cs::buildParkText(text, sizeof text, data, click, sizeof click) > 0);
  TEST_ASSERT_EQUAL_STRING("48.85661, 2.35222 \xC2\xB1" "38 m \xC2\xB7 drove 1 h 13 min \xC2\xB7 4G -95 dBm", text);
  TEST_ASSERT_EQUAL_STRING("https://maps.google.com/?q=48.85661,2.35222", click);

  cs::ParkResult none;
  none.runS = 900;
  none.src = cs::ParkSrc::Quota;
  none.aps = 4;
  TEST_ASSERT_TRUE(cs::buildParkData(data, sizeof data, none) > 0);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"t\":0,\"run\":900,\"src\":\"quota\",\"aps\":4}", data);
  TEST_ASSERT_TRUE(cs::buildParkText(text, sizeof text, data, click, sizeof click) > 0);
  TEST_ASSERT_EQUAL_STRING("Position unknown (quota) \xC2\xB7 drove 15 min", text);
  TEST_ASSERT_EQUAL_STRING("", click);
}

static void test_ntfy_click_and_kinds() {
  char out[512];
  TEST_ASSERT_TRUE(cs::buildNtfyEnvelope(out, sizeof out, "t", "Parked", "m", 2, "parking", "https://x/?q=1,2") > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"topic\":\"t\",\"title\":\"Parked\",\"message\":\"m\",\"priority\":2,\"tags\":[\"parking\"],"
      "\"click\":\"https://x/?q=1,2\"}",
      out);
  TEST_ASSERT_EQUAL_STRING("park", cs::kindName(cs::MsgKind::Park));
  TEST_ASSERT_EQUAL_STRING("starter", cs::kindName(cs::MsgKind::Starter));
  TEST_ASSERT_EQUAL_STRING("moved", cs::kindName(cs::MsgKind::Moved));
  TEST_ASSERT_EQUAL_UINT8(2, cs::kindPriority(cs::MsgKind::Park));
  TEST_ASSERT_EQUAL_UINT8(4, cs::kindPriority(cs::MsgKind::Starter));
  TEST_ASSERT_EQUAL_UINT8(4, cs::kindPriority(cs::MsgKind::Moved));
  TEST_ASSERT_EQUAL_STRING("Van moved?", cs::kindTitle(cs::MsgKind::Moved));
  TEST_ASSERT_EQUAL_STRING("battery", cs::kindTag(cs::MsgKind::Starter));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_engine_short_run_no_park);
  RUN_TEST(test_engine_drive_then_park);
  RUN_TEST(test_engine_short_stop_is_one_drive);
  RUN_TEST(test_engine_invalid_drops_run);
  RUN_TEST(test_starter_settle_then_alert_once);
  RUN_TEST(test_starter_high_sample_restarts_and_rearm);
  RUN_TEST(test_ap_filter_and_sort);
  RUN_TEST(test_same_place_and_watch);
  RUN_TEST(test_geo_decide_and_budget);
  RUN_TEST(test_geo_request_and_key);
  RUN_TEST(test_geo_responses);
  RUN_TEST(test_deco_signal_and_helpers);
  RUN_TEST(test_park_data_and_text);
  RUN_TEST(test_ntfy_click_and_kinds);
  return UNITY_END();
}
