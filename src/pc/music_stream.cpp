/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "music_stream.h"
#include "stb_vorbis.h"
#include "pc.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex s_mutex;
SDL_AudioStream* s_stream = nullptr;
std::vector<uint8_t> s_pcm_data;
size_t s_pcm_offset = 0;
float s_stream_volume = 1.0f;
bool s_playing = false;

static std::string to_lower(const std::string& str) {
    std::string out = str;
    std::transform(
        out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

static std::vector<std::filesystem::path> get_search_directories() {
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;

    // 1. User preferences directory: ~/.local/share/melee-pc/music/
    char* pref = SDL_GetPrefPath("", "melee-pc");
    if (pref != nullptr) {
        dirs.push_back(std::filesystem::path(pref) / "music");
        SDL_free(pref);
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        dirs.push_back(std::filesystem::path(home) / ".local" / "share" / "melee-pc" / "music");
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr) {
        dirs.push_back(std::filesystem::path(xdg) / "melee-pc" / "music");
    }

    // 2. Directory beside binary: <base>/music/
    const char* base = SDL_GetBasePath();
    if (base != nullptr) {
        dirs.push_back(std::filesystem::path(base) / "music");
    }

    // 3. Current working directory: ./music/
    dirs.push_back(std::filesystem::current_path(ec) / "music");
    dirs.push_back(std::filesystem::path("music"));

    // Deduplicate while preserving search order
    std::vector<std::filesystem::path> unique_dirs;
    for (const auto& d : dirs) {
        bool exists = false;
        for (const auto& u : unique_dirs) {
            if (d == u) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            unique_dirs.push_back(d);
        }
    }
    return unique_dirs;
}

static std::vector<std::string> generate_candidate_stems(const char* track_stem) {
    std::vector<std::string> candidates;
    if (track_stem == nullptr || track_stem[0] == '\0') {
        return candidates;
    }

    std::string stem(track_stem);

    // Strip leading directories
    size_t last_slash = stem.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        stem = stem.substr(last_slash + 1);
    }

    // Strip extension if present
    size_t last_dot = stem.find_last_of('.');
    if (last_dot != std::string::npos) {
        stem = stem.substr(0, last_dot);
    }

    if (stem.empty()) {
        return candidates;
    }

    std::string lower_stem = to_lower(stem);

    auto add_candidate = [&](const std::string& s) {
        if (!s.empty() && std::find(candidates.begin(), candidates.end(), s) == candidates.end()) {
            candidates.push_back(s);
        }
    };

    // Primary candidates
    add_candidate(stem);
    add_candidate(lower_stem);

    // If starts with "vl_", also check without "vl_" prefix (e.g. "vl_battle" -> "battle")
    if (lower_stem.rfind("vl_", 0) == 0 && lower_stem.length() > 3) {
        add_candidate(lower_stem.substr(3));
    } else {
        // Conversely, if doesn't start with "vl_", also probe "vl_" prefix (e.g. "battle" ->
        // "vl_battle")
        add_candidate("vl_" + lower_stem);
    }

    // Menu variations: menu01 <-> menu1, menu02 <-> menu2, menu3 <-> menu03, etc.
    if (lower_stem == "menu01") {
        add_candidate("menu1");
        add_candidate("menu_01");
        add_candidate("menu_1");
    } else if (lower_stem == "menu1") {
        add_candidate("menu01");
        add_candidate("menu_1");
        add_candidate("menu_01");
    } else if (lower_stem == "menu02") {
        add_candidate("menu2");
        add_candidate("menu_02");
        add_candidate("menu_2");
    } else if (lower_stem == "menu2") {
        add_candidate("menu02");
        add_candidate("menu_2");
        add_candidate("menu_02");
    } else if (lower_stem == "menu3") {
        add_candidate("menu03");
        add_candidate("menu_3");
        add_candidate("menu_03");
    } else if (lower_stem == "menu03") {
        add_candidate("menu3");
        add_candidate("menu_03");
        add_candidate("menu_3");
    }

    // Stadium variations: pstadium <-> pokesta
    if (lower_stem == "pstadium") {
        add_candidate("pokesta");
    } else if (lower_stem == "pokesta") {
        add_candidate("pstadium");
    }

    // Notice fanfare variations: s_info1..3 <-> notice / fanfare / achievement / info
    if (lower_stem == "s_info1" || lower_stem == "s_info2" || lower_stem == "s_info3") {
        add_candidate("notice");
        add_candidate("fanfare");
        add_candidate("achievement");
        add_candidate("unlock");
        add_candidate("info");
        add_candidate("s_info");
        if (lower_stem == "s_info1") {
            add_candidate("notice1");
            add_candidate("fanfare1");
            add_candidate("info1");
        } else if (lower_stem == "s_info2") {
            add_candidate("notice2");
            add_candidate("fanfare2");
            add_candidate("info2");
        } else if (lower_stem == "s_info3") {
            add_candidate("notice3");
            add_candidate("fanfare3");
            add_candidate("info3");
        }
    }

    return candidates;
}

static std::string s_override_path; /* pc_music_stream_override_next */
/* Replacement tracks loop like the BGM they stand in for; an override (a
 * character pack's victory fanfare) plays once and then stops. */
static bool s_loop = true;
static bool s_override_loop = false; /* how the pending override plays */

static std::string probe_audio_file(const char* track_stem) {
    std::error_code path_ec;
    /* A full path to an existing file is used as is (character packs). */
    if (std::filesystem::is_regular_file(track_stem, path_ec)) {
        return track_stem;
    }
    auto search_dirs = get_search_directories();
    auto candidates = generate_candidate_stems(track_stem);

    const char* extensions[] = {".ogg", ".wav", ".OGG", ".WAV"};
    std::error_code ec;

    for (const auto& dir : search_dirs) {
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            continue;
        }

        // 1. Direct path check
        for (const auto& cand : candidates) {
            for (const char* ext : extensions) {
                auto file_path = dir / (cand + ext);
                if (std::filesystem::is_regular_file(file_path, ec)) {
                    return file_path.string();
                }
            }
        }

        // 2. Case-insensitive directory scan
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string entry_stem = to_lower(entry.path().stem().string());
            std::string entry_ext = to_lower(entry.path().extension().string());

            if (entry_ext == ".ogg" || entry_ext == ".wav") {
                for (const auto& cand : candidates) {
                    if (entry_stem == to_lower(cand)) {
                        return entry.path().string();
                    }
                }
            }
        }
    }

    return "";
}

