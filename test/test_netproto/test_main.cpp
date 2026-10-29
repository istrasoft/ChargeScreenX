#include <unity.h>

#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

// Real responses captured on 2026-10-28 with curl.exe -s.
// https://timeapi.io/api/TimeZone/ip?ipAddress=<public IP> (Brussels, CET after DST ended)
static const char *TIMEAPI_BRUSSELS =
    R"JSON({"timeZone":"Europe/Brussels","currentLocalTime":"2026-10-28T23:53:17.7460455","currentUtcOffset":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"standardUtcOffset":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"hasDayLightSaving":true,"isDayLightSavingActive":false,"dstInterval":{"dstName":"CEST","dstOffsetToUtc":{"seconds":7200,"milliseconds":7200000,"ticks":72000000000,"nanoseconds":7200000000000},"dstOffsetToStandardTime":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"dstStart":"2027-03-28T01:00:00Z","dstEnd":"2027-10-31T01:00:00Z","dstDuration":{"days":217,"nanosecondOfDay":0,"hours":0,"minutes":0,"seconds":0,"milliseconds":0,"subsecondTicks":0,"subsecondNanoseconds":0,"bclCompatibleTicks":187488000000000,"totalDays":217,"totalHours":5208,"totalMinutes":312480,"totalSeconds":18748800,"totalMilliseconds":18748800000,"totalTicks":187488000000000,"totalNanoseconds":18748800000000000}}})JSON";

// https://timeapi.io/api/TimeZone/ip?ipAddress=8.8.8.8 (Los Angeles, PDT active)
static const char *TIMEAPI_LOS_ANGELES =
    R"JSON({"timeZone":"America/Los_Angeles","currentLocalTime":"2026-10-28T15:53:10.4713795","currentUtcOffset":{"seconds":-25200,"milliseconds":-25200000,"ticks":-252000000000,"nanoseconds":-25200000000000},"standardUtcOffset":{"seconds":-28800,"milliseconds":-28800000,"ticks":-288000000000,"nanoseconds":-28800000000000},"hasDayLightSaving":true,"isDayLightSavingActive":true,"dstInterval":{"dstName":"PDT","dstOffsetToUtc":{"seconds":-25200,"milliseconds":-25200000,"ticks":-252000000000,"nanoseconds":-25200000000000},"dstOffsetToStandardTime":{"seconds":3600,"milliseconds":3600000,"ticks":36000000000,"nanoseconds":3600000000000},"dstStart":"2026-03-08T10:00:00Z","dstEnd":"2026-11-01T09:00:00Z","dstDuration":{"days":237,"nanosecondOfDay":82800000000000,"hours":23,"minutes":0,"seconds":0,"milliseconds":0,"subsecondTicks":0,"subsecondNanoseconds":0,"bclCompatibleTicks":205596000000000,"totalDays":237.95833333333334,"totalHours":5711,"totalMinutes":342660,"totalSeconds":20559600,"totalMilliseconds":20559600000,"totalTicks":205596000000000,"totalNanoseconds":20559600000000000}}})JSON";

// https://timeapi.io/api/TimeZone/ip without ipAddress (HTTP 400)
static const char *TIMEAPI_NO_IP =
    R"JSON({"type":"https://tools.ietf.org/html/rfc9110#section-15.5.1","title":"One or more validation errors occurred.","status":400,"errors":{"ipAddress":["The ipAddress field is required."]},"traceId":"00-65fe893e925122ee5ab90ced4fdf984c-b177feb16b6f8636-00"})JSON";

// http://ip-api.com/json/?fields=status,timezone,offset
static const char *IPAPI_BRUSSELS = R"JSON({"status":"success","timezone":"Europe/Brussels","offset":3600})JSON";
// http://ip-api.com/json/8.8.8.8?fields=status,timezone,offset
static const char *IPAPI_NEW_YORK = R"JSON({"status":"success","timezone":"America/New_York","offset":-14400})JSON";
// http://ip-api.com/json/10.0.0.1?fields=status,message,offset
static const char *IPAPI_FAIL = R"JSON({"status":"fail","message":"private range"})JSON";

