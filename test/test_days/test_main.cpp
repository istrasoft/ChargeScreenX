#include <unity.h>

#include <cmath>

#include "chargescreen_stats.h"

void setUp() {}
void tearDown() {}

static cs::Sample shuntSample(float currentA) {
  cs::Sample s;
  s.shuntValid = true;
  s.currentA = currentA;
  s.soc = 80.0f;
  s.consumedAh = 40.0f;
  return s;
}

// A closed day with the given totals that counts as complete.
static cs::DayStats completeDay(int32_t day, float usedAh, float solarAh) {
  cs::DayStats d;
  d.day = day;
  d.solarAh = solarAh;
  d.netAh = solarAh - usedAh;
  d.samples = 8640;
  return d;
}

static void test_day_integrates_known_currents() {
  cs::DayStats d;
  cs::Sample s = shuntSample(-5.0f);
  s.solarValid = true;
  s.solarBatA = 3.0f;
  s.pvW = 120.0f;
  s.orionValid = true;
  s.orionOutA = 2.0f;
  for (int i = 0; i < 360; i++) {  // one hour of 10 s samples
    cs::dayAddSample(d, s, 10, 600);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, d.netAh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, d.solarAh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, d.orionAh);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 120.0f, d.solarWh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, cs::dayUsedAh(d));
  TEST_ASSERT_EQUAL_UINT32(360, d.samples);
}

static void test_day_used_is_clamped_at_zero() {
  cs::DayStats d;
  d.netAh = 10.0f;
  d.solarAh = 3.0f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, cs::dayUsedAh(d));
}

static void test_day_missing_devices_add_zero_and_invalid_shunt_skips() {
  cs::DayStats d;
  cs::Sample s = shuntSample(-3.6f);
  s.solarValid = false;
  s.solarBatA = 50.0f;
  s.orionValid = true;
  s.orionOutA = NAN;
  cs::dayAddSample(d, s, 3600, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -3.6f, d.netAh);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.solarAh);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.orionAh);
  TEST_ASSERT_EQUAL_UINT32(1, d.samples);
  cs::Sample bad = shuntSample(-3.6f);
  bad.shuntValid = false;
  bad.solarValid = true;
  bad.solarBatA = 10.0f;
  cs::dayAddSample(d, bad, 3600, 1);
  TEST_ASSERT_EQUAL_UINT32(1, d.samples);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.solarAh);
}

static void test_day_pv_first_last_and_peak() {
  cs::DayStats d;
  cs::Sample s = shuntSample(0.0f);
  s.solarValid = true;
  s.solarBatA = 0.0f;
  const struct {
    uint16_t minute;
    float pv;
  } points[] = {
      {7 * 60 + 40, 3.0f}, {7 * 60 + 48, 6.0f}, {13 * 60 + 12, 286.0f}, {15 * 60, 150.0f},
      {19 * 60 + 26, 5.5f}, {19 * 60 + 30, 5.0f}, {20 * 60, 0.0f},
  };
  for (const auto &p : points) {
    s.pvW = p.pv;
    cs::dayAddSample(d, s, 10, p.minute);
  }
  TEST_ASSERT_EQUAL_INT16(7 * 60 + 48, d.pvFirstMinute);
  TEST_ASSERT_EQUAL_INT16(19 * 60 + 26, d.pvLastMinute);
  TEST_ASSERT_EQUAL_FLOAT(286.0f, d.pvPeakW);
  TEST_ASSERT_EQUAL_INT16(13 * 60 + 12, d.pvPeakMinute);
}

static void test_day_history_roll_and_find() {
  cs::DayHistory h;
  cs::dayHistoryRoll(h, 100);
  TEST_ASSERT_EQUAL_INT32(100, h.current.day);
  TEST_ASSERT_EQUAL_UINT8(0, h.count);
  h.current.netAh = -1.0f;
  cs::dayHistoryRoll(h, 100);
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, h.current.netAh);
  for (int32_t day = 101; day <= 110; day++) {
    cs::dayHistoryRoll(h, day);
  }
  TEST_ASSERT_EQUAL_UINT8(7, h.count);
  TEST_ASSERT_EQUAL_INT32(110, h.current.day);
  TEST_ASSERT_EQUAL_INT32(109, h.days[0].day);
  TEST_ASSERT_EQUAL_INT32(103, h.days[6].day);
  TEST_ASSERT_NOT_NULL(cs::dayHistoryFind(h, 105));
  TEST_ASSERT_NULL(cs::dayHistoryFind(h, 100));
}

