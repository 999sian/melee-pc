#pragma once

#include "types.hpp"

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = true;
/* Melee's busiest Classic frames land right on 24 MiB of per-draw uniforms
 * and occasionally tip over it (observed: 25167920 requested). These pools
 * are non-owning views onto the mapped staging buffer, so overflowing is a
 * hard abort, not a realloc. MELEE_GFX_STATS=1 prints the peak each second
 * if this needs sizing again. */
#if defined(__ANDROID__)
// Mobile optimization: clamp GPU buffer pools to conserve memory on 2-3GB mobile devices
// while retaining sufficient headroom for Melee draw calls.
inline constexpr uint64_t UniformBufferSize = 25165824; // 24 MiB (ample for Melee peak 24MB, instead of 48MB)
inline constexpr uint64_t VertexBufferSize = 4194304;   // 4 MiB
inline constexpr uint64_t IndexBufferSize = 1048576;    // 1 MiB
inline constexpr uint64_t StorageBufferSize = 4194304;  // 4 MiB
inline constexpr uint64_t TextureUploadSize = 12582912; // 12 MiB
#else
inline constexpr uint64_t UniformBufferSize = 50331648; // 48 MiB
inline constexpr uint64_t VertexBufferSize = 5242880;   // 5 MiB
inline constexpr uint64_t IndexBufferSize = 2097152;    // 2 MiB
inline constexpr uint64_t StorageBufferSize = 8388608;  // 8 MiB
inline constexpr uint64_t TextureUploadSize = 25165824; // 24 MiB
#endif

namespace detail {
struct Resources {
  wgpu::Buffer vertexBuffer;
  wgpu::Buffer uniformBuffer;
  wgpu::Buffer indexBuffer;
  wgpu::Buffer storageBuffer;
  wgpu::BindGroupLayout staticBindGroupLayout;
  wgpu::BindGroup staticBindGroup;
  wgpu::BindGroupLayout uniformBindGroupLayout;
  wgpu::BindGroup uniformBindGroup;
  wgpu::Limits limits;
  AuroraStats stats{};
};

Resources& resources() noexcept;
} // namespace detail
} // namespace aurora::gfx
