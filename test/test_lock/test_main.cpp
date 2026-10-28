#include <unity.h>

#include <cmath>
#include <cstring>

#include "chargescreen_logic.h"

void setUp() {}
void tearDown() {}

static void test_read_bits_spans_bytes() {
  const uint8_t data[] = {0xAB, 0xCD};
  TEST_ASSERT_EQUAL_HEX32(0xDA, cs::readBits(data, sizeof(data), 4, 8));
  TEST_ASSERT_EQUAL_HEX32(0xCDAB, cs::readBits(data, sizeof(data), 0, 16));
}

static void test_read_bits_stops_at_end() {
  const uint8_t data[] = {0xFF};
  TEST_ASSERT_EQUAL_HEX32(0xFF, cs::readBits(data, sizeof(data), 0, 16));
}

static void test_sign_extend() {
  TEST_ASSERT_EQUAL_INT32(-1, cs::signExtend(0x3FFFFF, 22));
  TEST_ASSERT_EQUAL_INT32(-2097152, cs::signExtend(0x200000, 22));
  TEST_ASSERT_EQUAL_INT32(5, cs::signExtend(5, 22));
}

static void test_normalise_mac() {
  char out[13];
  TEST_ASSERT_TRUE(cs::normaliseMac("AA:bb:CC:dd:EE:ff", out));
  TEST_ASSERT_EQUAL_STRING("aabbccddeeff", out);
  TEST_ASSERT_FALSE(cs::normaliseMac("aa:bb:cc", out));
  TEST_ASSERT_FALSE(cs::normaliseMac("aa:bb:cc:dd:ee:ff:00", out));
  TEST_ASSERT_FALSE(cs::normaliseMac("", out));
}

static void test_format_mac() {
  char out[18];
  cs::formatMac("aabbccddeeff", out);
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", out);
}

static void test_plausibility() {
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 80.0f));
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 102.3f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(4.0f, 80.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(71.0f, 80.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, 150.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, -1.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(NAN, 80.0f));
  TEST_ASSERT_TRUE(cs::plausibleOptionalVoltage(NAN));
  TEST_ASSERT_TRUE(cs::plausibleOptionalVoltage(14.4f));
  TEST_ASSERT_FALSE(cs::plausibleOptionalVoltage(2.0f));
}

static void test_plausible_shunt_current() {
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 80.0f, 25.5f));
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 102.3f, -250.0f));
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 80.0f, 1000.0f));
  TEST_ASSERT_TRUE(cs::plausibleShunt(13.2f, 80.0f, -1000.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, 80.0f, 1500.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, 80.0f, -2000.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, 80.0f, NAN));
  TEST_ASSERT_FALSE(cs::plausibleShunt(4.0f, 80.0f, 10.0f));
  TEST_ASSERT_FALSE(cs::plausibleShunt(13.2f, 150.0f, 10.0f));
}

static void test_plausible_solar() {
  TEST_ASSERT_TRUE(cs::plausibleSolar(3, 13.6f, 250.0f));
  TEST_ASSERT_TRUE(cs::plausibleSolar(0, 12.8f, 0.0f));
  TEST_ASSERT_TRUE(cs::plausibleSolar(5, NAN, NAN));
  TEST_ASSERT_TRUE(cs::plausibleSolar(4, 57.6f, 15000.0f));
  TEST_ASSERT_FALSE(cs::plausibleSolar(99, 13.6f, 250.0f));
  TEST_ASSERT_FALSE(cs::plausibleSolar(0xFF, 13.6f, 250.0f));
  TEST_ASSERT_FALSE(cs::plausibleSolar(3, 2.0f, 250.0f));
  TEST_ASSERT_FALSE(cs::plausibleSolar(3, 13.6f, 40000.0f));
  TEST_ASSERT_FALSE(cs::plausibleSolar(3, 13.6f, -5.0f));
}

static void test_unlocked_accepts_any_address() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  TEST_ASSERT_TRUE(cs::lockAccepts(lock, "aabbccddeeff"));
  TEST_ASSERT_TRUE(cs::lockAccepts(lock, "112233445566"));
}