static void test_timeapi_offset_uses_current_offset() {
  int32_t offset = 0;
  TEST_ASSERT_TRUE(cs::parseTimeapiOffset(TIMEAPI_BRUSSELS, offset));
  TEST_ASSERT_EQUAL_INT32(3600, offset);
  TEST_ASSERT_TRUE(cs::parseTimeapiOffset(TIMEAPI_LOS_ANGELES, offset));
  TEST_ASSERT_EQUAL_INT32(-25200, offset);
}

static void test_timeapi_rejects_error_and_garbage() {
  int32_t offset = 99;
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset(TIMEAPI_NO_IP, offset));
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset("", offset));
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset("{\"currentUtcOffset\":{\"seconds\":99999}}", offset));
  TEST_ASSERT_FALSE(cs::parseTimeapiOffset("{\"currentUtcOffset\":3600}", offset));
  TEST_ASSERT_EQUAL_INT32(99, offset);
}

static void test_ipapi_offset() {
  int32_t offset = 0;
  TEST_ASSERT_TRUE(cs::parseIpApiOffset(IPAPI_BRUSSELS, offset));
  TEST_ASSERT_EQUAL_INT32(3600, offset);
  TEST_ASSERT_TRUE(cs::parseIpApiOffset(IPAPI_NEW_YORK, offset));
  TEST_ASSERT_EQUAL_INT32(-14400, offset);
  TEST_ASSERT_FALSE(cs::parseIpApiOffset(IPAPI_FAIL, offset));
  TEST_ASSERT_FALSE(cs::parseIpApiOffset("not json", offset));
}

static void test_json_key_finder() {
  const char *json = "{ \"a\" : 5, \"x\":\"seconds\", \"seconds\" :\n -12 }";
  const char *p = cs::jsonValueAfterKey(json, "seconds");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_CHAR('-', *p);
  TEST_ASSERT_NULL(cs::jsonValueAfterKey(json, "second"));
  TEST_ASSERT_EQUAL_CHAR('5', *cs::jsonValueAfterKey(json, "a"));
}

static void test_plain_ip_and_timeapi_url() {
  char ip[46];
  TEST_ASSERT_TRUE(cs::parsePlainIp("80.200.185.90", ip, sizeof ip));
  TEST_ASSERT_EQUAL_STRING("80.200.185.90", ip);
  TEST_ASSERT_TRUE(cs::parsePlainIp(" 2a02:a03f::1\n", ip, sizeof ip));
  TEST_ASSERT_EQUAL_STRING("2a02:a03f::1", ip);
  TEST_ASSERT_FALSE(cs::parsePlainIp("<html>", ip, sizeof ip));
  TEST_ASSERT_FALSE(cs::parsePlainIp("1.2.3.4 extra", ip, sizeof ip));
  TEST_ASSERT_FALSE(cs::parsePlainIp("", ip, sizeof ip));
  char url[128];
  TEST_ASSERT_TRUE(cs::buildTimeapiUrl("80.200.185.90", url, sizeof url));
  TEST_ASSERT_EQUAL_STRING("https://timeapi.io/api/TimeZone/ip?ipAddress=80.200.185.90", url);
}

static void test_tm_offset() {
  // 2026-10-28 23:53 local vs 22:53 UTC (yday 300).
  TEST_ASSERT_EQUAL_INT32(3600, cs::tmOffsetSeconds(126, 300, 23 * 3600 + 53 * 60, 126, 300, 22 * 3600 + 53 * 60));
  // Local already in the new year.
  TEST_ASSERT_EQUAL_INT32(3600, cs::tmOffsetSeconds(127, 0, 30 * 60, 126, 364, 23 * 3600 + 30 * 60));
  // Los Angeles 15:53 vs 22:53 UTC.
  TEST_ASSERT_EQUAL_INT32(-25200, cs::tmOffsetSeconds(126, 300, 15 * 3600 + 53 * 60, 126, 300, 22 * 3600 + 53 * 60));
  // Local still on the previous day.
  TEST_ASSERT_EQUAL_INT32(-18000, cs::tmOffsetSeconds(126, 299, 21 * 3600, 126, 300, 2 * 3600));
}

