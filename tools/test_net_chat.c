#include "pc/net_chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL_timer.h>
static bool active = true, queue_full;
static uint64_t now;
static unsigned sends;
static uint8_t last_id;
Uint64 SDL_GetTicks(void) {
    return now;
}
bool pc_net_active(void) {
    return active;
}
bool pc_net_send_reliable(uint8_t type, const void* data, int size) {
    assert(type == PC_NET_CHAT_REL && size == 1);
    if (queue_full)
        return false;
    last_id = *(const uint8_t*)data;
    sends++;
    return true;
}
static void press(unsigned bit, bool eligible) {
    pc_net_chat_poll(0, eligible, now);
    pc_net_chat_poll(bit, eligible, now);
}
int main(void) {
    static const unsigned directions[] = {8, 2, 4, 1};
    for (unsigned id = 0; id < 16; id++) {
        pc_net_chat_reset();
        now += 2000;
        press(directions[id / 4], true);
        press(directions[id % 4], true);
        assert(last_id == id && strstr(pc_net_chat_line(), pc_net_chat_phrase(id)));
    }
    assert(!pc_net_chat_phrase(16));
    unsigned before = sends;
    press(8, true);
    press(8, true);
    assert(sends == before); /* send cooldown */
    now += 2000;
    press(8, false);
    press(8, false);
    assert(sends == before);
    press(8, true);
    pc_net_chat_poll(8, true, now);
    assert(sends == before); /* held != second press */
    now += 3001;
    press(2, true);
    assert(sends == before); /* selection expired */
    press(2, true);
    assert(sends == before + 1 && last_id == 5);
    pc_net_chat_reset();
    queue_full = true;
    press(8, true);
    press(8, true);
    assert(sends == before + 1 && !pc_net_chat_line()[0]);
    queue_full = false;
    press(8, true);
    press(8, true);
    assert(sends == before + 2);
    uint8_t id = 15, bad = 16;
    pc_net_chat_receive(&bad, 1);
    assert(strstr(pc_net_chat_line(), "You"));
    pc_net_chat_receive(&id, 0);
    pc_net_chat_receive(&id, 2);
    pc_net_chat_receive(NULL, 1);
    assert(strstr(pc_net_chat_line(), "You"));
    pc_net_chat_receive(&id, 1);
    assert(strstr(pc_net_chat_line(), "Opponent: No problem"));
    id = 0;
    pc_net_chat_receive(&id, 1);
    assert(strstr(pc_net_chat_line(), "No problem"));
    now += 2000;
    pc_net_chat_receive(&id, 1);
    assert(strstr(pc_net_chat_line(), "Opponent: Hello"));
    now += 6001;
    pc_net_chat_poll(0, true, now);
    assert(!pc_net_chat_line()[0]);
    active = false;
    press(8, true);
    press(8, true);
    pc_net_chat_receive(&id, 1);
    assert(!pc_net_chat_line()[0] && !pc_net_chat_prompt()[0]);
    puts("PASS: 16 phrase IDs, edge selection, timeouts, send/receive limits, queue failure, "
         "malformed payloads and scene/connection gating");
}
