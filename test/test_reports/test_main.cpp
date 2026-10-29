#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static cs::DaysEstimate estimate(uint8_t daysUsed, bool showSolar, bool solarCovers, float withSolar, float noSun) {
  cs::DaysEstimate e;
  e.valid = true;
  e.daysUsed = daysUsed;
  e.showSolar = showSolar;
  e.solarCovers = solarCovers;
  e.withSolar = withSolar;
  e.noSun = noSun;
  return e;
}

static cs::DayStats yesterdayStats(float usedAh, float solarAh, float orionAh) {
  cs::DayStats d;
  d.day = 9;
  d.samples = 8640;
  d.solarAh = solarAh;
  d.orionAh = orionAh;
  d.netAh = solarAh + orionAh - usedAh;
  return d;
}

static void test_kinds() {
  TEST_ASSERT_EQUAL_STRING("alert", cs::kindName(cs::MsgKind::Alert));
  TEST_ASSERT_EQUAL_STRING("test", cs::kindName(cs::MsgKind::Test));
  TEST_ASSERT_EQUAL_UINT8(4, cs::kindPriority(cs::MsgKind::Alert));
  TEST_ASSERT_EQUAL_UINT8(3, cs::kindPriority(cs::MsgKind::Clear));
  TEST_ASSERT_EQUAL_UINT8(1, cs::kindPriority(cs::MsgKind::Batch));
  TEST_ASSERT_EQUAL_UINT8(3, cs::kindPriority(cs::MsgKind::Test));
  TEST_ASSERT_EQUAL_STRING("warning", cs::kindTag(cs::MsgKind::Alert));
  TEST_ASSERT_EQUAL_STRING("white_check_mark", cs::kindTag(cs::MsgKind::Clear));
  TEST_ASSERT_EQUAL_STRING("sunny", cs::kindTag(cs::MsgKind::Morning));
  TEST_ASSERT_EQUAL_STRING("city_sunset", cs::kindTag(cs::MsgKind::Evening));
  TEST_ASSERT_EQUAL_STRING("", cs::kindTag(cs::MsgKind::Batch));
  TEST_ASSERT_EQUAL_STRING("Solar today", cs::kindTitle(cs::MsgKind::Evening));
  cs::Message m;
  cs::messageInit(m, cs::MsgKind::Test, 7, 1793227997UL, 1234);
  TEST_ASSERT_EQUAL_STRING("ChargeScreen test", m.title);
  TEST_ASSERT_EQUAL_UINT8(3, m.priority);
  TEST_ASSERT_EQUAL_UINT16(7, m.boot);
  TEST_ASSERT_EQUAL_UINT32(1793227997UL, m.eventUnix);
  cs::messageInit(m, cs::MsgKind::Alert, 7, 42, 1234);
  TEST_ASSERT_EQUAL_UINT32(0, m.eventUnix);
}

static void test_alert_and_clear_text() {
  char body[cs::MSG_BODY_LEN];
  TEST_ASSERT_TRUE(cs::buildAlertBody(body, sizeof body, 6.8f, 45, 4.0f, 71.0f, 178.0f) > 0);
  TEST_ASSERT_EQUAL_STRING("Averaging 6.8 A over the last 45 min (limit 4.0 A). Battery 71% (178 Ah left).", body);
  cs::buildAlertBody(body, sizeof body, 6.8f, 45, 4.0f, 71.0f, NAN);
  TEST_ASSERT_EQUAL_STRING("Averaging 6.8 A over the last 45 min (limit 4.0 A). Battery 71%.", body);
  cs::buildAlertBody(body, sizeof body, 6.8f, 45, 4.0f, 102.3f, NAN);
  TEST_ASSERT_EQUAL_STRING("Averaging 6.8 A over the last 45 min (limit 4.0 A).", body);
  TEST_ASSERT_TRUE(cs::buildClearBody(body, sizeof body, 1.4f, 69.0f) > 0);
  TEST_ASSERT_EQUAL_STRING("1.4 A average. Battery 69%.", body);
}

static void test_morning_full_example() {
  char body[cs::MSG_BODY_LEN];
  cs::DayStats y = yesterdayStats(52.0f, 31.0f, 40.0f);
  TEST_ASSERT_TRUE(cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(3, true, false, 4.2f, 2.1f), &y,
                                        true, true) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\n"
      "At the last 3 days' rate: about 4.2 days with solar, about 2.1 days with no sun.\n"
      "Yesterday: used 52 Ah, solar +31 Ah, Orion +40 Ah.",
      body);
}

static void test_morning_omissions() {
  char body[cs::MSG_BODY_LEN];
  cs::DayStats y = yesterdayStats(52.0f, 0.0f, 40.0f);
  // No solar key: single figure, no solar part.
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(3, false, false, NAN, 2.1f), &y, false, true);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\nAt the last 3 days' rate: about 2.1 days.\nYesterday: used 52 Ah, Orion +40 Ah.", body);
  // Orion key missing and solar 0.0 Ah: both parts left out.
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(3, false, false, NAN, 2.1f), &y, true, false);
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left)\nAt the last 3 days' rate: about 2.1 days.\nYesterday: used 52 Ah.", body);
  // Capacity not learned: no "Ah left" and no estimate line.
  cs::DaysEstimate none;
  cs::buildMorningBody(body, sizeof body, 78.0f, NAN, none, &y, true, true);
  TEST_ASSERT_EQUAL_STRING("78%\nYesterday: used 52 Ah, Orion +40 Ah.", body);
  // No complete yesterday and no complete day at all.
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, none, nullptr, true, true);
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left)", body);
}

