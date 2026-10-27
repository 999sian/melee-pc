/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_INPUT_POLL_H
#define PC_INPUT_POLL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the dedicated 1000 Hz (1 ms) input polling worker thread.
 * Polls physical and virtual controller inputs independently of the 60 Hz frame boundary,
 * eliminating input quantization and latency jitter. */
void pc_input_poll_init(void);

/* Stop and join the 1000 Hz input polling worker thread. */
void pc_input_poll_shutdown(void);

/* Query whether the 1000 Hz input polling thread is active. */
bool pc_input_poll_is_running(void);

/* Query total count of 1000 Hz poll iterations completed. */
uint64_t pc_input_poll_get_count(void);

/* Smoothed poll-thread rate (Hz) for the diagnostic HUD. */
float pc_input_poll_hz(void);

/* Where there is no poll thread (the browser), the frame boundary reads the
 * controllers itself and stamps the read here, so pc_input_latency_record()
 * measures the same thing: how old the sample the tick consumes is. */
void pc_input_note_sample(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_INPUT_POLL_H */
