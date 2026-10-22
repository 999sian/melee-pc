#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/texture.hpp"
#include "../../gfx/recording.hpp"
#include "../../window.hpp"
#include "../../webgpu/gpu.hpp"
#include "../../gx/texture.hpp"
#include "../vi/vi_internal.hpp"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
aurora::Vec2<uint32_t> scale_copy_dst(u32 logicalWidth, u32 logicalHeight) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {logicalWidth, logicalHeight};
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {logicalWidth, logicalHeight};
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto scaledWidth = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalWidth) * scaleX)), 1);
  const auto scaledHeight = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalHeight) * scaleY)), 1);
  return {scaledWidth, scaledHeight};
}

// Source rect of an EFB copy, in render-target pixels.
//
// The scissor mapping rounds a rect outwards so nothing drawn is clipped. A
// copy needs the opposite: at a fractional render scale the outward-rounded
// edge lies past the region the pass rendered (256 logical pixels at 1024x768
// is 409.6, and column 409 is only partly covered), so the copy picks up an
// unwritten black fringe. Projected textures - fighter shadows above all -
// clamp to that fringe for every coordinate outside the image, which turns
// whole stage floors black. Round inwards, and size the destination from the
// same rect so a 1:1 copy stays 1:1 instead of being resampled.
aurora::gfx::ClipRect map_copy_src(const aurora::gfx::ClipRect& logicalRect) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalRect;
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalRect;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto inward = [](float v, int32_t limit) {
    return std::clamp(static_cast<int32_t>(std::floor(v)), 0, limit);
  };
  const auto left = inward(static_cast<float>(logicalRect.x) * scaleX, static_cast<int32_t>(targetWidth));
  const auto top = inward(static_cast<float>(logicalRect.y) * scaleY, static_cast<int32_t>(targetHeight));
  const auto right = std::max(
      inward(static_cast<float>(logicalRect.x + logicalRect.width) * scaleX, static_cast<int32_t>(targetWidth)),
      left + 1);
  const auto bottom = std::max(
      inward(static_cast<float>(logicalRect.y + logicalRect.height) * scaleY, static_cast<int32_t>(targetHeight)),
      top + 1);
  return {.x = left, .y = top, .width = right - left, .height = bottom - top};
}
} // namespace

namespace aurora::gx {
namespace {
// Copies only exist on the GPU, so the texture cache keys them by destination
// pointer. Nothing tells it when the game frees that memory, and a model file
// loaded there later would keep sampling the old copy. On hardware the copy
// writes its pixels to the destination, so writing a stamp there instead is
// safe, and a texture load that finds the stamp gone knows the memory was
// reused. Jigglypuff's eyes on Big Blue were drawn from a freed shadow buffer.
constexpr u64 CopyStampMagic = 0xA0B0'C0D0'0000'0000ull;
absl::flat_hash_map<const void*, u64> s_copyStamps;
u64 s_nextCopyStamp = 0;
} // namespace

void stamp_copy_dest(void* dest) noexcept {
  if (dest == nullptr) {
    return;
  }
  const u64 stamp = CopyStampMagic | ++s_nextCopyStamp;
  std::memcpy(dest, &stamp, sizeof(stamp));
  s_copyStamps[dest] = stamp;
}

void retire_overwritten_copy_dest(const void* data) noexcept {
  if (data == nullptr || s_copyStamps.empty()) {
    return;
  }
  const auto it = s_copyStamps.find(data);
  if (it == s_copyStamps.end()) {
    return;
  }
  if (std::memcmp(data, &it->second, sizeof(it->second)) != 0) {
    GXDestroyCopyTex(const_cast<void*>(data));
  }
}

void forget_copy_dest(const void* dest) noexcept { s_copyStamps.erase(dest); }

void copy_tex(const void* dest, GXBool clear) noexcept {
  const auto rect = map_copy_src(g_gxState.texCopySrc);
  auto [dstWidth, dstHeight] = scale_copy_dst(g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight);
  if (g_gxState.texCopyDstWidth == g_gxState.texCopySrc.width &&
      g_gxState.texCopyDstHeight == g_gxState.texCopySrc.height) {
    dstWidth = static_cast<u32>(rect.width);
    dstHeight = static_cast<u32>(rect.height);
  }
  const auto texCopyFmt = g_gxState.texCopyFmt;
  const auto copy = texture::resolve_copy_texture({
      .dest = dest,
      .width = dstWidth,
      .height = dstHeight,
      .format = texCopyFmt,
  });

  const auto clearColor = clear && g_gxState.colorUpdate;
  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
  const auto clearDepth = clear && g_gxState.depthUpdate;
  gfx::resolve_pass_into(copy.handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,
                         clear_depth_value(), texCopyFmt, g_gxState.pixelFmt);
}
} // namespace aurora::gx

