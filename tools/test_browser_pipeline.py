#!/usr/bin/env python3
"""Exercise production browser alpha/main pipeline orchestration without a GPU."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class BrowserPipeline(unittest.TestCase):
    def test_alpha_prepass_survives_async_main_creation(self):
        gx = (ROOT / 'extern/aurora/lib/gx/gx.cpp').read_text()
        pipeline = (ROOT / 'extern/aurora/lib/gx/pipeline.cpp').read_text()
        start = gx.index('#ifdef __EMSCRIPTEN__', gx.index('wgpu::RenderPipeline build_pipeline('))
        browser_build = gx[start:gx.index('\n#endif', start)]
        start = gx.index('bool take_browser_pipeline(')
        take = gx[start:gx.index('\n#endif', start)]
        start = pipeline.index('gfx::CompiledPipeline create_pipeline(')
        create = pipeline[start:pipeline.index('// Diagnostics', start)]
        source = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#define ZoneScoped
#define __EMSCRIPTEN__ 1
#define EM_ASM(...) ((void)0)
using HashType = unsigned;
enum GXBlendFactor { GX_BL_SRCALPHA, GX_BL_INVSRCALPHA };
enum { GX_BM_BLEND };
namespace wgpu {
struct RenderPipeline { int id=0; explicit operator bool() const { return id!=0; } };
using ShaderModule=int;
using StringView=const char*;
enum class CreatePipelineAsyncStatus { Success };
enum class CallbackMode { AllowSpontaneous };
}
namespace gfx {
using PipelineRef=unsigned;
enum class ShaderType { GX };
struct RenderTargetLayout { unsigned key=0,colorAttachmentCount=1; };
struct CompiledPipeline { wgpu::RenderPipeline main,prepass; };
}
namespace webgpu { bool g_dualSourceBlendingSupported=false; }
namespace fmt { template<class... T> std::string format(const char*,T...) { return "test"; } }
template<class T> unsigned xxh3_hash(const T&,unsigned=0) { return 1; }
struct { void error(const char*) {} } Log;
enum class DstAlphaMode { None,Replace,DualSource };
struct PipelineOptions {
    DstAlphaMode dstAlphaMode=DstAlphaMode::None;
    bool colorUpdate=false,alphaUpdate=false,depthUpdate=false;
};
struct PipelineConfig {
    int shaderConfig=0;
    bool colorUpdate=true,alphaUpdate=true,depthUpdate=true;
    unsigned dstAlpha=128;
    int blendMode=GX_BM_BLEND;
    GXBlendFactor blendFacSrc=GX_BL_SRCALPHA,blendFacDst=GX_BL_INVSRCALPHA;
};
int synchronous=0,asynchronous=0;
std::function<void()> complete;
struct Device {
    wgpu::RenderPipeline CreateRenderPipeline(const int*) { ++synchronous; return {1}; }
    template<class Callback>
    void CreateRenderPipelineAsync(const int*,wgpu::CallbackMode,Callback callback) {
        ++asynchronous;
        complete=[callback] { callback(wgpu::CreatePipelineAsyncStatus::Success,{2},""); };
    }
} g_device;
std::map<gfx::PipelineRef,std::shared_ptr<wgpu::RenderPipeline>> sBrowserPipelines;
int build_shader(int,const gfx::RenderTargetLayout&,DstAlphaMode) { return 0; }
wgpu::RenderPipeline build_pipeline(const PipelineConfig& config,const gfx::RenderTargetLayout& layout,
                                    const PipelineOptions& options,int,int,const char*) {
    int descriptor=0;
''' + browser_build + '\n#endif\n}\n' + take + create + r'''
int main() {
    auto pipeline=create_pipeline({},{});
    assert(synchronous==1 && asynchronous==1);
    assert(pipeline.prepass && !pipeline.main);
    assert(!take_browser_pipeline(1,pipeline.main));
    complete();
    assert(take_browser_pipeline(1,pipeline.main));
    assert(pipeline.prepass.id==1 && pipeline.main.id==2);
    assert(sBrowserPipelines.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix='browser-pipeline-') as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(source)
            subprocess.run(['c++', '-std=c++20', str(path / 'test.cpp'), '-o',
                            str(path / 'test')], check=True)
            result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
