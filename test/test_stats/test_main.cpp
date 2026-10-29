#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

// One minute of 10 s samples at a constant discharge (positive amps).
static cs::DrainEvent feedMinute(cs::DrainMonitor &m, float dischargeA, float limitA, uint16_t windowMin) {
  for (int i = 0; i < 6; i++) {
    cs::drainAddSample(m, -dischargeA);
  }
  return cs::drainCloseMinute(m, limitA, windowMin);
}

static void test_settings_sanitisers() {
  TEST_ASSERT_EQUAL_FLOAT(4.0f, cs::sanitiseDisAmps(NAN));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, cs::sanitiseDisAmps(0.1f));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, cs::sanitiseDisAmps(80.0f));
  TEST_ASSERT_EQUAL_FLOAT(6.5f, cs::sanitiseDisAmps(6.5f));
  TEST_ASSERT_EQUAL_UINT16(15, cs::sanitiseDisWinMin(5));
  TEST_ASSERT_EQUAL_UINT16(45, cs::sanitiseDisWinMin(45));
  TEST_ASSERT_EQUAL_UINT16(120, cs::sanitiseDisWinMin(500));
  TEST_ASSERT_EQUAL_UINT8(4, cs::sanitiseReportHour(1));
  TEST_ASSERT_EQUAL_UINT8(12, cs::sanitiseReportHour(13));
  TEST_ASSERT_EQUAL_UINT8(16, cs::sanitiseEveningHour(10));
  TEST_ASSERT_EQUAL_UINT8(22, cs::sanitiseEveningHour(23));
  TEST_ASSERT_EQUAL_UINT8(15, cs::sanitiseBatchMin(15));
  TEST_ASSERT_EQUAL_UINT8(60, cs::sanitiseBatchMin(60));
  TEST_ASSERT_EQUAL_UINT8(30, cs::sanitiseBatchMin(20));
  TEST_ASSERT_EQUAL_UINT8(1, cs::sanitiseDaysAvg(0));
  TEST_ASSERT_EQUAL_UINT8(7, cs::sanitiseDaysAvg(9));
  cs::OnlineSettings defaults;
  TEST_ASSERT_EQUAL_FLOAT(4.0f, defaults.disAmps);
  TEST_ASSERT_EQUAL_UINT16(45, defaults.disWinMin);
  TEST_ASSERT_EQUAL_UINT8(10, defaults.reportHour);
  TEST_ASSERT_EQUAL_UINT8(20, defaults.eveningHour);
  TEST_ASSERT_EQUAL_UINT8(30, defaults.batchMin);
  TEST_ASSERT_EQUAL_UINT8(3, defaults.daysAvg);
}

static void test_online_configured() {
  TEST_ASSERT_FALSE(cs::onlineConfigured("", "van", ""));
  TEST_ASSERT_FALSE(cs::onlineConfigured("https://ntfy.sh/x", "", ""));
  TEST_ASSERT_TRUE(cs::onlineConfigured("https://ntfy.sh/x", "van", ""));
  TEST_ASSERT_TRUE(cs::onlineConfigured("https://ntfy.sh/x", "", "phone"));
}

static void test_local_day_and_minute() {
  // 2026-10-28T22:53:17Z, Brussels +3600 -> 23:53 local, same day.
  const uint32_t t = 1793227997UL;
  TEST_ASSERT_TRUE(cs::clockValid(t));
  TEST_ASSERT_FALSE(cs::clockValid(5000));
  TEST_ASSERT_EQUAL_UINT16(22 * 60 + 53, cs::localMinuteOfDay(t, 0));
  TEST_ASSERT_EQUAL_UINT16(23 * 60 + 53, cs::localMinuteOfDay(t, 3600));
  TEST_ASSERT_EQUAL_INT32(cs::localDayNumber(t, 0), cs::localDayNumber(t, 3600));
  // +7200 crosses local midnight: next day, 00:53.
  TEST_ASSERT_EQUAL_INT32(cs::localDayNumber(t, 0) + 1, cs::localDayNumber(t, 7200));
  TEST_ASSERT_EQUAL_UINT16(53, cs::localMinuteOfDay(t, 7200));
  // Los Angeles -25200 -> 15:53 local.
  TEST_ASSERT_EQUAL_UINT16(15 * 60 + 53, cs::localMinuteOfDay(t, -25200));
  char hm[6];
  cs::formatHhMm(7 * 60 + 48, hm);
  TEST_ASSERT_EQUAL_STRING("07:48", hm);
  cs::formatHhMm(23 * 60 + 5, hm);
  TEST_ASSERT_EQUAL_STRING("23:05", hm);
}

