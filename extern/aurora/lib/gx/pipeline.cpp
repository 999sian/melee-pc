#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/resource_cache.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <cstdlib>

#include <tracy/Tracy.hpp>

namespace aurora::gx {

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout) {
  ZoneScoped;
  const auto shader = build_shader(config.shaderConfig, layout);
  const auto label = fmt::format("GX Pipeline {:x}",
                                 xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX))));
  return build_pipeline(config, layout, {}, shader, label.c_str());
}

// Diagnostics for the untextured-white-quad artifact in melee-pc:
//   AURORA_SKIP_UNTEX=1      drop every draw that binds no texture
//   AURORA_SKIP_UNTEX_VTX=n  drop only untextured draws with n vertices
//   AURORA_LOG_UNTEX=1       report untextured draws, with the breadcrumb
// A draw with no texture bind group falls back to flat colour, which is what
// a solid white quad looks like on screen. aurora_draw_tag lets the game
// record which subsystem is currently rendering, so an offending draw can be
// traced back past the GX boundary.
extern "C" {
unsigned int aurora_draw_tag = 0;
}

static bool env_flag(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != '\0' && *v != '0';
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
    /* AURORA_LOG_SKIPPED=1: report draws dropped because their pipeline is
     * still compiling. Pipelines are built asynchronously and an unready one
     * silently skips its draw, so a short-lived screen can miss its geometry
     * entirely on a cold cache. */
    if (env_flag("AURORA_LOG_SKIPPED")) {
      static u32 count = 0;
      if (++count <= 4000) {
        std::fprintf(stderr, "[WARN] aurora::gx: skipped draw: pipeline not ready (vtx %u, tag %u)\n",
                     static_cast<unsigned>(data.vtxCount), aurora_draw_tag);
      }
    }
    return;
  }

  // AURORA_ONLY_UNTEX_VTX=<n>: render ONLY untextured draws with n vertices
  // and drop everything else, so the offending geometry can be seen in
  // isolation on a black frame. Suppressing a draw proves it is responsible;
  // isolating it shows WHAT it is.
  {
    static const long onlyN = [] {
      const char* v = std::getenv("AURORA_ONLY_UNTEX_VTX");
      return v != nullptr ? std::strtol(v, nullptr, 10) : 0L;
    }();
    if (onlyN != 0 &&
        !(!data.bindGroups.textureBindGroup && static_cast<long>(data.vtxCount) == onlyN)) {
      return;
    }
  }

  if (!data.bindGroups.textureBindGroup) {
    static const bool skip = env_flag("AURORA_SKIP_UNTEX");
    static const bool log = env_flag("AURORA_LOG_UNTEX");
    // AURORA_SKIP_UNTEX_VTX=<n>: only drop untextured draws with exactly n
    // vertices, so a suspect quad can be removed without also removing the
    // legitimate untextured geometry that shares this path.
    static const long onlyVtx = [] {
      const char* v = std::getenv("AURORA_SKIP_UNTEX_VTX");
      return v != nullptr ? std::strtol(v, nullptr, 10) : 0L;
    }();
    // Untextured quads are the shape the artifact takes, so report those
    // individually and the rest only sparsely. The per-vertex byte stride
    // discriminates a 2D sprite quad (XY + ST floats = 16B) from 3D geometry.
    if (log) {
      static uint64_t n = 0;
      const bool quad = data.vtxCount == 4;
      if (quad || (n % 2000) == 0) {
        const uint32_t stride = data.vtxCount != 0 ? data.vertRange.size / data.vtxCount : 0;
        fmt::print(stderr, "untex draw #{}{}: idx={} vtx={} stride={} tag={}\n", n, quad ? " QUAD" : "",
                   data.indexCount, data.vtxCount, stride, data.tag);
      }
      ++n;
    }
    if (onlyVtx != 0) {
      if (static_cast<long>(data.vtxCount) == onlyVtx) {
        return;
      }
    } else if (skip) {
      return;
    }
  }

  const auto& resources = gfx::detail::resources();
#ifdef __EMSCRIPTEN__
  const std::array immediateOffsets{data.immediateRange.offset};
  pass.SetBindGroup(3, resources.uniformBindGroup, immediateOffsets.size(), immediateOffsets.data());
#else
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
#endif
  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
}

} // namespace aurora::gx