static void test_locks_after_three_good_decodes() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 100));
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 200));
  TEST_ASSERT_TRUE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 300));
  TEST_ASSERT_TRUE(cs::lockIsLocked(lock));
  TEST_ASSERT_EQUAL_STRING("aabbccddeeff", lock.locked);
  TEST_ASSERT_EQUAL_UINT32(300, lock.lastLockedSeenMs);
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 400));
}

static void test_other_address_resets_candidate() {
  cs::DeviceLock lock;
  cs::lockNoteGoodDecode(lock, "aabbccddeeff", 100);
  cs::lockNoteGoodDecode(lock, "aabbccddeeff", 200);
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "112233445566", 300));
  TEST_ASSERT_FALSE(cs::lockNoteGoodDecode(lock, "aabbccddeeff", 400));
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  TEST_ASSERT_EQUAL_UINT8(1, lock.candidateCount);
}

static void test_locked_rejects_other_addresses() {
  cs::DeviceLock lock;
  TEST_ASSERT_TRUE(cs::lockRestore(lock, "AA:BB:CC:DD:EE:FF", 0));
  TEST_ASSERT_TRUE(cs::lockAccepts(lock, "aabbccddeeff"));
  TEST_ASSERT_FALSE(cs::lockAccepts(lock, "112233445566"));
}

static void test_note_seen_refreshes_only_locked_address() {
  cs::DeviceLock lock;
  cs::lockRestore(lock, "aabbccddeeff", 1000);
  cs::lockNoteSeen(lock, "112233445566", 5000);
  TEST_ASSERT_EQUAL_UINT32(1000, lock.lastLockedSeenMs);
  cs::lockNoteSeen(lock, "aabbccddeeff", 6000);
  TEST_ASSERT_EQUAL_UINT32(6000, lock.lastLockedSeenMs);
}

static void test_lock_expiry_with_wraparound() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 999999, 600000));
  cs::lockRestore(lock, "aabbccddeeff", 1000);
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 600999, 600000));
  TEST_ASSERT_TRUE(cs::lockExpired(lock, 601000, 600000));
  cs::lockRestore(lock, "aabbccddeeff", 0xFFFFFF00UL);
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 0x100UL, 600000));
}

// The BLE task can stamp lastLockedSeenMs after the watchdog read `now`.
static void test_lock_not_expired_when_seen_after_now() {
  cs::DeviceLock lock;
  cs::lockRestore(lock, "aabbccddeeff", 100005);
  TEST_ASSERT_FALSE(cs::lockExpired(lock, 100000, 600000));
}

static void test_lock_expired_across_wraparound() {
  cs::DeviceLock lock;
  cs::lockRestore(lock, "aabbccddeeff", 0xFFFFFF00UL);
  TEST_ASSERT_TRUE(cs::lockExpired(lock, 0xFFFFFF00UL + 600000UL, 600000));
}

static void test_restore_rejects_bad_value_and_clear_unlocks() {
  cs::DeviceLock lock;
  TEST_ASSERT_FALSE(cs::lockRestore(lock, "", 0));
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  cs::lockRestore(lock, "aabbccddeeff", 0);
  cs::lockClear(lock);
  TEST_ASSERT_FALSE(cs::lockIsLocked(lock));
  TEST_ASSERT_EQUAL_UINT8(0, lock.candidateCount);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_read_bits_spans_bytes);
  RUN_TEST(test_read_bits_stops_at_end);
  RUN_TEST(test_sign_extend);
  RUN_TEST(test_normalise_mac);
  RUN_TEST(test_format_mac);
  RUN_TEST(test_plausibility);
  RUN_TEST(test_plausible_shunt_current);
  RUN_TEST(test_plausible_solar);
  RUN_TEST(test_unlocked_accepts_any_address);
  RUN_TEST(test_locks_after_three_good_decodes);
  RUN_TEST(test_other_address_resets_candidate);
  RUN_TEST(test_locked_rejects_other_addresses);
  RUN_TEST(test_note_seen_refreshes_only_locked_address);
  RUN_TEST(test_lock_expiry_with_wraparound);
  RUN_TEST(test_lock_not_expired_when_seen_after_now);
  RUN_TEST(test_lock_expired_across_wraparound);
  RUN_TEST(test_restore_rejects_bad_value_and_clear_unlocks);
  return UNITY_END();
}
