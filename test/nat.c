#include <errno.h>
#include <string.h>

#include <unity.h>

#include "nat.h"

static nat_session_t *session;
static uint64_t const now = 1000;
// The last datagram the session sent
static uint8_t sent[NAT_PACKET_MAX_SIZE];
static size_t sent_size;

static bool fill_counting(void * /*ctx*/, size_t size, uint8_t data[]) {
  static uint8_t next;
  for (size_t i = 0; i < size; i++) {
    data[i] = next++;
  }
  return true;
}

void setUp(void) {
  session = nat_session_begin((nat_config_t){
      .local = pcp_ip_address_from_octets(10, 0, 0, 2),
      .rng = {.fill = fill_counting},
      .capacity = 3,
  });
  TEST_ASSERT_NOT_NULL(session);
  sent_size = 0;
}

void tearDown(void) { nat_session_end(session); }

// The next event other than NAT_EVENT_SEND, keeping what was sent before it
static nat_event_t poll_event(void) {
  for (;;) {
    nat_event_t const event = nat_session_poll(session, now);
    if (event.type != NAT_EVENT_SEND) {
      return event;
    }
    TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(sent), event.send.size);
    memcpy(sent, event.send.data, event.send.size);
    sent_size = event.send.size;
  }
}

static void expect_idle(void) {
  TEST_ASSERT_EQUAL(NAT_EVENT_TIMEOUT, poll_event().type);
}

static nat_handle_t map(nat_protocol_t protocol, uint16_t port) {
  nat_handle_or_error_t const handle =
      nat_session_map_request(session, 120, protocol, port, 0);
  TEST_ASSERT_TRUE(handle.has_value);
  return handle.value;
}

static uint16_t read16(uint8_t const data[]) {
  return (uint16_t)(data[0] << 8 | data[1]);
}

static void write16(uint8_t data[], uint16_t value) {
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

// Answers the MAP request in `sent`
static void answer_pcp(uint8_t result, uint16_t external_port) {
  TEST_ASSERT_EQUAL_size_t(60, sent_size);
  uint8_t response[60] = {2, 0x81, 0, result};
  memcpy(&response[4], &sent[4], 4);    // the lifetime asked for
  response[11] = 10;                    // epoch
  memcpy(&response[24], &sent[24], 18); // nonce, protocol and internal port
  write16(&response[42], external_port);
  uint8_t const external[] = {0xff, 0xff, 203, 0, 113, 7};
  memcpy(&response[54], external, sizeof(external));
  nat_session_on_datagram(session, now, sizeof(response), response);
}

// What a router that only speaks NAT-PMP answers PCP with
static void answer_unsupported_version(void) {
  uint8_t const response[] = {0, 0, 0, 1, 0, 0, 0, 5};
  nat_session_on_datagram(session, now, sizeof(response), response);
}

// Answers the external address request in `sent`
static void answer_natpmp_address(void) {
  TEST_ASSERT_EQUAL_size_t(2, sent_size);
  uint8_t const response[] = {0, 128, 0, 0, 0, 0, 0, 10, 203, 0, 113, 7};
  nat_session_on_datagram(session, now, sizeof(response), response);
}

// Answers the map request in `sent`
static void answer_natpmp_map(uint16_t result, uint16_t external_port) {
  TEST_ASSERT_EQUAL_size_t(12, sent_size);
  uint8_t response[16] = {0, (uint8_t)(128 + sent[1])};
  write16(&response[2], result);
  response[7] = 10; // epoch
  memcpy(&response[8], &sent[4], 2);
  write16(&response[10], external_port);
  memcpy(&response[12], &sent[8], 4);
  nat_session_on_datagram(session, now, sizeof(response), response);
}

static void expect_mapped(nat_handle_t handle, uint16_t external_port) {
  nat_event_t const event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_MAPPED, event.type);
  TEST_ASSERT_EQUAL_UINT16(handle, event.mapped.handle);
  pcp_ip_address_t const external = pcp_ip_address_from_octets(203, 0, 113, 7);
  TEST_ASSERT_EQUAL_MEMORY(&external, &event.mapped.external.ip,
                           sizeof(external));
  TEST_ASSERT_EQUAL_UINT16(external_port, event.mapped.external.port);
}

static void expect_failed(nat_handle_t handle, nat_result_t result) {
  nat_event_t const event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_FAILED, event.type);
  TEST_ASSERT_EQUAL_UINT16(handle, event.failed.handle);
  TEST_ASSERT_EQUAL(result, event.failed.result);
}

