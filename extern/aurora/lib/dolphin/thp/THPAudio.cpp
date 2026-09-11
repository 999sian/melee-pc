#include "dolphin/thp.h"

#include "../../internal.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace {
constexpr size_t kHeaderSize = 80;

struct AudioHeader {
  u32 nextChannelOffset = 0;
  u32 sampleCount = 0;
  std::array<std::array<s16, 2>, 8> leftCoeffs{};
  std::array<std::array<s16, 2>, 8> rightCoeffs{};
  s16 leftYn1 = 0;
  s16 leftYn2 = 0;
  s16 rightYn1 = 0;
  s16 rightYn2 = 0;
};

AudioHeader read_header(const u8* frame) noexcept {
  AudioHeader header{};
  header.nextChannelOffset = read_bits<u32>(frame);
  header.sampleCount = read_bits<u32>(frame + 4);
  for (size_t predictor = 0; predictor < 8; ++predictor) {
    for (size_t coefficient = 0; coefficient < 2; ++coefficient) {
      const size_t index = predictor * 2 + coefficient;
      header.leftCoeffs[predictor][coefficient] = read_bits<s16>(frame + 8 + index * 2);
      header.rightCoeffs[predictor][coefficient] = read_bits<s16>(frame + 40 + index * 2);
    }
  }
  header.leftYn1 = read_bits<s16>(frame + 72);
  header.leftYn2 = read_bits<s16>(frame + 74);
  header.rightYn1 = read_bits<s16>(frame + 76);
  header.rightYn2 = read_bits<s16>(frame + 78);
  return header;
}

class NibbleReader {
public:
  explicit NibbleReader(const u8* data) noexcept
  : mData{data}
  , mCursor{1}
  , mNibbleOffset{2}
  , mPredictor{static_cast<u8>((data[0] >> 4) & 7)}
  , mScale{static_cast<u8>(data[0] & 15)} {}

  s32 next() noexcept {
    if ((mNibbleOffset & 15) == 0) {
      const u8 header = mData[mCursor++];
      mPredictor = static_cast<u8>((header >> 4) & 7);
      mScale = static_cast<u8>(header & 15);
      mNibbleOffset += 2;
    }

    u8 nibble = 0;
    if ((mNibbleOffset & 1) != 0) {
      nibble = static_cast<u8>(mData[mCursor++] & 15);
    } else {
      nibble = static_cast<u8>(mData[mCursor] >> 4);
    }
    ++mNibbleOffset;
    return nibble >= 8 ? static_cast<s32>(nibble) - 16 : nibble;
  }

  u8 predictor() const noexcept { return mPredictor; }
  u8 scale() const noexcept { return mScale; }

private:
  const u8* mData;
  size_t mCursor;
  u32 mNibbleOffset;
  u8 mPredictor;
  u8 mScale;
};

void decode_channel(s16* output, size_t stride, const u8* encoded, u32 sampleCount,
                    const std::array<std::array<s16, 2>, 8>& coeffs, s16 yn1, s16 yn2) noexcept {
  NibbleReader reader{encoded};
  for (u32 i = 0; i < sampleCount; ++i) {
    const s64 sample = reader.next();
    const auto& coeff = coeffs[reader.predictor()];
    const s64 acc = static_cast<s64>(coeff[1]) * yn2 + static_cast<s64>(coeff[0]) * yn1 +
                    sample * (s64{1} << reader.scale()) * 2048;
    const s64 rounded = std::clamp(acc * 32 + 0x8000, static_cast<s64>(std::numeric_limits<s32>::min()),
                                   static_cast<s64>(std::numeric_limits<s32>::max()));
    const auto decoded = static_cast<s16>(static_cast<s32>(rounded) >> 16);
    output[static_cast<size_t>(i) * stride] = decoded;
    yn2 = yn1;
    yn1 = decoded;
  }
}
} // namespace

extern "C" {
u32 THPAudioDecode(s16* output, const u8* frame, s32 flag) {
  if (output == nullptr || frame == nullptr) {
    return 0;
  }

  const AudioHeader header = read_header(frame);
  const size_t stride = flag == 1 ? 1 : 2;
  s16* rightOutput = output;
  s16* leftOutput = flag == 1 ? output + header.sampleCount : output + 1;
  const u8* leftData = frame + kHeaderSize;

  decode_channel(leftOutput, stride, leftData, header.sampleCount, header.leftCoeffs, header.leftYn1, header.leftYn2);
  if (header.nextChannelOffset == 0) {
    for (u32 i = 0; i < header.sampleCount; ++i) {
      rightOutput[static_cast<size_t>(i) * stride] = leftOutput[static_cast<size_t>(i) * stride];
    }
  } else {
    decode_channel(rightOutput, stride, leftData + header.nextChannelOffset, header.sampleCount, header.rightCoeffs,
                   header.rightYn1, header.rightYn2);
  }
  return header.sampleCount;
}
}