static void test_http_classification_and_backoff() {
  TEST_ASSERT_EQUAL(cs::NetResult::Sent, cs::classifyHttp(200));
  TEST_ASSERT_EQUAL(cs::NetResult::Sent, cs::classifyHttp(204));
  TEST_ASSERT_EQUAL(cs::NetResult::RateLimited, cs::classifyHttp(429));
  TEST_ASSERT_EQUAL(cs::NetResult::Drop, cs::classifyHttp(400));
  TEST_ASSERT_EQUAL(cs::NetResult::Drop, cs::classifyHttp(413));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(500));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(503));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(-1));
  TEST_ASSERT_EQUAL(cs::NetResult::Fail, cs::classifyHttp(301));
  TEST_ASSERT_EQUAL_UINT32(0, cs::backoffSeconds(0));
  TEST_ASSERT_EQUAL_UINT32(60, cs::backoffSeconds(1));
  TEST_ASSERT_EQUAL_UINT32(120, cs::backoffSeconds(2));
  TEST_ASSERT_EQUAL_UINT32(300, cs::backoffSeconds(3));
  TEST_ASSERT_EQUAL_UINT32(600, cs::backoffSeconds(4));
  TEST_ASSERT_EQUAL_UINT32(1800, cs::backoffSeconds(5));
  TEST_ASSERT_EQUAL_UINT32(1800, cs::backoffSeconds(9));
}

static void test_ring_overwrites_oldest_and_tracks_unsent() {
  cs::RingIndex r;
  const uint16_t cap = cs::RECORD_RING_SIZE;
  for (int k = 0; k < 10; k++) {
    cs::ringPush(r);
  }
  TEST_ASSERT_EQUAL_UINT32(10, cs::ringUnsentCount(r, cap));
  cs::ringMarkSentThrough(r, 4);
  TEST_ASSERT_EQUAL_UINT32(4, cs::ringFirstUnsentSeq(r, cap));
  TEST_ASSERT_EQUAL_UINT32(6, cs::ringUnsentCount(r, cap));
  cs::ringMarkSentThrough(r, 2);  // never moves back
  TEST_ASSERT_EQUAL_UINT32(4, r.sentSeq);
  for (int k = 0; k < 300; k++) {
    cs::ringPush(r);
  }
  TEST_ASSERT_EQUAL_UINT32(310, r.nextSeq);
  TEST_ASSERT_EQUAL_UINT32(22, cs::ringOldestSeq(r, cap));
  TEST_ASSERT_EQUAL_UINT32(22, cs::ringFirstUnsentSeq(r, cap));
  TEST_ASSERT_EQUAL_UINT32(288, cs::ringUnsentCount(r, cap));
  TEST_ASSERT_EQUAL_UINT16(22, cs::ringSlot(310, cap));
  cs::ringMarkSentThrough(r, 999);
  TEST_ASSERT_EQUAL_UINT32(310, r.sentSeq);
  TEST_ASSERT_EQUAL_UINT32(0, cs::ringUnsentCount(r, cap));
}

