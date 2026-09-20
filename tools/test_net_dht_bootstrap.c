/* Offline regression for bootstrap pongs arriving after async DNS finishes.
 * Includes the upstream engine so its maintenance deadline can be asserted.
 * Every outgoing datagram is captured; no socket or network calls are made. */
#include "../extern/dht/dht.c"
#include <assert.h>
static unsigned sent;
int dht_random_bytes(void* out, size_t n) {
    memset(out, 0x55, n);
    return (int)n;
}
void dht_hash(
    void* out, int n, const void* a, int na, const void* b, int nb, const void* c, int nc) {
    (void)a;
    (void)na;
    (void)b;
    (void)nb;
    (void)c;
    (void)nc;
    memset(out, 0x33, n);
}
int dht_blacklisted(const struct sockaddr* from, int len) {
    (void)from;
    (void)len;
    return 0;
}
int dht_sendto(
    int socket, const void* data, int len, int flags, const struct sockaddr* to, int tolen) {
    (void)socket;
    (void)data;
    (void)flags;
    (void)to;
    (void)tolen;
    sent++;
    return len;
}
int main(void) {
    unsigned char id[20];
    memset(id, 1, sizeof(id));
    assert(dht_init(123, -1, id, NULL) > 0);
    time_t sleep = 0;
    confirm_nodes_time = 0; /* The first short startup timer elapsed before DNS. */
    assert(dht_periodic(NULL, 0, NULL, 0, &sleep, NULL, NULL) > 0);
    assert(sleep >= 5 && sleep <= 14 && sent == 0); /* Startup stays responsive. */
    struct sockaddr_in router = {0};
    router.sin_family = AF_INET;
    router.sin_port = htons(6881);
    router.sin_addr.s_addr = htonl(0x08080808);
    for (int i = 0; i < 4; i++) {
        char pong[] = "d1:rd2:id20:AAAAAAAAAAAAAAAAAAAAe1:t4:pnAA1:y1:re";
        memset(pong + 12, 'A' + i, 20);
        router.sin_addr.s_addr = htonl(0x08080808 + (i << 8));
        assert(dht_periodic(pong, sizeof(pong) - 1, (struct sockaddr*)&router, sizeof(router),
                   &sleep, NULL, NULL) > 0);
    }
    int good = 0, dubious = 0;
    assert(dht_nodes(AF_INET, &good, &dubious, NULL, NULL) == 4 && good == 4);
    confirm_nodes_time = 0;
    assert(dht_periodic(NULL, 0, NULL, 0, &sleep, NULL, NULL) > 0 && sent > 0);
    for (int batch = 0; batch < 2; batch++) {
        unsigned char packet[2048];
        unsigned char* p = packet;
        memcpy(p, "d1:rd2:id20:", 12);
        p += 12;
        memset(p, 'A', 20);
        p += 20;
        p += sprintf((char*)p, "5:nodes%d:", 16 * 26);
        for (int i = batch * 16; i < (batch + 1) * 16; i++) {
            memset(p, 0x10 + i, 20);
            p += 20;
            uint32_t ip = htonl(0x0b010203 + (i << 24));
            memcpy(p, &ip, 4);
            p += 4;
            *p++ = 0x1a;
            *p++ = 0xe1;
        }
        memcpy(p, "e1:t4:fnAA1:y1:re", 17);
        p += 17;
        *p = 0;
        router.sin_addr.s_addr = htonl(0x08080808);
        assert(dht_periodic(packet, (size_t)(p - packet), (struct sockaddr*)&router, sizeof(router),
                   &sleep, NULL, NULL) > 0);
    }
    dht_nodes(AF_INET, &good, &dubious, NULL, NULL);
    printf("synthetic bootstrap: good=%d dubious=%d outgoing=%u\n", good, dubious, sent);
    assert(good >= 4 && good + dubious >= 30);
    dht_uninit();
    puts("DHT offline bootstrap replies reach the readiness gate");
}
