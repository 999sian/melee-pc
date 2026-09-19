/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "pc/endian.hpp"
#include "pc/thp_jpeg.hpp"
#include "pc/thp_stream.hpp"

static void test_jpeg_reinflation() {
    // Construct a synthetic JPEG buffer with:
    // SOI (FF D8)
    // APP0 marker (FF E0 00 04 11 22)
    // SOS marker (FF DA 00 03 AA) -> header length is 3 (bytes: 00 03 AA)
    // Entropy data with unstuffed 0xFF bytes: [10, 20, FF, 30, 40, FF, 50]
    // EOI (FF D9)
    std::vector<uint8_t> input = {
        0xFF, 0xD8,                                // SOI
        0xFF, 0xE0, 0x00, 0x04, 0x11, 0x22,        // APP0
        0xFF, 0xDA, 0x00, 0x03, 0xAA,              // SOS (len=3)
        0x10, 0x20, 0xFF, 0x30, 0x40, 0xFF, 0x50,  // Entropy scan with unstuffed FF
        0xFF, 0xD9                                 // EOI
    };

    std::vector<uint8_t> reinflated;
    bool ok = pc::thp::reinflate_thp_jpeg(input, reinflated);
    assert(ok);

    // Expected:
    // SOI, APP0, SOS headers preserved
    // Entropy: 10, 20, FF, 00, 30, 40, FF, 00, 50
    // EOI: FF, D9
    assert(reinflated[0] == 0xFF && reinflated[1] == 0xD8);
    assert(reinflated[2] == 0xFF && reinflated[3] == 0xE0);
    assert(reinflated[8] == 0xFF && reinflated[9] == 0xDA);

    // Look for stuffed 0xFF 0x00 in entropy scan
    bool found_stuffing = false;
    for (size_t i = 13; i + 1 < reinflated.size(); ++i) {
        if (reinflated[i] == 0xFF && reinflated[i + 1] == 0x00) {
            found_stuffing = true;
            break;
        }
    }
    assert(found_stuffing);
    assert(reinflated.back() == 0xD9);
    assert(reinflated[reinflated.size() - 2] == 0xFF);

    printf("[PASS] test_jpeg_reinflation\n");
}

static void test_adpcm_decoding() {
    pc::thp::THPAdpcmChannel state{};
    // Coefficients (c1=2048 (1.0 in 11-bit fixed point), c2=0)
    state.coef[0][0] = 2048;
    state.coef[0][1] = 0;
    state.yn1 = 0;
    state.yn2 = 0;

    // 1 block of 8 bytes
    // Header byte: pred_idx = 0, scale_exp = 1 (scale = 2)
    // 14 nibbles: +1, -1, +2, -2, +3, -3, +4, -4, +5, -5, +6, -6, +7, -7
    uint8_t block[8] = {
        0x01,  // byte 0: pred=0, scale=1 (scale = 2)
        0x1F,  // nibbles: +1 (1), -1 (F)
        0x2E,  // nibbles: +2 (2), -2 (E)
        0x3D,  // nibbles: +3 (3), -3 (D)
        0x4C,  // nibbles: +4 (4), -4 (C)
        0x5B,  // nibbles: +5 (5), -5 (B)
        0x6A,  // nibbles: +6 (6), -6 (A)
        0x79   // nibbles: +7 (7), -7 (9)
    };

    std::vector<int16_t> out_pcm;
    pc::thp::THPStream::decode_adpcm_channel(state, block, 14, out_pcm);
    assert(out_pcm.size() == 14);

    // Sample 0: scale=2 * +1 + (2048 * 0 + 1024) >> 11 = 2 + 0 = 2
    assert(out_pcm[0] == 2);
    // yn1 becomes 2
    // Sample 1: scale=2 * -1 + (2048 * 2 + 1024) >> 11 = -2 + (5120 >> 11) = -2 + 2 = 0
    assert(out_pcm[1] == 0);

    printf("[PASS] test_adpcm_decoding\n");
}

static void test_container_parsing() {
    namespace fs = std::filesystem;
    const fs::path test_thp_path = "test_sample.thp";

    // Build a minimal valid THP file
    std::vector<uint8_t> thp_bytes(0x30 + 0x20 + 0x20 + 0x40, 0);

    auto write_be32 = [](uint8_t* p, uint32_t val) {
        be_val<uint32_t> be = val;
        std::memcpy(p, &be, 4);
    };
    auto write_bef32 = [](uint8_t* p, float val) {
        be_val<float> be = val;
        std::memcpy(p, &be, 4);
    };

    // 0x00: Magic "THP\0"
    std::memcpy(thp_bytes.data() + 0x00, "THP\0", 4);
    write_be32(thp_bytes.data() + 0x04, 0x00011000);  // Version
    write_be32(thp_bytes.data() + 0x08, 1024);        // Max buffer size
    write_be32(thp_bytes.data() + 0x0C, 100);         // Max audio samples
    write_bef32(thp_bytes.data() + 0x10, 60.0f);      // FPS
    write_be32(thp_bytes.data() + 0x14, 1);           // Num frames = 1
    write_be32(thp_bytes.data() + 0x18, 16);          // First frame size
    write_be32(thp_bytes.data() + 0x1C, 256);         // Data size
    write_be32(thp_bytes.data() + 0x20, 0x30);        // Comp info offset = 0x30
    write_be32(thp_bytes.data() + 0x24, 0x60);        // Offsets offset
    write_be32(thp_bytes.data() + 0x28, 0x70);        // First frame offset = 0x70
    write_be32(thp_bytes.data() + 0x2C, 0x70);        // Last frame offset

    // Component info at 0x30:
    write_be32(thp_bytes.data() + 0x30, 1);  // 1 component
    thp_bytes[0x34] = 0;                     // Component 0 is Video
    // Video descriptor at 0x30 + 20 = 0x44:
    write_be32(thp_bytes.data() + 0x44, 640);  // Width = 640
    write_be32(thp_bytes.data() + 0x48, 480);  // Height = 480
    write_be32(thp_bytes.data() + 0x4C, 0);    // Format = 0

    // Write to file
    {
        std::ofstream f(test_thp_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(thp_bytes.data()), thp_bytes.size());
    }

    pc::thp::THPStream stream;
    bool opened = stream.open(test_thp_path);
    assert(opened);
    assert(stream.is_open());
    assert(stream.width() == 640);
    assert(stream.height() == 480);
    assert(stream.fps() == 60.0f);
    assert(stream.total_frames() == 1);
    assert(!stream.has_audio());

    stream.close();
    fs::remove(test_thp_path);

    printf("[PASS] test_container_parsing\n");
}

int main() {
    test_jpeg_reinflation();
    test_adpcm_decoding();
    test_container_parsing();
    printf("All THP tests passed successfully!\n");
    return 0;
}