static void test_drain_no_trigger_before_window_fills() {
  cs::DrainMonitor m;
  for (int minute = 1; minute < 45; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 10.0f, 4.0f, 45));
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 10.0f, 4.0f, 45));
  TEST_ASSERT_TRUE(m.active);
}

static void test_drain_triggers_only_above_limit() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 15; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 4.0f, 4.0f, 15));
  }
  TEST_ASSERT_EQUAL_FLOAT(4.0f, cs::drainWindowAverage(m, 15));
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 4.6f, 4.0f, 15));
}

static void test_drain_ignores_short_spike() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 14; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 1.0f, 4.0f, 15));
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 30.0f, 4.0f, 15));
  TEST_ASSERT_FALSE(m.active);
}

static void test_drain_charging_is_not_drain() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 20; minute++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, -20.0f, 4.0f, 15));
  }
}

static void test_drain_clears_after_15_minutes_below_80_percent() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 14; minute++) {
    feedMinute(m, 6.0f, 4.0f, 15);
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 6.0f, 4.0f, 15));
  // At 0 A the 15 min average drops below 3.2 A after 8 minutes, and the
  // all-clear needs 15 consecutive minutes below: minute 22.
  for (int k = 1; k <= 21; k++) {
    TEST_ASSERT_EQUAL(cs::DrainEvent::None, feedMinute(m, 0.0f, 4.0f, 15));
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Clear, feedMinute(m, 0.0f, 4.0f, 15));
  TEST_ASSERT_FALSE(m.active);
}

static void test_drain_clear_counter_resets_when_average_rises() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 15; minute++) {
    feedMinute(m, 6.0f, 4.0f, 15);
  }
  TEST_ASSERT_TRUE(m.active);
  for (int k = 0; k < 10; k++) {
    feedMinute(m, 0.0f, 4.0f, 15);
  }
  TEST_ASSERT_TRUE(m.belowMinutes > 0);
  feedMinute(m, 60.0f, 4.0f, 15);
  TEST_ASSERT_EQUAL_UINT16(0, m.belowMinutes);
  TEST_ASSERT_TRUE(m.active);
}

static void test_drain_cooldown_60_minutes() {
  cs::DrainMonitor m;
  for (int minute = 0; minute < 14; minute++) {
    feedMinute(m, 6.0f, 4.0f, 15);
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Trigger, feedMinute(m, 6.0f, 4.0f, 15));  // minute 15
  for (int k = 0; k < 21; k++) {
    feedMinute(m, 0.0f, 4.0f, 15);
  }
  TEST_ASSERT_EQUAL(cs::DrainEvent::Clear, feedMinute(m, 0.0f, 4.0f, 15));  // minute 37
  int triggerAt = -1;
  for (int minute = 38; minute <= 100; minute++) {
    if (feedMinute(m, 10.0f, 4.0f, 15) == cs::DrainEvent::Trigger && triggerAt < 0) {
      triggerAt = minute;
    }
  }
  TEST_ASSERT_EQUAL_INT(75, triggerAt);
}

static void test_drain_minute_without_samples_is_skipped() {
  cs::DrainMonitor m;
  feedMinute(m, 2.0f, 4.0f, 15);
  TEST_ASSERT_EQUAL(cs::DrainEvent::None, cs::drainCloseMinute(m, 4.0f, 15));
  TEST_ASSERT_EQUAL_UINT8(1, m.count);
  TEST_ASSERT_EQUAL_UINT32(2, m.minuteCounter);
  cs::drainAddSample(m, NAN);
  TEST_ASSERT_EQUAL_UINT8(0, m.sampleCount);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_settings_sanitisers);
  RUN_TEST(test_online_configured);
  RUN_TEST(test_local_day_and_minute);
  RUN_TEST(test_drain_no_trigger_before_window_fills);
  RUN_TEST(test_drain_triggers_only_above_limit);
  RUN_TEST(test_drain_ignores_short_spike);
  RUN_TEST(test_drain_charging_is_not_drain);
  RUN_TEST(test_drain_clears_after_15_minutes_below_80_percent);
  RUN_TEST(test_drain_clear_counter_resets_when_average_rises);
  RUN_TEST(test_drain_cooldown_60_minutes);
  RUN_TEST(test_drain_minute_without_samples_is_skipped);
  return UNITY_END();
}
