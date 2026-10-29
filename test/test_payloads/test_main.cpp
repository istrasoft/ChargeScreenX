#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static cs::Record specRecord() {
  cs::Record r;
  r.t = 1761638400UL;
  r.soc = 78.1f;
  r.used = 44.2f;
  r.i = -2.1f;
  r.imin = -6.0f;
  r.imax = 3.2f;
  r.pv = 120.0f;
  r.pvmax = 286.0f;
  r.orion = 0.0f;
  return r;
}

static cs::Record worstCaseRecord() {
  cs::Record r;
  r.t = 4294967295UL;
  r.soc = 100.0f;
  r.used = -9999.9f;
  r.i = -9999.9f;
  r.imin = -9999.9f;
  r.imax = -9999.9f;
  r.pv = 99999.0f;
  r.pvmax = 99999.0f;
  r.orion = -9999.9f;
  return r;
}

static void test_record_builder_averages_and_extremes() {
  cs::RecordBuilder b;
  cs::recordStart(b, 1793174400UL, 5000, 3);
  cs::Sample s;
  s.shuntValid = true;
  s.soc = 78.0f;
  s.consumedAh = 44.0f;
  s.solarValid = true;
  s.orionValid = true;
  const float currents[] = {-6.0f, 3.2f, -3.5f};
  const float pvs[] = {100.0f, 286.0f, 0.0f};
  for (int k = 0; k < 3; k++) {
    s.currentA = currents[k];
    s.pvW = pvs[k];
    s.orionOutA = 1.0f;
    cs::recordAdd(b, s);
  }
  s.soc = 102.3f;  // not available: keeps the last known SOC
  s.consumedAh = 44.2f;
  s.currentA = -2.1f;
  s.solarValid = false;
  s.orionValid = false;
  cs::recordAdd(b, s);
  cs::Record r = cs::recordClose(b);
  TEST_ASSERT_EQUAL_UINT32(1793174400UL, r.t);
  TEST_ASSERT_EQUAL_UINT32(5000, r.startMs);
  TEST_ASSERT_EQUAL_UINT16(3, r.boot);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 78.0f, r.soc);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 44.2f, r.used);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -2.1f, r.i);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -6.0f, r.imin);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.2f, r.imax);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 128.667f, r.pv);
  TEST_ASSERT_EQUAL_FLOAT(286.0f, r.pvmax);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, r.orion);
}

static void test_record_without_clock_or_devices() {
  cs::RecordBuilder b;
  cs::recordStart(b, 1000, 77, 1);
  cs::Record r = cs::recordClose(b);
  TEST_ASSERT_EQUAL_UINT32(0, r.t);
  TEST_ASSERT_TRUE(std::isnan(r.i));
  TEST_ASSERT_TRUE(std::isnan(r.soc));
  TEST_ASSERT_TRUE(std::isnan(r.pv));
  TEST_ASSERT_TRUE(std::isnan(r.pvmax));
  TEST_ASSERT_TRUE(std::isnan(r.orion));
}

static void test_batch_json_matches_spec_example() {
  char out[cs::BATCH_JSON_MAX];
  cs::Record r = specRecord();
  TEST_ASSERT_TRUE(cs::buildBatchJson(out, sizeof out, &r, 1) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"recs\":[{\"t\":1761638400,\"soc\":78.1,\"used\":44.2,\"i\":-2.1,\"imin\":-6.0,\"imax\":3.2,"
      "\"pv\":120,\"pvmax\":286,\"or\":0.0}]}",
      out);
}

static void test_batch_json_omits_invalid_fields_and_rounds() {
  char out[cs::BATCH_JSON_MAX];
  cs::Record recs[2];
  recs[0].t = 100;
  recs[0].soc = 78.14f;
  recs[0].i = -0.04f;
  recs[0].pv = 119.6f;
  recs[1].t = 400;
  recs[1].orion = 12.25f;
  cs::buildBatchJson(out, sizeof out, recs, 2);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"recs\":[{\"t\":100,\"soc\":78.1,\"i\":0.0,\"pv\":120},{\"t\":400,\"or\":12.3}]}",
                           out);
  cs::buildBatchJson(out, sizeof out, recs, 0);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"recs\":[]}", out);
}

static void test_twelve_records_fit_ntfy_limit() {
  cs::Record recs[cs::RECORDS_PER_BATCH];
  for (auto &r : recs) {
    r = worstCaseRecord();
  }
  char json[cs::BATCH_JSON_MAX];
  size_t len = cs::buildBatchJson(json, sizeof json, recs, cs::RECORDS_PER_BATCH);
  TEST_ASSERT_TRUE(len > 0);
  TEST_ASSERT_TRUE(len < cs::NTFY_MESSAGE_LIMIT);
  static char envelope[cs::ENVELOPE_MAX];
  TEST_ASSERT_TRUE(cs::buildNtfyEnvelope(envelope, sizeof envelope, "ghibli_harmony", "ChargeScreen data", json, 1,
                                         "") > 0);
  TEST_ASSERT_TRUE(cs::buildGenericEnvelope(envelope, sizeof envelope, cs::MsgKind::Batch, "ChargeScreen data",
                                            "12 records", 1, 1761638400UL, json) > 0);
}

