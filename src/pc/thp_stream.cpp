/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/thp_stream.hpp"
#include "pc/endian.hpp"
#include "pc/thp_jpeg.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace pc::thp {

static constexpr size_t THP_CONTAINER_HEADER_SIZE = 0x30;

THPStream::THPStream() = default;

THPStream::~THPStream() {
    close();
}

void THPStream::close() {
    if (m_file.is_open()) {
        m_file.close();
    }
    m_file.clear();
    m_open = false;
    m_eof = false;
    m_header = {};
    m_video_info = {};
    m_audio_info = {};
    m_has_video = false;
    m_has_audio = false;
    m_current_frame = 0;
    m_next_frame_offset = 0;
    m_current_packet_size = 0;
    m_packet_buffer.clear();
    m_left_adpcm = {};
    m_right_adpcm = {};
}

bool THPStream::open(const std::filesystem::path& path) {
    close();

    m_file.open(path, std::ios::binary);
    if (!m_file.is_open()) {
        return false;
    }

    if (!read_headers()) {
        close();
        return false;
    }

    m_packet_buffer.resize(m_header.max_buf_size > 0 ? m_header.max_buf_size : (2 * 1024 * 1024));
    m_open = true;
    return true;
}

bool THPStream::read_headers() {
    std::array<uint8_t, THP_CONTAINER_HEADER_SIZE> hdr_raw{};
    m_file.read(reinterpret_cast<char*>(hdr_raw.data()), THP_CONTAINER_HEADER_SIZE);
    if (m_file.gcount() != static_cast<std::streamsize>(THP_CONTAINER_HEADER_SIZE)) {
        return false;
    }

    if (std::memcmp(hdr_raw.data(), "THP\0", 4) != 0) {
        return false;
    }

    auto read_u32 = [](const uint8_t* ptr) -> uint32_t {
        be_val<uint32_t> v;
        std::memcpy(&v, ptr, 4);
        return v;
    };

    auto read_f32 = [](const uint8_t* ptr) -> float {
        be_val<float> v;
        std::memcpy(&v, ptr, 4);
        return v;
    };

    std::memcpy(m_header.magic, hdr_raw.data(), 4);
    m_header.version = read_u32(hdr_raw.data() + 0x04);
    m_header.max_buf_size = read_u32(hdr_raw.data() + 0x08);
    m_header.max_audio_samples = read_u32(hdr_raw.data() + 0x0C);
    m_header.fps = read_f32(hdr_raw.data() + 0x10);
    m_header.total_frames = read_u32(hdr_raw.data() + 0x14);
    m_header.first_frame_size = read_u32(hdr_raw.data() + 0x18);
    m_header.data_size = read_u32(hdr_raw.data() + 0x1C);
    m_header.comp_info_offset = read_u32(hdr_raw.data() + 0x20);
    m_header.offsets_offset = read_u32(hdr_raw.data() + 0x24);
    m_header.first_frame_offset = read_u32(hdr_raw.data() + 0x28);
    m_header.last_frame_offset = read_u32(hdr_raw.data() + 0x2C);

    if (m_header.fps <= 0.0f || m_header.fps > 240.0f) {
        m_header.fps = 29.97f;
    }

    // Seek to component info table
    m_file.seekg(m_header.comp_info_offset, std::ios::beg);
    if (!m_file) {
        return false;
    }

    std::array<uint8_t, 20> comp_info_header{};
    m_file.read(reinterpret_cast<char*>(comp_info_header.data()), comp_info_header.size());
    if (!m_file) {
        return false;
    }

    uint32_t num_components = read_u32(comp_info_header.data());
    const uint8_t* comp_types = comp_info_header.data() + 4;

    for (uint32_t i = 0; i < num_components && i < 16; ++i) {
        uint8_t type = comp_types[i];
        if (type == 0) {
            // Video component
            std::array<uint8_t, 12> vinfo{};
            m_file.read(reinterpret_cast<char*>(vinfo.data()), vinfo.size());
            if (!m_file)
                return false;
            m_video_info.width = read_u32(vinfo.data() + 0);
            m_video_info.height = read_u32(vinfo.data() + 4);
            m_video_info.video_format = read_u32(vinfo.data() + 8);
            m_has_video = true;
        } else if (type == 1) {
            // Audio component
            std::array<uint8_t, 16> ainfo{};
            m_file.read(reinterpret_cast<char*>(ainfo.data()), ainfo.size());
            if (!m_file)
                return false;
            m_audio_info.channels = read_u32(ainfo.data() + 0);
            m_audio_info.sample_rate = read_u32(ainfo.data() + 4);
            m_audio_info.num_samples = read_u32(ainfo.data() + 8);
            m_audio_info.num_tracks = read_u32(ainfo.data() + 12);
            m_has_audio = true;
        }
    }

    m_next_frame_offset = m_header.first_frame_offset;
    m_current_packet_size = m_header.first_frame_size;
    m_current_frame = 0;
    m_eof = false;
    return true;
}

bool THPStream::rewind() {
    if (!m_open) {
        return false;
    }
    m_next_frame_offset = m_header.first_frame_offset;
    m_current_packet_size = m_header.first_frame_size;
    m_current_frame = 0;
    m_eof = false;
    m_left_adpcm = {};
    m_right_adpcm = {};
    return true;
}