static void pump_stream_locked() {
    if (!s_stream || s_pcm_data.empty()) {
        return;
    }

    // Keep at least 64 KiB converted data queued
    while (SDL_GetAudioStreamAvailable(s_stream) < 64 * 1024) {
        size_t remaining = s_pcm_data.size() - s_pcm_offset;
        if (remaining == 0) {
            if (!s_loop) {
                SDL_FlushAudioStream(s_stream); /* let the tail drain */
                break;
            }
            s_pcm_offset = 0;
            remaining = s_pcm_data.size();
        }

        size_t chunk = std::min(remaining, (size_t)(32 * 1024));
        if (chunk == 0) {
            break;
        }

        if (!SDL_PutAudioStreamData(s_stream, s_pcm_data.data() + s_pcm_offset, (int)chunk)) {
            break;
        }

        s_pcm_offset += chunk;
        if (s_pcm_offset >= s_pcm_data.size() && s_loop) {
            s_pcm_offset = 0;
        }
    }
}

static void stop_locked() {
    s_playing = false;
    if (s_stream) {
        SDL_DestroyAudioStream(s_stream);
        s_stream = nullptr;
    }
    s_pcm_data.clear();
    s_pcm_offset = 0;
}

}  // namespace

extern "C" bool pc_music_stream_open(const char* track_stem) {
    if (track_stem == nullptr || track_stem[0] == '\0') {
        return false;
    }

    std::string file_path;
    bool once = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_override_path.empty()) {
            file_path = s_override_path;
            s_override_path.clear();
            once = !s_override_loop;
        }
    }
    if (file_path.empty()) {
        file_path = probe_audio_file(track_stem);
    }
    if (file_path.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    stop_locked();

    std::string ext = to_lower(std::filesystem::path(file_path).extension().string());
    SDL_AudioSpec src_spec{};

    if (ext == ".ogg") {
        int channels = 0;
        int sample_rate = 0;
        short* decoded = nullptr;
        int samples_per_channel =
            stb_vorbis_decode_filename(file_path.c_str(), &channels, &sample_rate, &decoded);

        if (samples_per_channel <= 0 || decoded == nullptr || channels <= 0 || sample_rate <= 0) {
            if (decoded) {
                free(decoded);
            }
            return false;
        }

        size_t byte_count = (size_t)samples_per_channel * (size_t)channels * sizeof(short);
        s_pcm_data.assign((const uint8_t*)decoded, (const uint8_t*)decoded + byte_count);
        free(decoded);

        src_spec.format = SDL_AUDIO_S16;
        src_spec.channels = (Uint8)channels;
        src_spec.freq = sample_rate;
    } else if (ext == ".wav") {
        Uint8* wav_buffer = nullptr;
        Uint32 wav_length = 0;
        if (!SDL_LoadWAV(file_path.c_str(), &src_spec, &wav_buffer, &wav_length) || !wav_buffer ||
            wav_length == 0)
        {
            return false;
        }

        s_pcm_data.assign(wav_buffer, wav_buffer + wav_length);
        SDL_free(wav_buffer);
    } else {
        return false;
    }

    const SDL_AudioSpec dst_spec = {SDL_AUDIO_F32, 2, 32000};
    s_stream = SDL_CreateAudioStream(&src_spec, &dst_spec);
    if (!s_stream) {
        s_pcm_data.clear();
        return false;
    }

    s_pcm_offset = 0;
    s_stream_volume = 1.0f;
    s_playing = true;
    s_loop = !once;

    pump_stream_locked();
    return true;
}

