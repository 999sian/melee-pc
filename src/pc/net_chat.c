/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_chat.h"
#include "net.h"
#include "net_lan.h"
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <string.h>
static const char* phrases[PC_NET_CHAT_COUNT] = {"Hello", "Good luck", "Have fun", "Ready",
    "Good game", "Well played", "Nice combo", "That was close", "One more", "Last game",
    "Need a moment", "Thanks for waiting", "Thanks for playing", "See you", "Sorry", "No problem"};
static const char* choices[4] = {"UP Hello  RIGHT Good luck  DOWN Have fun  LEFT Ready",
    "UP Good game  RIGHT Well played  DOWN Nice combo  LEFT That was close",
    "UP One more  RIGHT Last game  DOWN Need a moment  LEFT Thanks for waiting",
    "UP Thanks for playing  RIGHT See you  DOWN Sorry  LEFT No problem"};
static struct {
    uint16_t buttons;
    int group;
    bool eligible, sent, received;
    uint64_t selected_at, sent_at, received_at, show_until;
    char line[80];
} chat = {.group = -1};
void pc_net_chat_reset(void) {
    memset(&chat, 0, sizeof chat);
    chat.group = -1;
}
const char* pc_net_chat_phrase(unsigned id) {
    return id < PC_NET_CHAT_COUNT ? phrases[id] : NULL;
}
const char* pc_net_chat_line(void) {
    return chat.line;
}
const char* pc_net_chat_prompt(void) {
    if (!chat.eligible)
        return "";
    return chat.group < 0 ? "CHAT: D-PAD twice  UP Greetings  RIGHT GG  DOWN Set  LEFT Thanks" :
                            choices[chat.group];
}
static void show(unsigned id, bool remote, uint64_t now) {
    snprintf(chat.line, sizeof chat.line, "%s: %s", remote ? "Opponent" : "You", phrases[id]);
    chat.show_until = now + 6000;
}
void pc_net_chat_receive(const void* payload, size_t length) {
    if (!pc_net_active() || !chat.eligible || !payload || length != 1)
        return;
    unsigned id = *(const uint8_t*)payload;
    uint64_t now = SDL_GetTicks();
    if (id >= PC_NET_CHAT_COUNT || (chat.received && now - chat.received_at < 2000))
        return;
    chat.received = true;
    chat.received_at = now;
    show(id, true, now);
}
void pc_net_chat_poll(uint16_t buttons, bool eligible, uint64_t now) {
    uint16_t held = buttons & 15, pressed = held & ~chat.buttons;
    chat.buttons = held;
    if (chat.line[0] && now >= chat.show_until)
        chat.line[0] = 0;
    chat.eligible = eligible && pc_net_active();
    if (!chat.eligible) {
        chat.group = -1;
        return;
    }
    if (chat.group >= 0 && now - chat.selected_at >= 3000)
        chat.group = -1;
    if (!pressed)
        return;
    if ((held & (held - 1)) != 0) {
        chat.group = -1;
        return;
    } /* no diagonal chords */
    int direction = pressed == 8 ? 0 : pressed == 2 ? 1 : pressed == 4 ? 2 : 3;
    if (chat.group < 0) {
        chat.group = direction;
        chat.selected_at = now;
        return;
    }
    uint8_t id = (uint8_t)(chat.group * 4 + direction);
    chat.group = -1;
    if (chat.sent && now - chat.sent_at < 2000)
        return;
    if (pc_net_send_reliable(PC_NET_CHAT_REL, &id, 1)) {
        chat.sent = true;
        chat.sent_at = now;
        show(id, false, now);
    }
}