static void test_morning_day_counts_and_limits() {
  char body[cs::MSG_BODY_LEN];
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(2, true, true, NAN, 2.1f), nullptr, true, true);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\nAt the last 2 days' rate: solar covers your usage, about 2.1 days with no sun.", body);
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(1, true, false, 45.0f, INFINITY), nullptr, true,
                       true);
  TEST_ASSERT_EQUAL_STRING(
      "78% (196 Ah left)\nAt the last day's rate: more than 30 days with solar, more than 30 days with no sun.", body);
  cs::buildMorningBody(body, sizeof body, 78.0f, 196.0f, estimate(1, false, false, NAN, 30.0f), nullptr, true, true);
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left)\nAt the last day's rate: about 30.0 days.", body);
}

static void test_evening_example() {
  char body[cs::MSG_BODY_LEN];
  cs::DayStats d;
  d.day = 10;
  d.solarAh = 31.0f;
  d.solarWh = 412.0f;
  d.pvPeakW = 286.0f;
  d.pvPeakMinute = 13 * 60 + 12;
  d.pvFirstMinute = 7 * 60 + 48;
  d.pvLastMinute = 19 * 60 + 26;
  TEST_ASSERT_TRUE(cs::buildEveningBody(body, sizeof body, d) > 0);
  TEST_ASSERT_EQUAL_STRING("+31 Ah (412 Wh), peak 286 W at 13:12\nProducing from 07:48 to 19:26 (11 h 38 m)", body);
}

static void test_delayed_suffix_forms() {
  char s[48];
  const uint32_t event = 1793227997UL;  // 22:53:17 UTC, 23:53 Brussels
  TEST_ASSERT_EQUAL_UINT32(0, cs::formatDelayedSuffix(s, sizeof s, event + 600, event, true, 3600, true, 0, 0));
  TEST_ASSERT_EQUAL_STRING("", s);
  cs::formatDelayedSuffix(s, sizeof s, event + 601, event, true, 3600, true, 0, 0);
  TEST_ASSERT_EQUAL_STRING(" (delayed, happened 23:53)", s);
  // Clock known but no local offset yet: minutes ago.
  cs::formatDelayedSuffix(s, sizeof s, event + 1500, event, false, 0, true, 0, 0);
  TEST_ASSERT_EQUAL_STRING(" (delayed, happened 25 min ago)", s);
  // Event before the first time sync: compare millis() in the same boot.
  cs::formatDelayedSuffix(s, sizeof s, event, 0, true, 3600, true, 3000000UL, 1200000UL);
  TEST_ASSERT_EQUAL_STRING(" (delayed, happened 30 min ago)", s);
  cs::formatDelayedSuffix(s, sizeof s, 0, 0, false, 0, true, 300000UL, 0);
  TEST_ASSERT_EQUAL_STRING("", s);
  // Event time unknown and from an earlier boot.
  cs::formatDelayedSuffix(s, sizeof s, event, 0, true, 3600, false, 0, 0);
  TEST_ASSERT_EQUAL_STRING(" (delayed)", s);
}

static void test_insert_after_first_line() {
  char body[64] = "78% (196 Ah left)\nYesterday: used 52 Ah.";
  TEST_ASSERT_TRUE(cs::insertAfterFirstLine(body, sizeof body, " (delayed)"));
  TEST_ASSERT_EQUAL_STRING("78% (196 Ah left) (delayed)\nYesterday: used 52 Ah.", body);
  char single[40] = "1.4 A average.";
  TEST_ASSERT_TRUE(cs::insertAfterFirstLine(single, sizeof single, " (delayed)"));
  TEST_ASSERT_EQUAL_STRING("1.4 A average. (delayed)", single);
  char tiny[16] = "1.4 A average.";
  TEST_ASSERT_FALSE(cs::insertAfterFirstLine(tiny, sizeof tiny, " (delayed)"));
  TEST_ASSERT_EQUAL_STRING("1.4 A average.", tiny);
}

static void test_online_status_text() {
  char t[16];
  cs::onlineStatusText(cs::OnlineState::Off, -1, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Off", t);
  cs::onlineStatusText(cs::OnlineState::NoWifi, 600, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("No WiFi", t);
  cs::onlineStatusText(cs::OnlineState::Sent, 7 * 60 + 5, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Sent 07:05", t);
  cs::onlineStatusText(cs::OnlineState::Error, -1, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Error --:--", t);
  cs::onlineStatusText(cs::OnlineState::Waiting, -1, t, sizeof t);
  TEST_ASSERT_EQUAL_STRING("Waiting", t);
}

static void test_writer_escapes_and_overflows() {
  char buf[32];
  cs::TextWriter w(buf, sizeof buf);
  w.putEscaped("a\"b\\c\nd\te");
  TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\nd\\u0009e", buf);
  char small[8];
  cs::TextWriter s(small, sizeof small);
  s.putStr("1234");
  s.putf("%s", "56789");
  TEST_ASSERT_TRUE(s.overflow);
  TEST_ASSERT_EQUAL_UINT32(0, s.result());
  TEST_ASSERT_EQUAL_STRING("1234", small);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_kinds);
  RUN_TEST(test_alert_and_clear_text);
  RUN_TEST(test_morning_full_example);
  RUN_TEST(test_morning_omissions);
  RUN_TEST(test_morning_day_counts_and_limits);
  RUN_TEST(test_evening_example);
  RUN_TEST(test_delayed_suffix_forms);
  RUN_TEST(test_insert_after_first_line);
  RUN_TEST(test_online_status_text);
  RUN_TEST(test_writer_escapes_and_overflows);
  return UNITY_END();
}