extern "C" void pc_music_stream_override_next(const char* path) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_override_path = path != nullptr ? path : "";
    s_override_loop = false;
}

extern "C" void pc_music_stream_override_next_loop(const char* path) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_override_path = path != nullptr ? path : "";
    s_override_loop = true;
}

/* ---- one-shot clips ------------------------------------------------------ */

namespace {
struct Clip {
    std::string path;
    std::vector<float> pcm; /* interleaved stereo F32 at the mixer rate */
};
std::vector<Clip> s_clips;
const Clip* s_clip_playing = nullptr;
size_t s_clip_pos = 0;

bool decode_clip(const std::string& path, std::vector<float>& out) {
    SDL_AudioSpec src{};
    std::vector<uint8_t> raw;
    std::string ext = to_lower(std::filesystem::path(path).extension().string());
    if (ext == ".ogg") {
        int channels = 0, rate = 0;
        short* decoded = nullptr;
        int frames = stb_vorbis_decode_filename(path.c_str(), &channels, &rate, &decoded);
        if (frames <= 0 || decoded == nullptr || channels <= 0 || rate <= 0) {
            free(decoded);
            return false;
        }
        raw.assign((const uint8_t*)decoded,
            (const uint8_t*)decoded + (size_t)frames * (size_t)channels * sizeof(short));
        free(decoded);
        src.format = SDL_AUDIO_S16;
        src.channels = channels;
        src.freq = rate;
    } else if (ext == ".wav") {
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path.c_str(), &src, &buf, &len) || buf == nullptr) {
            return false;
        }
        raw.assign(buf, buf + len);
        SDL_free(buf);
    } else {
        return false;
    }
    const SDL_AudioSpec dst = {SDL_AUDIO_F32, 2, 32000};
    Uint8* conv = nullptr;
    int conv_len = 0;
    if (!SDL_ConvertAudioSamples(&src, raw.data(), (int)raw.size(), &dst, &conv, &conv_len) ||
        conv == nullptr)
    {
        return false;
    }
    out.assign((const float*)conv, (const float*)conv + conv_len / (int)sizeof(float));
    SDL_free(conv);
    return !out.empty();
}
}  // namespace

