/* Internal parser/consensus tests, no network or DNS. */
#define PC_DHT_TEST_NO_BOOTSTRAP
#define recvfrom test_recvfrom
#define pc_dht_item_receive test_item_receive
#define dht_periodic test_periodic
#include "../src/pc/net_dht.c"
#undef recvfrom
#undef pc_dht_item_receive
#undef dht_periodic
#include <assert.h>
static unsigned receive_calls, periodic_calls;
static bool consume_packet;
#ifdef _WIN32
int WSAAPI test_recvfrom(SOCKET socket, char* buffer, int length, int flags,
    struct sockaddr* address, int* address_length) {
#else
ssize_t test_recvfrom(int socket, void* buffer, size_t length, int flags, struct sockaddr* address,
    socklen_t* address_length) {
#endif
    (void)socket;
    (void)flags;
    assert(length >= 2 && *address_length >= sizeof(struct sockaddr_in));
    if (receive_calls++)
        return -1;
    memset(address, 0, sizeof(struct sockaddr_in));
    memcpy(buffer, "de", 2);
    return 2;
}
bool test_item_receive(const void* data, size_t length, uint32_t ip, uint16_t port) {
    (void)data;
    (void)length;
    (void)ip;
    (void)port;
    /* Model a completion callback stopping DHT or handing its socket off. */
    fd = -1;
    return consume_packet;
}
int test_periodic(const void* data, size_t length, const struct sockaddr* from, int from_length,
    time_t* sleep, dht_callback_t* callback, void* context) {
    (void)data;
    (void)length;
    (void)from;
    (void)from_length;
    (void)sleep;
    (void)callback;
    (void)context;
    periodic_calls++;
    return 0;
}
static void reply(unsigned network, const unsigned char* wire, size_t len, bool tracked) {
    struct sockaddr_in from = {0};
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = htonl(0x08080001 + (network << 8));
    from.sin_port = htons(6881);
    struct request* r = &requests[network];
    if (tracked) {
        r->ip = from.sin_addr.s_addr;
        r->port = from.sin_port;
        r->len = 4;
        memcpy(r->tid, "pnXX", 4);
        r->sent = SDL_GetTicks();
    }
    observe(wire, len, &from);
}
int main(void) {
    const unsigned char valid[] = "d2:ip6:\x01\x02\x03\x04\x23\x28"
                                  "1:rd2:id20:abcdefghijklmnopqrste1:t4:pnXX1:y1:re";
    const unsigned char nested[] = "d1:rd2:ip6:\x01\x02\x03\x04\x23\x28"
                                   "e1:t4:pnXX1:y1:re";
    struct fields f;
    assert(fields(valid, sizeof(valid) - 1, &f) && f.iplen == 6 && f.tidlen == 4);
    for (size_t i = 0; i < sizeof(valid) - 1; i++)
        assert(!fields(valid, i, &f));
    assert(fields(nested, sizeof(nested) - 1, &f) && !f.ip);
    assert(!fields("d1:t999999999999999999999:", 26, &f));
    fd = 123;
    struct pc_dht_endpoint ep;
    reply(0, valid, sizeof(valid) - 1, false);
    assert(!pc_dht_external_endpoint(&ep));
    reply(0, nested, sizeof(nested) - 1, true);
    assert(!pc_dht_external_endpoint(&ep));
    reply(0, valid, sizeof(valid) - 1, true);
    reply(0, valid, sizeof(valid) - 1, true); /* Same /24 is only one vote. */
    reply(1, valid, sizeof(valid) - 1, true);
    assert(!pc_dht_external_endpoint(&ep));
    reply(2, valid, sizeof(valid) - 1, true);
    assert(pc_dht_external_endpoint(&ep));
    assert(ntohl(ep.address) == 0x01020304 && ep.port == 9000);
    for (unsigned i = 0; i < 16; i++)
        votes[i].when = SDL_GetTicks() - 600001;
    assert(!pc_dht_external_endpoint(&ep));
    fd = -1;
    for (unsigned consumed = 0; consumed < 2; consumed++) {
        fd = 123;
        consume_packet = consumed != 0;
        receive_calls = periodic_calls = 0;
        next_periodic = 0;
        pc_dht_poll();
        assert(fd == -1 && receive_calls == 1 && periodic_calls == 0);
    }
    puts("DHT bounded bencode and independent endpoint consensus tests passed");
}
