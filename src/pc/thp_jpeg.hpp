/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace pc::thp {

/**
 * Reinflates GameCube THP Motion-JPEG scan entropy by inserting standard
 * 0x00 byte-stuffing after 0xFF entropy bytes.
 */
bool reinflate_thp_jpeg(std::span<const uint8_t> src, std::vector<uint8_t>& dst);

/**
 * Decodes a GameCube THP Motion-JPEG frame directly to RGBA8888 pixels.
 *
 * @param src Raw THP video frame data
 * @param out_width Decoded image width
 * @param out_height Decoded image height
 * @param out_rgba Output buffer receiving 32-bit packed RGBA pixels
 * @return true on successful decode
 */
bool decode_frame_rgba(
    std::span<const uint8_t> src, int& out_width, int& out_height, std::vector<uint32_t>& out_rgba);

}  // namespace pc::thp
