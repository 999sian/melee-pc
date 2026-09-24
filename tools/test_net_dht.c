/* Offline deterministic topic and socket ownership regression. */
#include "pc/net_dht.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif
static void hash(const char* text, const char* expected) {
    unsigned char digest[20];
    char hex[41];
    pc_dht_sha1(text, strlen(text), digest);
    for (int i = 0; i < 20; i++)
        sprintf(hex + 2 * i, "%02x", digest[i]);
    assert(!strcmp(hex, expected));
}
static void topic(
    enum pc_dht_mode m, const char* code, int band, int64_t minute, const char* text) {
    unsigned char actual[20], expected[20];
    pc_dht_sha1(text, strlen(text), expected);
    assert(pc_dht_topic(m, code, band, minute, actual));
    assert(!memcmp(actual, expected, 20));
}
static int received;
static void packet(const void* data, size_t len, const struct pc_dht_endpoint* from, void* ctx) {
    assert(len == 5 && !memcmp(data, "HELLO", 5));
    assert(from->port && ctx == &received);
    received++;
}
int main(void) {
    hash("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    hash("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    topic(PC_DHT_DIRECT, "ABCDEFGH", 0, 42, "meleepc/v2/direct/ABCDEFGH");
    topic(PC_DHT_UNRANKED, NULL, 0, 123456, "meleepc/v1/unranked/123456");
    topic(PC_DHT_RANKED, NULL, 10, 123456, "meleepc/v1/ranked/10/123456");
    unsigned char out[20];
    assert(!pc_dht_topic(PC_DHT_DIRECT, NULL, 0, 0, out));
    assert(!pc_dht_topic(PC_DHT_RANKED, NULL, -1, 0, out));
    assert(pc_dht_take_socket() == -1);
    pc_dht_stop();
#if defined(PC_DHT_TEST_NO_BOOTSTRAP) && !defined(_WIN32)
    assert(pc_dht_start(PC_DHT_DIRECT, "FOX#ABCD", 0, 0));
    intptr_t owned = pc_dht_socket();
    assert(owned >= 0 && pc_dht_port());
    pc_dht_set_datagram_callback(packet, &received);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sender >= 0);
    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(pc_dht_port());
    assert(sendto(sender, "HELLO", 5, 0, (struct sockaddr*)&target, sizeof(target)) == 5);
    pc_dht_poll();
    assert(received == 1);
    close(sender);
    assert(pc_dht_take_socket() == owned);
    assert(pc_dht_socket() == -1);
    pc_dht_stop();
    assert(fcntl(owned, F_GETFD) >= 0);
    close(owned);
    assert(pc_dht_start(PC_DHT_UNRANKED, NULL, 0, 0));
    owned = pc_dht_socket();
    pc_dht_stop();
    assert(fcntl(owned, F_GETFD) == -1);
#endif
    puts("DHT SHA1/topic and socket ownership tests passed");
    return 0;
}