// Falls back from the PCP requests already sent, up to the first NAT-PMP map
// request
static void fall_back(void) {
  answer_unsupported_version();
  TEST_ASSERT_EQUAL(NAT_EVENT_PROTOCOL, poll_event().type);
  TEST_ASSERT_EQUAL(NAT_GATEWAY_PROTOCOL_NATPMP, nat_session_protocol(session));

  expect_idle();
  answer_natpmp_address();
  nat_event_t const event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_ADDRESS, event.type);
  pcp_ip_address_t const external = pcp_ip_address_from_octets(203, 0, 113, 7);
  TEST_ASSERT_EQUAL_MEMORY(&external, &event.address.external,
                           sizeof(external));
  expect_idle();
}

static void test_pcp_maps(void) {
  nat_handle_t const handle = map(NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  TEST_ASSERT_EQUAL_UINT8(2, sent[0]);
  TEST_ASSERT_EQUAL_UINT16(12345, read16(&sent[40]));

  answer_pcp(0, 40345);
  expect_mapped(handle, 40345);
  expect_idle();
  TEST_ASSERT_EQUAL(NAT_GATEWAY_PROTOCOL_PCP, nat_session_protocol(session));
}

static void test_pcp_refusal_is_translated(void) {
  nat_handle_t const handle = map(NAT_PROTOCOL_TCP, 8080);
  expect_idle();
  answer_pcp(2, 0); // NOT_AUTHORIZED
  expect_failed(handle, NAT_RESULT_NOT_AUTHORIZED);
}

static void test_fall_back_keeps_handles(void) {
  nat_handle_t const released = map(NAT_PROTOCOL_UDP, 1);
  nat_handle_t const udp = map(NAT_PROTOCOL_UDP, 12345);
  nat_handle_t const tcp = map(NAT_PROTOCOL_TCP, 8080);
  // So the NAT-PMP session numbers the other two differently
  nat_session_release(session, released);
  expect_idle();
  fall_back();

  TEST_ASSERT_EQUAL_UINT8(1, sent[1]);
  TEST_ASSERT_EQUAL_UINT16(12345, read16(&sent[4]));
  answer_natpmp_map(0, 40345);
  expect_mapped(udp, 40345);

  expect_idle();
  TEST_ASSERT_EQUAL_UINT8(2, sent[1]);
  TEST_ASSERT_EQUAL_UINT16(8080, read16(&sent[4]));
  answer_natpmp_map(0, 40080);
  expect_mapped(tcp, 40080);
}

static void test_natpmp_refusal_is_translated(void) {
  nat_handle_t const handle = map(NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  fall_back();
  answer_natpmp_map(2, 0); // NOT_AUTHORIZED
  expect_failed(handle, NAT_RESULT_NOT_AUTHORIZED);
}

static void test_natpmp_unreachable_fails(void) {
  nat_handle_t const handle = map(NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  fall_back();
  nat_session_on_host_unreachable(session);
  expect_failed(handle, NAT_RESULT_UNREACHABLE);
}

static void test_release_deletes(void) {
  nat_handle_t const handle = map(NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  answer_pcp(0, 40345);
  expect_mapped(handle, 40345);
  expect_idle();

  nat_session_release(session, handle);
  nat_event_t event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_TIMEOUT, event.type);
  uint8_t const no_lifetime[4] = {};
  TEST_ASSERT_EQUAL_MEMORY(no_lifetime, &sent[4], sizeof(no_lifetime));

  answer_pcp(0, 0);
  event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_TIMEOUT, event.type);
  TEST_ASSERT_FALSE(event.timeout.deadline.has_value);
}

static void test_full_session_refuses(void) {
  map(NAT_PROTOCOL_UDP, 1);
  nat_handle_t const second = map(NAT_PROTOCOL_UDP, 2);
  map(NAT_PROTOCOL_UDP, 3);
  nat_handle_or_error_t const fourth =
      nat_session_map_request(session, 120, NAT_PROTOCOL_UDP, 4, 0);
  TEST_ASSERT_FALSE(fourth.has_value);
  TEST_ASSERT_EQUAL_INT(ENOBUFS, fourth.error);

  nat_session_release(session, second);
  TEST_ASSERT_EQUAL_UINT16(second, map(NAT_PROTOCOL_UDP, 4));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_pcp_maps);
  RUN_TEST(test_pcp_refusal_is_translated);
  RUN_TEST(test_fall_back_keeps_handles);
  RUN_TEST(test_natpmp_refusal_is_translated);
  RUN_TEST(test_natpmp_unreachable_fails);
  RUN_TEST(test_release_deletes);
  RUN_TEST(test_full_session_refuses);
  return UNITY_END();
}
