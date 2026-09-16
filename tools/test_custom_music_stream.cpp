/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pc/music_stream.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

// Provide weak fallbacks normally supplied by audio.c / launcher.cpp
static float s_test_music_vol = 1.0f;
extern "C" float pc_get_music_volume(void) {
    return s_test_music_vol;
}
extern "C" float pc_audio_get_music_volume(void) {
    return s_test_music_vol;
}

int main(void) {
    SDL_Init(SDL_INIT_AUDIO);

    std::filesystem::path local_music("./music");
    std::filesystem::create_directories(local_music);

    char* pref = SDL_GetPrefPath("", "melee-pc");
    std::filesystem::path user_music;
    if (pref) {
        user_music = std::filesystem::path(pref) / "music";
        std::filesystem::create_directories(user_music);
        SDL_free(pref);
    }

    // 1. Missing track returns false
    assert(!pc_music_stream_open("nonexistent_track_xyz"));
    assert(!pc_music_stream_is_playing());

    // 2. Test OGG streaming with pstadium
    std::filesystem::copy_file("/tmp/test_sine.ogg", local_music / "pstadium.ogg",
        std::filesystem::copy_options::overwrite_existing);

    assert(pc_music_stream_open("pstadium"));
    assert(pc_music_stream_is_playing());

    float out[160 * 2];
    std::fill(std::begin(out), std::end(out), 0.0f);
    pc_music_stream_mix(out, nullptr, 160);

    bool has_nonzero = false;
    for (int i = 0; i < 320; i++) {
        if (std::abs(out[i]) > 0.001f) {
            has_nonzero = true;
            break;
        }
    }
    assert(has_nonzero);

    // Test volume muting
    pc_music_stream_set_volume(0.0f);
    std::fill(std::begin(out), std::end(out), 0.0f);
    pc_music_stream_mix(out, nullptr, 160);
    for (int i = 0; i < 320; i++) {
        assert(out[i] == 0.0f);
    }

    // Test volume restoration
    pc_music_stream_set_volume(1.0f);
    std::fill(std::begin(out), std::end(out), 0.0f);
    pc_music_stream_mix(out, nullptr, 160);
    has_nonzero = false;
    for (int i = 0; i < 320; i++) {
        if (std::abs(out[i]) > 0.001f) {
            has_nonzero = true;
            break;
        }
    }
    assert(has_nonzero);

    // Test stop
    pc_music_stream_stop();
    assert(!pc_music_stream_is_playing());
    std::fill(std::begin(out), std::end(out), 0.0f);
    pc_music_stream_mix(out, nullptr, 160);
    for (int i = 0; i < 320; i++) {
        assert(out[i] == 0.0f);
    }

    // 3. Test WAV streaming with izumi
    std::filesystem::copy_file("/tmp/test_sine.wav", local_music / "izumi.wav",
        std::filesystem::copy_options::overwrite_existing);

    assert(pc_music_stream_open("audio/izumi.hps"));
    assert(pc_music_stream_is_playing());
    std::fill(std::begin(out), std::end(out), 0.0f);
    pc_music_stream_mix(out, nullptr, 160);
    has_nonzero = false;
    for (int i = 0; i < 320; i++) {
        if (std::abs(out[i]) > 0.001f) {
            has_nonzero = true;
            break;
        }
    }
    assert(has_nonzero);
    pc_music_stream_stop();

    // 4. Test user-supplied music in user directory (~/.local/share/melee-pc/music/)
    if (!user_music.empty()) {
        std::filesystem::copy_file("/tmp/test_sine.ogg", user_music / "battle.ogg",
            std::filesystem::copy_options::overwrite_existing);

        // Probe via "vl_battle" (which matches battle.ogg)
        assert(pc_music_stream_open("vl_battle"));
        assert(pc_music_stream_is_playing());
        pc_music_stream_stop();

        // Probe via "audio/vl_battle.hps"
        assert(pc_music_stream_open("audio/vl_battle.hps"));
        assert(pc_music_stream_is_playing());
        pc_music_stream_stop();
    }

    // 5. Test menu variation probing: menu01 <-> menu1
    std::filesystem::copy_file("/tmp/test_sine.wav", local_music / "menu01.wav",
        std::filesystem::copy_options::overwrite_existing);
    assert(pc_music_stream_open("menu1"));
    assert(pc_music_stream_is_playing());
    pc_music_stream_stop();

    assert(pc_music_stream_open("audio/menu01.hps"));
    assert(pc_music_stream_is_playing());

    // 6. Test looping across multiple seconds (source file is only 1.0s long)
    for (int frame = 0; frame < 500; frame++) {  // 500 frames * 160 samples = 80,000 samples = 2.5s
        std::fill(std::begin(out), std::end(out), 0.0f);
        pc_music_stream_mix(out, nullptr, 160);
    }
    assert(pc_music_stream_is_playing());
    pc_music_stream_stop();
    assert(!pc_music_stream_is_playing());

    // Clean up test files
    std::filesystem::remove(local_music / "pstadium.ogg");
    std::filesystem::remove(local_music / "izumi.wav");
    std::filesystem::remove(local_music / "menu01.wav");
    if (!user_music.empty()) {
        std::filesystem::remove(user_music / "battle.ogg");
    }

    SDL_Quit();
    std::puts("PASS: custom soundtrack streaming (.ogg, .wav, stems, volumes, looping, stop)");
    return 0;
}
