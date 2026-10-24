#pragma once

#include "pipeline.hpp"
#include <cstdlib>

namespace aurora::gx {
// shortcut: 1–4 ADD/SUB stages only; expand after broader GPU and visual measurements.
constexpr u32 MaxUberStages = 4;
struct alignas(16) UberStageUniform {
  std::array<u32, 4> colorArgs;
  std::array<u32, 4> alphaArgs;
  std::array<u32, 4> colorOp;
  std::array<u32, 4> alphaOp;
  std::array<u32, 4> konst;
};
struct alignas(16) UberUniform {
  std::array<UberStageUniform, MaxUberStages> stages;
  std::array<std::array<u32, 4>, 2> alpha;
};
static_assert(sizeof(UberStageUniform) == 80 && sizeof(UberUniform) == 352);

inline bool hybrid_ubershaders_enabled() {
#ifdef __EMSCRIPTEN__
  return false;
#else
  // Desktop permits the opt-in for image comparisons against Android.
  static const bool enabled = [] {
    const auto* value = std::getenv("MELEE_ANDROID_UBERSHADERS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
#endif
}

inline bool make_uber_pipeline_config(const PipelineConfig& specialized, PipelineConfig& family) {
  const auto& shader = specialized.shaderConfig;
  if (shader.uberTev || shader.tevStageCount == 0 || shader.tevStageCount > MaxUberStages ||
      shader.numIndStages != 0 || shader.zTexOp != GX_ZT_DISABLE)
    return false;
  for (const auto& tcg : shader.tcgs)
    if (tcg.postMtx != GX_PTIDENTITY || (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7)) return false;
  for (u32 i = 0; i < shader.tevStageCount; ++i) {
    const auto& stage = shader.tevStages[i];
    if (stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF ||
        stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev ||
        stage.channelId == GX_ALPHA_BUMP || stage.channelId == GX_ALPHA_BUMPN ||
        (stage.texMapId != GX_TEXMAP_NULL &&
         (stage.texMapId > GX_TEXMAP7 || stage.texCoordId > GX_TEXCOORD7 ||
          shader.tcgs[stage.texCoordId].src == GX_MAX_TEXGENSRC))) return false;
    for (const auto& op : {stage.colorOp, stage.alphaOp})
      if ((op.op != GX_TEV_ADD && op.op != GX_TEV_SUB) || op.bias > GX_TB_SUBHALF ||
          op.scale > GX_CS_DIVIDE_2 || op.outReg > GX_TEVREG2) return false;
    const auto validKColor = stage.kcSel <= GX_TEV_KCSEL_1_8 ||
                            (stage.kcSel >= GX_TEV_KCSEL_K0 && stage.kcSel <= GX_TEV_KCSEL_K3_A);
    const auto validKAlpha = stage.kaSel <= GX_TEV_KASEL_1_8 ||
                            (stage.kaSel >= GX_TEV_KASEL_K0_R && stage.kaSel <= GX_TEV_KASEL_K3_A);
    if (!validKColor || !validKAlpha) return false;
  }
  family = specialized;
  auto& generic = family.shaderConfig;
  generic.uberTev = 1;
  generic.alphaCompare = {};
  for (u32 i = 0; i < generic.tevStageCount; ++i) {
    auto& stage = generic.tevStages[i];
    // Keep resource routing static so every member has the same bindings.
    stage.colorPass = {GX_CC_TEXC, GX_CC_RASC, GX_CC_KONST, GX_CC_CPREV};
    stage.alphaPass = {GX_CA_TEXA, GX_CA_RASA, GX_CA_KONST, GX_CA_APREV};
    stage.colorOp = {};
    stage.alphaOp = {};
    stage.kcSel = GX_TEV_KCSEL_K0;
    stage.kaSel = GX_TEV_KASEL_K0_R;
  }
  return true;
}

inline UberUniform make_uber_uniform(const std::array<TevStage, MaxTevStages>& stages, const AlphaCompare& alpha) {
  UberUniform uniform{};
  for (u32 i = 0; i < MaxUberStages; ++i) {
    const auto& stage = stages[i];
    auto& out = uniform.stages[i];
    out.colorArgs = {stage.colorPass.a, stage.colorPass.b, stage.colorPass.c, stage.colorPass.d};
    out.alphaArgs = {stage.alphaPass.a, stage.alphaPass.b, stage.alphaPass.c, stage.alphaPass.d};
    const auto op = [](const TevOp& value) -> std::array<u32, 4> {
      return {value.op, value.bias, value.scale, u32(value.outReg) | (u32(value.clamp) << 8)};
    };
    out.colorOp = op(stage.colorOp);
    out.alphaOp = op(stage.alphaOp);
    out.konst = {stage.kcSel, stage.kaSel, 0, 0};
  }
  uniform.alpha[0] = {alpha.comp0, alpha.ref0, alpha.op, alpha.comp1};
  uniform.alpha[1] = {alpha.ref1, 0, 0, 0};
  return uniform;
}
} // namespace aurora::gx
