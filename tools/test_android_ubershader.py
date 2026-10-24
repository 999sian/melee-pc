#!/usr/bin/env python3
"""Compile and exercise the production hybrid family/payload helpers."""
from pathlib import Path
import json, os, subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parent.parent

class HybridShaders(unittest.TestCase):
    def test_family_and_payload(self):
        header = ROOT / 'extern/aurora/lib/gx/uber_shader.hpp'
        self.assertTrue(header.exists(), 'production generic TEV family is missing')
        commands = json.loads((Path(os.environ.get('MELEE_TEST_BUILD', ROOT/'build'))/'compile_commands.json').read_text())
        command = next(c for c in commands if c['file'].endswith('/gx/shader.cpp'))
        import shlex
        args = shlex.split(command['command'])
        # Reuse the real target's include paths/definitions without its source/output.
        cleaned = []; skip = False
        for arg in args[1:]:
            if skip: skip = False; continue
            if arg in ('-o', '-MF', '-MT'): skip = True; continue
            if arg in ('-c','-MD') or arg == command['file']: continue
            cleaned.append(arg)
        cache_source = (ROOT/'extern/aurora/lib/gfx/pipeline_cache.cpp').read_text()
        self.assertIn('static bool portable_starter_config(', cache_source)
        starter = cache_source[cache_source.index('static bool portable_starter_config('):cache_source.index('static void seed_pipeline_cache()')]
        source = r'''
#include "gx/uber_shader.hpp"
#include <cassert>
#include <cstring>
using namespace aurora::gx;
''' + 'namespace aurora::gfx {\n' + starter + '}\n' + r'''
int main() {
  PipelineConfig a{}; a.shaderConfig.tevStageCount = 1;
  auto& stage = a.shaderConfig.tevStages[0];
  stage.texMapId = GX_TEXMAP0; stage.texCoordId = GX_TEXCOORD0;
  stage.channelId = GX_COLOR0A0;
  a.shaderConfig.tcgs[0].src = GX_TG_TEX0;
  stage.colorPass = {GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO};
  stage.alphaPass = {GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO};
  assert(aurora::gfx::portable_starter_config(reinterpret_cast<const uint8_t*>(&a),sizeof(a)));
  assert(!aurora::gfx::portable_starter_config(nullptr,sizeof(a)));
  assert(!aurora::gfx::portable_starter_config(reinterpret_cast<const uint8_t*>(&a),sizeof(a)-1));
  auto invalid=a; invalid.version++;
  assert(!aurora::gfx::portable_starter_config(reinterpret_cast<const uint8_t*>(&invalid),sizeof(invalid)));
  invalid=a; invalid.shaderConfig.numIndStages=1;
  assert(aurora::gfx::portable_starter_config(reinterpret_cast<const uint8_t*>(&invalid),sizeof(invalid)));
  invalid=a; invalid.shaderConfig.uberTev=1;
  assert(aurora::gfx::portable_starter_config(reinterpret_cast<const uint8_t*>(&invalid),sizeof(invalid)));
  PipelineConfig family{}; assert(make_uber_pipeline_config(a, family));
  assert(family.shaderConfig.uberTev == 1);
  auto b = a; b.shaderConfig.tevStages[0].colorPass.d = GX_CC_C0;
  b.shaderConfig.tevStages[0].colorOp = {GX_TEV_SUB,GX_TB_ADDHALF,GX_CS_SCALE_2,GX_TEVREG1,false};
  b.shaderConfig.tevStages[0].kcSel = GX_TEV_KCSEL_K2_A;
  b.shaderConfig.alphaCompare = {GX_LESS,80,GX_AOP_XOR,GX_GEQUAL,201};
  PipelineConfig merged{}; assert(make_uber_pipeline_config(b,merged));
  assert(std::memcmp(&family,&merged,sizeof(family))==0);
  auto payload = make_uber_uniform(b.shaderConfig.tevStages, b.shaderConfig.alphaCompare);
  assert(payload.stages[0].colorArgs[3] == GX_CC_C0);
  assert(payload.stages[0].colorOp[0] == GX_TEV_SUB);
  assert(payload.stages[0].colorOp[3] == GX_TEVREG1);
  assert(payload.stages[0].konst[0] == GX_TEV_KCSEL_K2_A);
  assert(payload.alpha[0][1] == 80 && payload.alpha[1][0] == 201);
  b=a; b.shaderConfig.tevStages[0].texMapId=GX_TEXMAP1;
  assert(make_uber_pipeline_config(b,merged));
  assert(std::memcmp(&family,&merged,sizeof(family))!=0);
  b=a; b.blendMode=GX_BM_SUBTRACT;
  assert(make_uber_pipeline_config(b,merged)); assert(merged.blendMode==GX_BM_SUBTRACT);
  for (int mode=0;mode<7;++mode) {
    b=a;
    if(mode==0) b.shaderConfig.numIndStages=1;
    if(mode==1) b.shaderConfig.zTexOp=GX_ZT_ADD;
    if(mode==2) b.shaderConfig.tevStageCount=5;
    if(mode==3) b.shaderConfig.tevStages[0].colorOp.op=GX_TEV_COMP_R8_GT;
    if(mode==4) b.shaderConfig.tcgs[0].postMtx=GX_PTTEXMTX0;
    if(mode==5) b.shaderConfig.tevStages[0].texCoordId=GX_TEXCOORD_NULL;
    if(mode==6) b.shaderConfig.tevStageCount=0;
    assert(!make_uber_pipeline_config(b,merged));
  }
  static_assert(sizeof(UberStageUniform)==80);
  static_assert(sizeof(UberUniform)==352);
  static_assert(offsetof(UberUniform,alpha)==320);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'test.cpp'; cpp.write_text(source)
            exe=Path(tmp)/'test'
            subprocess.run([args[0],*cleaned,'-UNDEBUG','-I'+str(ROOT/'extern/aurora/lib'),str(cpp),'-o',str(exe)],cwd=command['directory'],check=True,capture_output=True,text=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__': unittest.main()
