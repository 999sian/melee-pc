/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/thp_jpeg.hpp"

#include <algorithm>
#include <cstring>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "pc/stb_image.h"

namespace pc::thp {

bool reinflate_thp_jpeg(std::span<const uint8_t> src, std::vector<uint8_t>& dst) {
    if (src.size() < 4) {
        return false;
    }
    // Verify SOI (Start of Image 0xFF 0xD8)
    if (src[0] != 0xFF || src[1] != 0xD8) {
        return false;
    }

    // Find SOS (Start of Scan 0xFF 0xDA)
    size_t sos_idx = 0;
    bool found_sos = false;
    for (size_t i = 0; i + 1 < src.size(); ++i) {
        if (src[i] == 0xFF && src[i + 1] == 0xDA) {
            sos_idx = i;
            found_sos = true;
            break;
        }
    }

    if (!found_sos || sos_idx + 4 > src.size()) {
        dst.assign(src.begin(), src.end());
        return true;
    }

    // Read SOS header length (big-endian 16-bit)
    uint16_t sos_len = (static_cast<uint16_t>(src[sos_idx + 2]) << 8) | src[sos_idx + 3];
    size_t entropy_start = sos_idx + 2 + sos_len;
    if (entropy_start >= src.size()) {
        dst.assign(src.begin(), src.end());
        return true;
    }

    // Find optional existing EOI (0xFF 0xD9)
    size_t entropy_end = src.size();
    if (src.size() >= 2 && src[src.size() - 2] == 0xFF && src[src.size() - 1] == 0xD9) {
        entropy_end = src.size() - 2;
    }

    dst.clear();
    dst.reserve(src.size() + 256);
    // Copy headers up to entropy scan unchanged
    dst.insert(dst.end(), src.begin(), src.begin() + entropy_start);

    // Reinflate entropy data: insert 0x00 after 0xFF unless it's a marker or already stuffed
    for (size_t i = entropy_start; i < entropy_end; ++i) {
        uint8_t b = src[i];
        dst.push_back(b);
        if (b == 0xFF) {
            if (i + 1 < entropy_end) {
                uint8_t next_b = src[i + 1];
                if (next_b == 0x00) {
                    // Already stuffed
                    dst.push_back(0x00);
                    ++i;
                } else if (next_b >= 0xD0 && next_b <= 0xD7) {
                    // RST marker (0xFFD0 - 0xFFD7)
                    dst.push_back(next_b);
                    ++i;
                } else if (next_b == 0xD9) {
                    // Premature EOI
                    break;
                } else {
                    // Missing byte stuffing: insert 0x00
                    dst.push_back(0x00);
                }
            } else {
                // Trailing 0xFF before end of scan
                dst.push_back(0x00);
            }
        }
    }

    // Ensure EOI marker at end
    dst.push_back(0xFF);
    dst.push_back(0xD9);
    return true;
}

bool decode_frame_rgba(std::span<const uint8_t> src, int& out_width, int& out_height,
    std::vector<uint32_t>& out_rgba) {
    std::vector<uint8_t> reinflated;
    if (!reinflate_thp_jpeg(src, reinflated)) {
        return false;
    }

    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load_from_memory(
        reinflated.data(), static_cast<int>(reinflated.size()), &w, &h, &channels, 4);
    if (!pixels) {
        return false;
    }

    out_width = w;
    out_height = h;
    const size_t num_pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    out_rgba.resize(num_pixels);
    std::memcpy(out_rgba.data(), pixels, num_pixels * sizeof(uint32_t));
    stbi_image_free(pixels);
    return true;
}

}  // namespace pc::thp