static void test_sink_detection() {
  TEST_ASSERT_EQUAL(cs::SinkType::Ntfy, cs::sinkType("https://ntfy.sh/ghibli_harmony"));
  TEST_ASSERT_EQUAL(cs::SinkType::Ntfy, cs::sinkType("https://NTFY.sh/ghibli_harmony"));
  TEST_ASSERT_EQUAL(cs::SinkType::Ntfy, cs::sinkType("https://eu.ntfy.sh/topic"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("https://example.com/cs/post.php"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("https://notntfy.sh/topic"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("https://ntfy.sh.example.com/topic"));
  TEST_ASSERT_EQUAL(cs::SinkType::Generic, cs::sinkType("garbage"));
}

static void test_ntfy_topic_extraction() {
  char base[96];
  char topic[64];
  TEST_ASSERT_TRUE(cs::ntfyTarget("https://ntfy.sh/ghibli_harmony", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/", base);
  TEST_ASSERT_EQUAL_STRING("ghibli_harmony", topic);
  TEST_ASSERT_TRUE(cs::ntfyTarget("https://eu.ntfy.sh:443/my-topic/?x=1", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_EQUAL_STRING("https://eu.ntfy.sh:443/", base);
  TEST_ASSERT_EQUAL_STRING("my-topic", topic);
  TEST_ASSERT_FALSE(cs::ntfyTarget("https://ntfy.sh/", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_FALSE(cs::ntfyTarget("https://ntfy.sh", base, sizeof base, topic, sizeof topic));
  TEST_ASSERT_FALSE(cs::ntfyTarget("https://ntfy.sh/a/b", base, sizeof base, topic, sizeof topic));
}

static void test_ntfy_envelope() {
  char out[512];
  TEST_ASSERT_TRUE(cs::buildNtfyEnvelope(out, sizeof out, "ghibli_harmony", "High battery drain",
                                         "Averaging 6.8 A.\n\"quoted\"", 4, "warning") > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"topic\":\"ghibli_harmony\",\"title\":\"High battery drain\",\"message\":\"Averaging 6.8 A.\\n\\\"quoted\\\"\","
      "\"priority\":4,\"tags\":[\"warning\"]}",
      out);
  cs::buildNtfyEnvelope(out, sizeof out, "t", "ChargeScreen data", "{}", 1, "");
  TEST_ASSERT_EQUAL_STRING("{\"topic\":\"t\",\"title\":\"ChargeScreen data\",\"message\":\"{}\",\"priority\":1}", out);
}

static void test_generic_envelope() {
  char out[512];
  TEST_ASSERT_TRUE(cs::buildGenericEnvelope(out, sizeof out, cs::MsgKind::Clear, "Drain back to normal",
                                            "1.4 A average. Battery 69%.", 3, 1761638400UL, nullptr) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"kind\":\"clear\",\"title\":\"Drain back to normal\",\"message\":\"1.4 A average. Battery 69%.\","
      "\"priority\":3,\"time\":1761638400,\"data\":{}}",
      out);
  cs::buildGenericEnvelope(out, sizeof out, cs::MsgKind::Batch, "ChargeScreen data", "1 records", 1, 5,
                           "{\"v\":1,\"recs\":[]}");
  TEST_ASSERT_EQUAL_STRING(
      "{\"v\":1,\"kind\":\"batch\",\"title\":\"ChargeScreen data\",\"message\":\"1 records\",\"priority\":1,"
      "\"time\":5,\"data\":{\"v\":1,\"recs\":[]}}",
      out);
  char tiny[16];
  TEST_ASSERT_EQUAL_UINT32(0, cs::buildGenericEnvelope(tiny, sizeof tiny, cs::MsgKind::Test, "x", "y", 3, 0, nullptr));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_record_builder_averages_and_extremes);
  RUN_TEST(test_record_without_clock_or_devices);
  RUN_TEST(test_batch_json_matches_spec_example);
  RUN_TEST(test_batch_json_omits_invalid_fields_and_rounds);
  RUN_TEST(test_twelve_records_fit_ntfy_limit);
  RUN_TEST(test_sink_detection);
  RUN_TEST(test_ntfy_topic_extraction);
  RUN_TEST(test_ntfy_envelope);
  RUN_TEST(test_generic_envelope);
  return UNITY_END();
}
