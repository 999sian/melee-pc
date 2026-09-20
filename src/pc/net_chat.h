/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_CHAT_H
#define PC_NET_CHAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PC_NET_CHAT_COUNT 16
#define PC_NET_CHAT_REL 0x15
/* Game/presentation thread only, outside rollback. Buttons are physical
 * local pad bits (GC D-pad 1,2,4,8), never synchronized simulation input.
 * eligible is true only in online lobby, CSS or results. */
void pc_net_chat_poll(uint16_t buttons, bool eligible, uint64_t now_ms);
void pc_net_chat_receive(const void* payload, size_t length);
void pc_net_chat_reset(void);
const char* pc_net_chat_phrase(unsigned id);
const char* pc_net_chat_line(void);
const char* pc_net_chat_prompt(void);
#ifdef __cplusplus
}
#endif
#endif
