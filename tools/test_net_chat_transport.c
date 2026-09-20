/* Reuse the actual reliable-channel fixture and link net_chat.c. */
#include <stdarg.h>
#define main reliable_fixture_main
#include "test_net_reliable.c"
#undef main
#include "../src/pc/net_chat.h"
Uint64 SDL_GetTicks(void) {
    return s_now / 1000000;
}
bool pc_net_active(void) {
    return net.active;
}
int main(void) {
    assert(reliable_fixture_main() == 0);
    rel_reset();
    s_now = 1000000000ull;
    pc_net_chat_poll(0, true, SDL_GetTicks());
    uint8_t phrase = 5;
    assert(pc_net_send_reliable(REL_CHAT, &phrase, 1) && deliver());
    assert(strstr(pc_net_chat_line(), "Opponent: Well played"));
    assert(recv_user() < 0); /* matcher/rank cannot consume chat */
    ack();
    assert(pc_net_send_reliable(REL_CHAT, "xx", 2) && deliver());
    assert(recv_user() < 0);
    ack(); /* invalid payload still ACKed, cannot block queue */
    assert(pc_net_send_reliable(0x61, "body", 4) && deliver());
    uint8_t type = 0;
    char body[4];
    assert(pc_net_recv_reliable(&type, body, sizeof body) == 4 && type == 0x61);
    assert(!memcmp(body, "body", 4));
    ack();
    rel_reset();
    assert(!pc_net_chat_line()[0]);
    puts("PASS: chat dispatch bypasses caller queue, malformed chat cannot block rank messages, "
         "reset clears history");
}