static void test_capacity_learns_200ah() {
  cs::CapacityLearner c;
  TEST_ASSERT_TRUE(cs::capacityUpdate(c, 78.0f, 44.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, c.capacityAh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 156.0f, cs::remainingAh(c, 78.0f));
}

static void test_capacity_ignores_high_or_unknown_soc() {
  cs::CapacityLearner c;
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 91.0f, 10.0f));
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 102.3f, 10.0f));
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 50.0f, NAN));
  TEST_ASSERT_FALSE(cs::capacityUpdate(c, 50.0f, 0.0f));
  TEST_ASSERT_TRUE(std::isnan(c.capacityAh));
  TEST_ASSERT_TRUE(std::isnan(cs::remainingAh(c, 50.0f)));
  TEST_ASSERT_TRUE(cs::capacityUpdate(c, 90.0f, 20.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.0f, c.capacityAh);
  TEST_ASSERT_TRUE(std::isnan(cs::remainingAh(c, 102.3f)));
}

static void test_capacity_smoothing_and_persist_rule() {
  cs::CapacityLearner c;
  cs::capacityUpdate(c, 78.0f, 44.0f);         // 200
  TEST_ASSERT_TRUE(cs::capacityUpdate(c, 78.0f, 55.0f));  // raw 250 -> 205
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 205.0f, c.capacityAh);
  TEST_ASSERT_TRUE(cs::capacityShouldPersist(205.0f, NAN));
  TEST_ASSERT_FALSE(cs::capacityShouldPersist(203.0f, 200.0f));
  TEST_ASSERT_TRUE(cs::capacityShouldPersist(205.0f, 200.0f));
  TEST_ASSERT_FALSE(cs::capacityShouldPersist(NAN, 200.0f));
}

static void test_days_remaining_both_figures() {
  cs::DayHistory h;
  h.days[0] = completeDay(10, 100.0f, 50.0f);
  h.days[1] = completeDay(9, 100.0f, 50.0f);
  h.days[2] = completeDay(8, 100.0f, 50.0f);
  h.count = 3;
  cs::DaysEstimate e = cs::daysRemaining(h, 3, 200.0f, true);
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_EQUAL_UINT8(3, e.daysUsed);
  TEST_ASSERT_TRUE(e.showSolar);
  TEST_ASSERT_FALSE(e.solarCovers);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, e.withSolar);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, e.noSun);
  cs::DaysEstimate noKey = cs::daysRemaining(h, 3, 200.0f, false);
  TEST_ASSERT_FALSE(noKey.showSolar);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, noKey.noSun);
}

static void test_days_remaining_solar_covers_usage() {
  cs::DayHistory h;
  h.days[0] = completeDay(10, 40.0f, 60.0f);
  h.count = 1;
  cs::DaysEstimate e = cs::daysRemaining(h, 3, 150.0f, true);
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_TRUE(e.solarCovers);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.75f, e.noSun);
}

static void test_days_remaining_uses_fewer_days_and_skips_incomplete() {
  cs::DayHistory h;
  h.days[0] = completeDay(10, 60.0f, 0.0f);
  h.days[1] = completeDay(9, 60.0f, 0.0f);
  h.days[1].samples = 100;  // partial day, not counted
  h.days[2] = completeDay(8, 40.0f, 0.0f);
  h.count = 3;
  cs::DaysEstimate e = cs::daysRemaining(h, 3, 100.0f, true);
  TEST_ASSERT_TRUE(e.valid);
  TEST_ASSERT_EQUAL_UINT8(2, e.daysUsed);
  TEST_ASSERT_FALSE(e.showSolar);  // avgSolar is 0
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, e.noSun);
  cs::DaysEstimate one = cs::daysRemaining(h, 1, 100.0f, true);
  TEST_ASSERT_EQUAL_UINT8(1, one.daysUsed);
}