void THPStream::decode_adpcm_channel(THPAdpcmChannel& state, const uint8_t* block_data,
    uint32_t num_samples, std::vector<int16_t>& out_channel) {
    uint32_t samples_decoded = 0;
    size_t block_offset = 0;

    while (samples_decoded < num_samples) {
        const uint8_t* block = block_data + block_offset;
        uint8_t header = block[0];
        int scale_exp = header & 0x0F;
        int pred_idx = (header >> 4) & 0x07;
        int32_t scale = 1 << scale_exp;
        int32_t c1 = state.coef[pred_idx][0];
        int32_t c2 = state.coef[pred_idx][1];

        uint32_t samples_in_block = std::min<uint32_t>(14, num_samples - samples_decoded);
        for (uint32_t i = 0; i < samples_in_block; ++i) {
            int byte_idx = 1 + static_cast<int>(i / 2);
            int nibble = (i % 2 == 0) ? (block[byte_idx] >> 4) : (block[byte_idx] & 0x0F);
            if (nibble & 0x08) {
                nibble -= 0x10;
            }
            int32_t hist = (c1 * state.yn1 + c2 * state.yn2 + 1024) >> 11;
            int32_t val = scale * nibble + hist;
            val = std::clamp(val, -32768, 32767);
            state.yn2 = state.yn1;
            state.yn1 = static_cast<int16_t>(val);
            out_channel.push_back(static_cast<int16_t>(val));
            samples_decoded++;
        }
        block_offset += 8;
    }
}

bool THPStream::read_next_frame(std::vector<uint32_t>& out_rgba, std::vector<int16_t>& out_pcm) {
    if (!m_open || m_eof || m_current_packet_size == 0) {
        return false;
    }

    m_file.seekg(m_next_frame_offset, std::ios::beg);
    if (!m_file) {
        m_eof = true;
        return false;
    }

    if (m_packet_buffer.size() < m_current_packet_size) {
        m_packet_buffer.resize(m_current_packet_size);
    }

    m_file.read(reinterpret_cast<char*>(m_packet_buffer.data()), m_current_packet_size);
    if (m_file.gcount() != static_cast<std::streamsize>(m_current_packet_size)) {
        m_eof = true;
        return false;
    }

    const uint8_t* ptr = m_packet_buffer.data();
    auto read_u32 = [](const uint8_t* p) -> uint32_t {
        be_val<uint32_t> v;
        std::memcpy(&v, p, 4);
        return v;
    };
    auto read_s16 = [](const uint8_t* p) -> int16_t {
        be_val<int16_t> v;
        std::memcpy(&v, p, 2);
        return v;
    };

    uint32_t frame_hdr_size = m_has_audio ? 16 : 12;
    if (m_current_packet_size < frame_hdr_size) {
        return false;
    }

    uint32_t next_packet_size = read_u32(ptr + 0);
    uint32_t image_size = read_u32(ptr + 8);
    uint32_t audio_size = m_has_audio ? read_u32(ptr + 12) : 0;

    // Decode video
    if (image_size > 0 && frame_hdr_size + image_size <= m_current_packet_size) {
        std::span<const uint8_t> jpeg_span(ptr + frame_hdr_size, image_size);
        int w = 0, h = 0;
        if (!decode_frame_rgba(jpeg_span, w, h, out_rgba)) {
            return false;
        }
    }

    // Decode audio
    out_pcm.clear();
    if (m_has_audio && audio_size >= 0x50 &&
        frame_hdr_size + image_size + audio_size <= m_current_packet_size)
    {
        const uint8_t* aptr = ptr + frame_hdr_size + image_size;
        uint32_t channel_stride = read_u32(aptr + 0);
        uint32_t sample_count = read_u32(aptr + 4);

        // Load coefficients
        for (int i = 0; i < 8; ++i) {
            m_left_adpcm.coef[i][0] = read_s16(aptr + 0x08 + i * 4 + 0);
            m_left_adpcm.coef[i][1] = read_s16(aptr + 0x08 + i * 4 + 2);
            if (m_audio_info.channels > 1) {
                m_right_adpcm.coef[i][0] = read_s16(aptr + 0x28 + i * 4 + 0);
                m_right_adpcm.coef[i][1] = read_s16(aptr + 0x28 + i * 4 + 2);
            }
        }
        m_left_adpcm.yn1 = read_s16(aptr + 0x48);
        m_left_adpcm.yn2 = read_s16(aptr + 0x4A);
        if (m_audio_info.channels > 1) {
            m_right_adpcm.yn1 = read_s16(aptr + 0x4C);
            m_right_adpcm.yn2 = read_s16(aptr + 0x4E);
        }

        const uint8_t* audio_payload = aptr + 0x50;
        std::vector<int16_t> left_samples;
        left_samples.reserve(sample_count);
        decode_adpcm_channel(m_left_adpcm, audio_payload, sample_count, left_samples);

        if (m_audio_info.channels > 1) {
            std::vector<int16_t> right_samples;
            right_samples.reserve(sample_count);
            decode_adpcm_channel(
                m_right_adpcm, audio_payload + channel_stride, sample_count, right_samples);

            out_pcm.resize(sample_count * 2);
            for (size_t i = 0; i < sample_count; ++i) {
                out_pcm[i * 2 + 0] = left_samples[i];
                out_pcm[i * 2 + 1] = (i < right_samples.size()) ? right_samples[i] : 0;
            }
        } else {
            out_pcm = std::move(left_samples);
        }
    }

    m_next_frame_offset += m_current_packet_size;
    m_current_packet_size = next_packet_size;
    m_current_frame++;

    if (m_current_frame >= m_header.total_frames || m_current_packet_size == 0) {
        m_eof = true;
    }

    return true;
}

}  // namespace pc::thp