extern "C" bool pc_clip_play(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    const Clip* clip = nullptr;
    for (const Clip& c : s_clips) {
        if (c.path == path) {
            clip = &c;
            break;
        }
    }
    if (clip == nullptr) {
        Clip c;
        c.path = path;
        if (!decode_clip(c.path, c.pcm)) {
            SDL_Log("clip: cannot decode %s", path);
            return false;
        }
        s_clip_playing = nullptr; /* the vector may move */
        s_clips.push_back(std::move(c));
        clip = &s_clips.back();
    }
    s_clip_playing = clip;
    s_clip_pos = 0;
    return true;
}

extern "C" void pc_clip_mix(float* out, int num_samples) {
    if (out == nullptr || num_samples <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_clip_playing == nullptr) {
        return;
    }
    const float vol = pc_audio_get_sfx_volume();
    const std::vector<float>& pcm = s_clip_playing->pcm;
    size_t n = (size_t)num_samples * 2;
    for (size_t i = 0; i < n && s_clip_pos < pcm.size(); ++i) {
        out[i] += pcm[s_clip_pos++] * vol;
    }
    if (s_clip_pos >= pcm.size()) {
        s_clip_playing = nullptr;
    }
}

extern "C" void pc_music_stream_stop(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    stop_locked();
}

extern "C" void pc_music_stream_set_volume(float vol) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_stream_volume = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
}

extern "C" bool pc_music_stream_is_playing(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_playing;
}

extern "C" void pc_music_stream_mix(float* dst_left, float* dst_right, int num_samples) {
    if (!dst_left || num_samples <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_playing || !s_stream) {
        return;
    }

    pump_stream_locked();
    if (!s_loop && s_pcm_offset >= s_pcm_data.size() && SDL_GetAudioStreamAvailable(s_stream) <= 0)
    {
        stop_locked(); /* a play-once track has finished */
        return;
    }

    float mix_volume = s_stream_volume * pc_get_music_volume();
    if (mix_volume < 0.0f) {
        mix_volume = 0.0f;
    }

    constexpr int STACK_FLOATS = 1024;
    float stack_buf[STACK_FLOATS];
    std::vector<float> heap_buf;
    float* buf = stack_buf;
    int total_floats = num_samples * 2;
    if (total_floats > STACK_FLOATS) {
        heap_buf.resize((size_t)total_floats);
        buf = heap_buf.data();
    }

    int bytes_to_read = total_floats * (int)sizeof(float);
    int bytes_read = SDL_GetAudioStreamData(s_stream, buf, bytes_to_read);
    if (bytes_read < 0) {
        bytes_read = 0;
    }

    int floats_read = bytes_read / (int)sizeof(float);
    if (floats_read < total_floats) {
        std::fill(buf + floats_read, buf + total_floats, 0.0f);
    }

    if (mix_volume > 0.0f) {
        if (dst_right != nullptr) {
            for (int i = 0; i < num_samples; i++) {
                dst_left[i] += buf[i * 2] * mix_volume;
                dst_right[i] += buf[i * 2 + 1] * mix_volume;
            }
        } else {
            for (int i = 0; i < num_samples; i++) {
                dst_left[i * 2] += buf[i * 2] * mix_volume;
                dst_left[i * 2 + 1] += buf[i * 2 + 1] * mix_volume;
            }
        }
    }
}