static void test_days_remaining_needs_complete_day_and_capacity() {
  cs::DayHistory h;
  TEST_ASSERT_FALSE(cs::daysRemaining(h, 3, 100.0f, true).valid);
  h.days[0] = completeDay(10, 60.0f, 0.0f);
  h.days[0].samples = 10;
  h.count = 1;
  TEST_ASSERT_FALSE(cs::daysRemaining(h, 3, 100.0f, true).valid);
  h.days[0].samples = 8640;
  TEST_ASSERT_FALSE(cs::daysRemaining(h, 3, NAN, true).valid);
  TEST_ASSERT_TRUE(cs::daysRemaining(h, 3, 100.0f, true).valid);
}

static void test_morning_due() {
  TEST_ASSERT_FALSE(cs::morningDue(cs::NO_DAY, 50, 9 * 60 + 59, 10));
  TEST_ASSERT_TRUE(cs::morningDue(cs::NO_DAY, 50, 10 * 60, 10));
  TEST_ASSERT_TRUE(cs::morningDue(49, 50, 15 * 60, 10));
  TEST_ASSERT_FALSE(cs::morningDue(50, 50, 15 * 60, 10));
}

static cs::DayStats producedUntil(int32_t day, int16_t lastMinute) {
  cs::DayStats d;
  d.day = day;
  d.pvFirstMinute = 8 * 60;
  d.pvLastMinute = lastMinute;
  return d;
}

static void test_sunset_not_before_evening_hour() {
  cs::DayStats d = producedUntil(50, 17 * 60);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 19 * 60 + 59, 20));
  TEST_ASSERT_TRUE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60, 20));
}

static void test_sunset_needs_production_that_day() {
  cs::DayStats d = producedUntil(50, -1);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 21 * 60, 20));
  cs::DayStats yesterday = producedUntil(49, 17 * 60);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, yesterday, 50, 21 * 60, 20));
}

static void test_sunset_waits_for_30_quiet_minutes() {
  cs::DayStats d = producedUntil(50, 20 * 60 + 10);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60 + 10, 20));
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60 + 39, 20));
  TEST_ASSERT_TRUE(cs::sunsetDue(cs::NO_DAY, d, 50, 20 * 60 + 40, 20));
}

static void test_sunset_forced_at_23() {
  cs::DayStats d = producedUntil(50, 22 * 60 + 59);
  TEST_ASSERT_FALSE(cs::sunsetDue(cs::NO_DAY, d, 50, 22 * 60 + 59, 20));
  TEST_ASSERT_TRUE(cs::sunsetDue(cs::NO_DAY, d, 50, 23 * 60, 20));
}

static void test_sunset_once_per_day() {
  cs::DayStats d = producedUntil(50, 17 * 60);
  TEST_ASSERT_FALSE(cs::sunsetDue(50, d, 50, 21 * 60, 20));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_day_integrates_known_currents);
  RUN_TEST(test_day_used_is_clamped_at_zero);
  RUN_TEST(test_day_missing_devices_add_zero_and_invalid_shunt_skips);
  RUN_TEST(test_day_pv_first_last_and_peak);
  RUN_TEST(test_day_history_roll_and_find);
  RUN_TEST(test_capacity_learns_200ah);
  RUN_TEST(test_capacity_ignores_high_or_unknown_soc);
  RUN_TEST(test_capacity_smoothing_and_persist_rule);
  RUN_TEST(test_days_remaining_both_figures);
  RUN_TEST(test_days_remaining_solar_covers_usage);
  RUN_TEST(test_days_remaining_uses_fewer_days_and_skips_incomplete);
  RUN_TEST(test_days_remaining_needs_complete_day_and_capacity);
  RUN_TEST(test_morning_due);
  RUN_TEST(test_sunset_not_before_evening_hour);
  RUN_TEST(test_sunset_needs_production_that_day);
  RUN_TEST(test_sunset_waits_for_30_quiet_minutes);
  RUN_TEST(test_sunset_forced_at_23);
  RUN_TEST(test_sunset_once_per_day);
  return UNITY_END();
}
