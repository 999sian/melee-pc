/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace pc::thp {

struct THPHeader {
    char magic[4];
    uint32_t version;
    uint32_t max_buf_size;
    uint32_t max_audio_samples;
    float fps;
    uint32_t total_frames;
    uint32_t first_frame_size;
    uint32_t data_size;
    uint32_t comp_info_offset;
    uint32_t offsets_offset;
    uint32_t first_frame_offset;
    uint32_t last_frame_offset;
};

struct THPVideoInfo {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t video_format{0};
};

struct THPAudioInfo {
    uint32_t channels{0};
    uint32_t sample_rate{0};
    uint32_t num_samples{0};
    uint32_t num_tracks{0};
};

struct THPAdpcmChannel {
    int16_t coef[8][2]{};
    int16_t yn1{0};
    int16_t yn2{0};
};

class THPStream {
public:
    THPStream();
    ~THPStream();

    THPStream(const THPStream&) = delete;
    THPStream& operator=(const THPStream&) = delete;
    THPStream(THPStream&&) noexcept = default;
    THPStream& operator=(THPStream&&) noexcept = default;

    bool open(const std::filesystem::path& path);
    void close();

    bool is_open() const { return m_open; }
    bool is_eof() const { return m_eof; }

    uint32_t width() const { return m_video_info.width; }
    uint32_t height() const { return m_video_info.height; }
    float fps() const { return m_header.fps; }
    uint32_t total_frames() const { return m_header.total_frames; }
    uint32_t current_frame() const { return m_current_frame; }
    bool has_audio() const { return m_has_audio; }
    uint32_t audio_channels() const { return m_audio_info.channels; }
    uint32_t audio_sample_rate() const { return m_audio_info.sample_rate; }

    /**
     * Reads and decodes the next video frame and audio packet.
     * @param out_rgba Receives decoded 32-bit packed RGBA pixels.
     * @param out_pcm Receives decoded interleaved 16-bit PCM audio samples.
     * @return true if frame was read and decoded successfully.
     */
    bool read_next_frame(std::vector<uint32_t>& out_rgba, std::vector<int16_t>& out_pcm);

    /**
     * Rewinds stream back to the first frame.
     */
    bool rewind();

    /**
     * Decodes a single GameCube DSP ADPCM channel buffer.
     */
    static void decode_adpcm_channel(THPAdpcmChannel& state, const uint8_t* block_data,
        uint32_t num_samples, std::vector<int16_t>& out_channel);

private:
    bool read_headers();

    std::ifstream m_file;
    bool m_open{false};
    bool m_eof{false};

    THPHeader m_header{};
    THPVideoInfo m_video_info{};
    THPAudioInfo m_audio_info{};
    bool m_has_video{false};
    bool m_has_audio{false};

    uint32_t m_current_frame{0};
    uint64_t m_next_frame_offset{0};
    uint32_t m_current_packet_size{0};

    std::vector<uint8_t> m_packet_buffer;
    THPAdpcmChannel m_left_adpcm{};
    THPAdpcmChannel m_right_adpcm{};
};

}  // namespace pc::thp
