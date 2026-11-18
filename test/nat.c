#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

static void map(uint64_t id, nat_protocol_t protocol, uint16_t port) {
  TEST_ASSERT_EQUAL_INT(
      0, nat_session_map_request(session, id, 120, protocol, port, 0));
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

static void expect_mapped(uint64_t id, uint16_t external_port) {
  nat_event_t const event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_MAPPED, event.type);
  TEST_ASSERT_EQUAL_UINT64(id, event.mapped.id);
  pcp_ip_address_t const external = pcp_ip_address_from_octets(203, 0, 113, 7);
  TEST_ASSERT_EQUAL_MEMORY(&external, &event.mapped.external.ip,
                           sizeof(external));
  TEST_ASSERT_EQUAL_UINT16(external_port, event.mapped.external.port);
}

static void expect_failed(uint64_t id, nat_result_t result) {
  nat_event_t const event = poll_event();
  TEST_ASSERT_EQUAL(NAT_EVENT_FAILED, event.type);
  TEST_ASSERT_EQUAL_UINT64(id, event.failed.id);
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
  map(7, NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  TEST_ASSERT_EQUAL_UINT8(2, sent[0]);
  TEST_ASSERT_EQUAL_UINT16(12345, read16(&sent[40]));

  answer_pcp(0, 40345);
  expect_mapped(7, 40345);
  expect_idle();
  TEST_ASSERT_EQUAL(NAT_GATEWAY_PROTOCOL_PCP, nat_session_protocol(session));
}

static void test_pcp_suggests_the_last_address(void) {
  map(1, NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  pcp_ip_address_t const any = pcp_ip_address_any_v4;
  TEST_ASSERT_EQUAL_MEMORY(&any, &sent[44], sizeof(any));
  answer_pcp(0, 40345);
  poll_event();

  map(2, NAT_PROTOCOL_TCP, 8080);
  expect_idle();
  pcp_ip_address_t const external = pcp_ip_address_from_octets(203, 0, 113, 7);
  TEST_ASSERT_EQUAL_MEMORY(&external, &sent[44], sizeof(external));
}

static void test_pcp_refusal_is_translated(void) {
  map(7, NAT_PROTOCOL_TCP, 8080);
  expect_idle();
  answer_pcp(2, 0); // NOT_AUTHORIZED
  expect_failed(7, NAT_RESULT_NOT_AUTHORIZED);
  expect_idle();
  TEST_ASSERT_EQUAL_INT(
      0, nat_session_map_request(session, 7, 120, NAT_PROTOCOL_TCP, 8080, 0));
}

// Answers the NAT-PMP map request in `sent` for whichever of the two ports it
// is, expecting the mapping's id back
static void answer_either(uint64_t udp, uint64_t tcp) {
  bool const is_udp = sent[1] == 1;
  uint16_t const port = read16(&sent[4]);
  TEST_ASSERT_EQUAL_UINT16(is_udp ? 12345 : 8080, port);
  answer_natpmp_map(0, (uint16_t)(port + 28000));
  expect_mapped(is_udp ? udp : tcp, (uint16_t)(port + 28000));
}

static void test_fall_back_keeps_ids(void) {
  map(41, NAT_PROTOCOL_UDP, 1);
  map(42, NAT_PROTOCOL_UDP, 12345);
  map(43, NAT_PROTOCOL_TCP, 8080);
  // Moves the last mapping into the first one's place
  nat_session_release(session, 41);
  expect_idle();
  fall_back();

  uint8_t const first = sent[1];
  answer_either(42, 43);
  expect_idle();
  TEST_ASSERT_NOT_EQUAL_UINT8(first, sent[1]);
  answer_either(42, 43);
  expect_idle();
}

static void test_natpmp_refusal_is_translated(void) {
  map(7, NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  fall_back();
  answer_natpmp_map(2, 0); // NOT_AUTHORIZED
  expect_failed(7, NAT_RESULT_NOT_AUTHORIZED);
}

static void test_natpmp_unreachable_fails(void) {
  map(7, NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  fall_back();
  nat_session_on_host_unreachable(session);
  expect_failed(7, NAT_RESULT_UNREACHABLE);
}

static void test_release_deletes(void) {
  map(7, NAT_PROTOCOL_UDP, 12345);
  expect_idle();
  answer_pcp(0, 40345);
  expect_mapped(7, 40345);
  expect_idle();

  nat_session_release(session, 7);
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
  map(1, NAT_PROTOCOL_UDP, 1);
  map(2, NAT_PROTOCOL_UDP, 2);
  map(3, NAT_PROTOCOL_UDP, 3);
  TEST_ASSERT_EQUAL_INT(-ENOBUFS, nat_session_map_request(
                                      session, 4, 120, NAT_PROTOCOL_UDP, 4, 0));

  nat_session_release(session, 2);
  map(4, NAT_PROTOCOL_UDP, 4);
}

// Runs the test named on the command line, or all of them
#define RUN(test)                                                              \
  if (argc < 2 || strcmp(argv[1], #test) == 0) {                               \
    RUN_TEST(test);                                                            \
  }

int main(int argc, char *argv[]) {
  UNITY_BEGIN();
  RUN(test_pcp_maps);
  RUN(test_pcp_suggests_the_last_address);
  RUN(test_pcp_refusal_is_translated);
  RUN(test_fall_back_keeps_ids);
  RUN(test_natpmp_refusal_is_translated);
  RUN(test_natpmp_unreachable_fails);
  RUN(test_release_deletes);
  RUN(test_full_session_refuses);
  if (Unity.NumberOfTests == 0) {
    fprintf(stderr, "no test named %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  return UNITY_END();
}
