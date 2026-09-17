/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MELEE_PC_MUSIC_STREAM_H
#define MELEE_PC_MUSIC_STREAM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Probe if a user-supplied replacement file (.ogg, .wav) exists for the given
 * track stem (e.g. "pstadium", "izumi", "battle", "menu01", etc.) in:
 *   - ~/.local/share/melee-pc/music/
 *   - ./music/ (beside binary or current directory)
 * If found, open and begin streaming it.
 *
 * @param track_stem Track name or stem, with or without directory/extension.
 * @return true if replacement file was found and opened successfully, false otherwise.
 */
bool pc_music_stream_open(const char* track_stem);

/**
 * Stop playback and release the current custom music stream.
 */
void pc_music_stream_stop(void);

/**
 * Set relative stream volume [0.0, 1.0].
 */
void pc_music_stream_set_volume(float vol);

/**
 * Mix num_samples of the custom music stream into output buffers.
 * If dst_right is NULL, dst_left is treated as an interleaved stereo float buffer.
 * If dst_right is non-NULL, dst_left and dst_right receive left and right channels.
 */
void pc_music_stream_mix(float* dst_left, float* dst_right, int num_samples);

/**
 * Check if a custom music stream is currently active and playing.
 */
bool pc_music_stream_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* MELEE_PC_MUSIC_STREAM_H */