extern "C" {
GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};
GXRenderModeObj GXPal528IntDf = {
    VI_TVMODE_PAL_INT,
    704,
    528,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    VI_TVMODE_PAL_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};

void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
  const GXRenderModeObj in = *rmin;
  const u16 hor2 = hor * 2;
  const u16 ver2 = ver * 2;
  const u32 efbHeight = in.efbHeight;

  *rmout = in;
  rmout->fbWidth = in.fbWidth - hor2;
  rmout->efbHeight = efbHeight - (ver2 * efbHeight) / in.xfbHeight;

  if (in.xFBmode == VI_XFBMODE_SF && (in.viTVmode & 2) != 2) {
    rmout->xfbHeight = in.xfbHeight - ver;
  } else {
    rmout->xfbHeight = in.xfbHeight - ver2;
  }

  rmout->viWidth = in.viWidth - hor2;
  rmout->viHeight = in.viHeight - ver2;
  rmout->viXOrigin = in.viXOrigin + hor;
  rmout->viYOrigin = in.viYOrigin + ver;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_SRC);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetDispCopyDst(u16 wd, u16 ht) {}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DST);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
  GX_WRITE_U32(fmt);
  GX_WRITE_U8(mipmap != GX_FALSE);
}

// TODO GXSetDispCopyFrame2Field
// TODO GXSetCopyClamp

u32 GXSetDispCopyYScale(f32 vscale) { return 0; }

void GXSetCopyClear(GXColor color, u32 depth) {
  // BP 0x4F: clear color R + A
  u32 reg0 = 0;
  SET_REG_FIELD(0, reg0, 8, 0, color.r);
  SET_REG_FIELD(0, reg0, 8, 8, color.a);
  SET_REG_FIELD(0, reg0, 8, 24, 0x4F);
  GX_WRITE_RAS_REG(reg0);

  // BP 0x50: clear color B + G
  u32 reg1 = 0;
  SET_REG_FIELD(0, reg1, 8, 0, color.b);
  SET_REG_FIELD(0, reg1, 8, 8, color.g);
  SET_REG_FIELD(0, reg1, 8, 24, 0x50);
  GX_WRITE_RAS_REG(reg1);

  // BP 0x51: clear Z (24-bit)
  u32 reg2 = 0;
  SET_REG_FIELD(0, reg2, 24, 0, depth);
  SET_REG_FIELD(0, reg2, 8, 24, 0x51);
  GX_WRITE_RAS_REG(reg2);
  __gx->bpSent = 1;
}

void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {}

void GXSetDispCopyGamma(GXGamma gamma) {}

void GXCopyDisp(void* dest, GXBool clear) {}

void GXCopyTex(void* dest, GXBool clear) {
  aurora::gx::stamp_copy_dest(dest);
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DEST);
  GX_WRITE_U64(reinterpret_cast<u64>(dest));

  SET_REG_FIELD(0, __gx->cpTex, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, __gx->cpTex, 1, 14, 0);
  SET_REG_FIELD(0, __gx->cpTex, 8, 24, 0x52);
  GX_WRITE_RAS_REG(__gx->cpTex);
  aurora::gx::fifo::publish();
}

// TODO GXGetYScaleFactor
// TODO GXGetNumXfbLines
// TODO GXClearBoundingBox
// TODO GXReadBoundingBox
}