static void test_record_send_action_and_fill() {
  cs::Record r;
  r.boot = 4;
  r.startMs = 100000;
  TEST_ASSERT_EQUAL(cs::RecordAction::Wait, cs::recordSendAction(r, 4));
  TEST_ASSERT_EQUAL(cs::RecordAction::Skip, cs::recordSendAction(r, 5));
  TEST_ASSERT_FALSE(cs::recordFillTime(r, 5, 1793227997UL, 400000));
  TEST_ASSERT_FALSE(cs::recordFillTime(r, 4, 1000, 400000));
  TEST_ASSERT_TRUE(cs::recordFillTime(r, 4, 1793227997UL, 400000));
  TEST_ASSERT_EQUAL_UINT32(1793227997UL - 300, r.t);
  TEST_ASSERT_EQUAL(cs::RecordAction::Include, cs::recordSendAction(r, 9));
  cs::Message m;
  m.boot = 4;
  m.eventMs = 340000;
  TEST_ASSERT_TRUE(cs::messageFillTime(m, 4, 1793227997UL, 400000));
  TEST_ASSERT_EQUAL_UINT32(1793227997UL - 60, m.eventUnix);
  TEST_ASSERT_FALSE(cs::messageFillTime(m, 4, 1793227997UL, 400000));
}

static void test_queue_drops_oldest_when_full() {
  cs::QueueIndex q;
  uint32_t id = 0;
  bool dropped = false;
  uint32_t firstId = 0;
  for (int k = 0; k < cs::OUTBOX_SIZE; k++) {
    cs::queuePush(q, cs::MsgKind::Alert, id, dropped);
    if (k == 0) {
      firstId = id;
    }
    TEST_ASSERT_FALSE(dropped);
  }
  TEST_ASSERT_EQUAL_UINT8(16, cs::queueCount(q));
  TEST_ASSERT_EQUAL_UINT32(firstId, q.ids[cs::queueOldestSlot(q)]);
  uint8_t slot = cs::queuePush(q, cs::MsgKind::Clear, id, dropped);
  TEST_ASSERT_TRUE(dropped);
  TEST_ASSERT_EQUAL_UINT8(0, slot);
  TEST_ASSERT_EQUAL_UINT8(16, cs::queueCount(q));
  TEST_ASSERT_FALSE(cs::queuePopIfId(q, firstId));  // already dropped
  TEST_ASSERT_EQUAL_INT16(1, cs::queueOldestSlot(q));
}

static void test_queue_pop_and_morning_replacement() {
  cs::QueueIndex q;
  uint32_t alertId = 0;
  uint32_t morningId = 0;
  uint32_t newMorningId = 0;
  bool dropped = false;
  TEST_ASSERT_EQUAL_INT16(-1, cs::queueOldestSlot(q));
  cs::queuePush(q, cs::MsgKind::Alert, alertId, dropped);
  uint8_t morningSlot = cs::queuePush(q, cs::MsgKind::Morning, morningId, dropped);
  uint8_t replaced = cs::queuePush(q, cs::MsgKind::Morning, newMorningId, dropped);
  TEST_ASSERT_EQUAL_UINT8(morningSlot, replaced);
  TEST_ASSERT_EQUAL_UINT8(2, cs::queueCount(q));
  TEST_ASSERT_TRUE(cs::queuePopIfId(q, alertId));
  TEST_ASSERT_FALSE(cs::queuePopIfId(q, morningId));  // replaced while being sent
  TEST_ASSERT_TRUE(cs::queuePopIfId(q, newMorningId));
  TEST_ASSERT_EQUAL_UINT8(0, cs::queueCount(q));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_timeapi_offset_uses_current_offset);
  RUN_TEST(test_timeapi_rejects_error_and_garbage);
  RUN_TEST(test_ipapi_offset);
  RUN_TEST(test_json_key_finder);
  RUN_TEST(test_plain_ip_and_timeapi_url);
  RUN_TEST(test_tm_offset);
  RUN_TEST(test_http_classification_and_backoff);
  RUN_TEST(test_ring_overwrites_oldest_and_tracks_unsent);
  RUN_TEST(test_record_send_action_and_fill);
  RUN_TEST(test_queue_drops_oldest_when_full);
  RUN_TEST(test_queue_pop_and_morning_replacement);
  return UNITY_END();
}
