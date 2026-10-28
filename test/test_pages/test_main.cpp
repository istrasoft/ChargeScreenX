#include <unity.h>

#include "chargescreen_logic.h"

void setUp() {}
void tearDown() {}

static void test_page_visibility() {
  cs::PageAvailability none{false, false, false};
  cs::PageAvailability solarOnly{false, true, false};
  cs::PageAvailability orionOnly{false, false, true};
  cs::PageAvailability demo{true, false, false};
  TEST_ASSERT_TRUE(cs::pageVisible(0, none));
  TEST_ASSERT_FALSE(cs::pageVisible(1, none));
  TEST_ASSERT_TRUE(cs::pageVisible(1, solarOnly));
  TEST_ASSERT_FALSE(cs::pageVisible(2, solarOnly));
  TEST_ASSERT_TRUE(cs::pageVisible(2, orionOnly));
  TEST_ASSERT_TRUE(cs::pageVisible(1, demo));
  TEST_ASSERT_TRUE(cs::pageVisible(2, demo));
  TEST_ASSERT_FALSE(cs::pageVisible(3, demo));
}

static void test_next_visible_page() {
  cs::PageAvailability none{false, false, false};
  cs::PageAvailability solarOnly{false, true, false};
  cs::PageAvailability orionOnly{false, false, true};
  cs::PageAvailability demo{true, false, false};
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(0, 1, 3, none));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(0, -1, 3, none));
  TEST_ASSERT_EQUAL_UINT8(1, cs::nextVisiblePage(0, 1, 2, solarOnly));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(1, 1, 2, solarOnly));
  TEST_ASSERT_EQUAL_UINT8(1, cs::nextVisiblePage(0, -1, 2, solarOnly));
  TEST_ASSERT_EQUAL_UINT8(2, cs::nextVisiblePage(0, 1, 3, orionOnly));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(2, 1, 3, orionOnly));
  TEST_ASSERT_EQUAL_UINT8(1, cs::nextVisiblePage(0, 1, 3, demo));
  TEST_ASSERT_EQUAL_UINT8(2, cs::nextVisiblePage(1, 1, 3, demo));
  TEST_ASSERT_EQUAL_UINT8(0, cs::nextVisiblePage(2, 1, 3, demo));
  TEST_ASSERT_EQUAL_UINT8(2, cs::nextVisiblePage(0, -1, 3, demo));
}

static void test_cycle_seconds() {
  TEST_ASSERT_EQUAL_UINT16(0, cs::sanitiseCycleSeconds(0));
  TEST_ASSERT_EQUAL_UINT16(20, cs::sanitiseCycleSeconds(20));
  TEST_ASSERT_EQUAL_UINT16(60, cs::sanitiseCycleSeconds(60));
  TEST_ASSERT_EQUAL_UINT16(0, cs::sanitiseCycleSeconds(15));
  TEST_ASSERT_EQUAL_UINT16(0, cs::sanitiseCycleSeconds(-5));
  TEST_ASSERT_EQUAL_UINT16(10, cs::nextCycleSeconds(0));
  TEST_ASSERT_EQUAL_UINT16(20, cs::nextCycleSeconds(10));
  TEST_ASSERT_EQUAL_UINT16(30, cs::nextCycleSeconds(20));
  TEST_ASSERT_EQUAL_UINT16(60, cs::nextCycleSeconds(30));
  TEST_ASSERT_EQUAL_UINT16(0, cs::nextCycleSeconds(60));
}

static void test_cycle_due() {
  TEST_ASSERT_FALSE(cs::cycleDue(100000, 0, 0, 0));
  TEST_ASSERT_TRUE(cs::cycleDue(20000, 0, 0, 10));
  TEST_ASSERT_FALSE(cs::cycleDue(20000, 15000, 0, 10));
  TEST_ASSERT_FALSE(cs::cycleDue(20000, 0, 15000, 10));
  TEST_ASSERT_TRUE(cs::cycleDue(10000, 0, 0, 10));
  TEST_ASSERT_FALSE(cs::cycleDue(0x100UL, 0xFFFFFF00UL, 0xFFFFFF00UL, 10));
  TEST_ASSERT_TRUE(cs::cycleDue(0x3000UL, 0xFFFFFF00UL, 0xFFFFFF00UL, 10));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_page_visibility);
  RUN_TEST(test_next_visible_page);
  RUN_TEST(test_cycle_seconds);
  RUN_TEST(test_cycle_due);
  return UNITY_END();
}
