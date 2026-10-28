#include <unity.h>

#include <cmath>

#include "chargescreen_logic.h"

void setUp() {}
void tearDown() {}

static void test_parse_orion_xs_values() {
  // state 3 (Bulk), error 0, out 14.20 V, out 32.0 A, in 13.40 V, in 35.5 A, off 0
  const uint8_t plain[] = {0x03, 0x00, 0x8C, 0x05, 0x40, 0x01, 0x3C, 0x05,
                           0x63, 0x01, 0x00, 0x00, 0x00, 0x00};
  cs::OrionXsReading reading;
  TEST_ASSERT_TRUE(cs::parseOrionXs(plain, sizeof(plain), reading));
  TEST_ASSERT_EQUAL_UINT8(3, reading.state);
  TEST_ASSERT_EQUAL_UINT8(0, reading.error);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.20f, reading.outputVoltage);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 32.0f, reading.outputCurrent);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.40f, reading.inputVoltage);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 35.5f, reading.inputCurrent);
  TEST_ASSERT_EQUAL_HEX32(0, reading.offReason);
}

static void test_parse_orion_xs_not_available_and_off_reason() {
  const uint8_t plain[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                           0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00};
  cs::OrionXsReading reading;
  TEST_ASSERT_TRUE(cs::parseOrionXs(plain, sizeof(plain), reading));
  TEST_ASSERT_EQUAL_UINT8(0, reading.state);
  TEST_ASSERT_EQUAL_UINT8(0xFF, reading.error);
  TEST_ASSERT_TRUE(std::isnan(reading.outputVoltage));
  TEST_ASSERT_TRUE(std::isnan(reading.outputCurrent));
  TEST_ASSERT_TRUE(std::isnan(reading.inputVoltage));
  TEST_ASSERT_TRUE(std::isnan(reading.inputCurrent));
  TEST_ASSERT_EQUAL_HEX32(0x80, reading.offReason);
}

static void test_parse_orion_xs_rejects_short_data() {
  const uint8_t plain[13] = {};
  cs::OrionXsReading reading;
  TEST_ASSERT_FALSE(cs::parseOrionXs(plain, sizeof(plain), reading));
}

static void test_off_reason_text() {
  TEST_ASSERT_NULL(cs::orionOffReasonText(0));
  TEST_ASSERT_EQUAL_STRING("Engine off", cs::orionOffReasonText(0x80));
  TEST_ASSERT_EQUAL_STRING("Engine off", cs::orionOffReasonText(0x81));
  TEST_ASSERT_EQUAL_STRING("No input", cs::orionOffReasonText(0x01));
  TEST_ASSERT_EQUAL_STRING("Switched off", cs::orionOffReasonText(0x02));
  TEST_ASSERT_EQUAL_STRING("Switched off", cs::orionOffReasonText(0x04));
  TEST_ASSERT_EQUAL_STRING("Remote off", cs::orionOffReasonText(0x08));
  TEST_ASSERT_EQUAL_STRING("Protection", cs::orionOffReasonText(0x10));
  TEST_ASSERT_EQUAL_STRING("BMS off", cs::orionOffReasonText(0x40));
  TEST_ASSERT_EQUAL_STRING("Checking input", cs::orionOffReasonText(0x100));
  TEST_ASSERT_NULL(cs::orionOffReasonText(0x20));
}

static void test_charger_state_text() {
  TEST_ASSERT_EQUAL_STRING("Off", cs::chargerStateText(0));
  TEST_ASSERT_EQUAL_STRING("Bulk", cs::chargerStateText(3));
  TEST_ASSERT_EQUAL_STRING("Float", cs::chargerStateText(5));
  TEST_ASSERT_EQUAL_STRING("Power supply", cs::chargerStateText(11));
  TEST_ASSERT_EQUAL_STRING("External", cs::chargerStateText(252));
  TEST_ASSERT_EQUAL_STRING("Charging", cs::chargerStateText(99));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_orion_xs_values);
  RUN_TEST(test_parse_orion_xs_not_available_and_off_reason);
  RUN_TEST(test_parse_orion_xs_rejects_short_data);
  RUN_TEST(test_off_reason_text);
  RUN_TEST(test_charger_state_text);
  return UNITY_END();
}
