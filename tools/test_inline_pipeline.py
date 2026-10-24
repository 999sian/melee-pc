#!/usr/bin/env python3
"""Exercise production inline pipeline scheduling without a graphics driver."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class InlinePipeline(unittest.TestCase):
    def test_pending_draw_uses_available_compile_budget(self):
        cache = (ROOT / 'extern/aurora/lib/gfx/pipeline_cache.cpp').read_text()
        queues = cache[cache.index('template <typename Queue>'):cache.index('static void notify_pipeline_ready')]
        settings = cache[cache.index('static bool env_flag('):cache.index('/* MELEE_PIPELINE_JOBS=')]
        initialize = cache[cache.index('void initialize_pipeline_cache()'):cache.index('void shutdown_pipeline_cache()')]
        find = cache[cache.index('static PipelineRef find_pipeline_impl('):cache.index('#ifdef __ANDROID__\nstatic CompiledPipeline checked_android_pipeline(')]
        source = r'''
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <thread>
#include <vector>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#define ZoneScoped
using PipelineRef = unsigned;
struct CompiledPipeline { unsigned pipeline_count() const { return 1; } };
using NewPipelineCallback = std::function<CompiledPipeline()>;
struct PendingPipeline { PipelineRef hash; NewPipelineCallback create; };
std::mutex g_pipelineMutex;
bool g_hasPipelineThread = false, g_pipelineThreadEnd = false;
size_t g_pipelinesPerFrame = 0;
constexpr size_t BuildPipelinesPerFrame = 1;
std::map<PipelineRef, CompiledPipeline> g_pipelines;
std::set<PipelineRef> g_pendingPipelines, g_demandedPipelines;
std::deque<PendingPipeline> g_pipelineQueue, g_backgroundPipelineQueue;
std::condition_variable g_pipelineQueueCv, g_pipelineReadyCv;
unsigned queuedPipelines = 0, builds = 0;
CompiledPipeline create_timed(const NewPipelineCallback& create) { return create(); }
void notify_pipeline_ready(bool queued, uint32_t) {
    if (queued) --queuedPipelines;
    g_pipelineReadyCv.notify_all();
}
''' + settings + queues + find + r'''
namespace wgpu { enum class BackendType { WebGPU }; }
namespace webgpu { auto g_backendType = wgpu::BackendType::WebGPU; }
bool g_pipelineCacheBroken = false, g_pipelineCacheWriterStop = false, g_gpuCachePrunePending = false;
std::vector<std::thread> g_pipelineThreads;
unsigned pipeline_job_count() { return 0; }
void pipeline_worker() {}
size_t load_pipeline_cache() { return 0; }
void rebuild_pipeline_cache() {}
void start_pipeline_cache_writer() {}
struct { template<class... T> void info(const char*, T...) {} } Log;
''' + initialize + r'''
CompiledPipeline create() { ++builds; return {}; }
void reset() {
    g_hasPipelineThread = false;
    g_pipelinesPerFrame = builds = queuedPipelines = 0;
    g_pipelines.clear();
    g_pendingPipelines.clear();
    g_demandedPipelines.clear();
    g_pipelineQueue.clear();
    g_backgroundPipelineQueue.clear();
}
void pending(bool background) {
    auto& queue = background ? g_backgroundPipelineQueue : g_pipelineQueue;
    queue.push_back({7, create});
    g_pendingPipelines.insert(7);
    ++queuedPipelines;
}
int main() {
    // Android applies melee-env.txt after static initializers, before cache startup.
    setenv("MELEE_PIPELINE_SYNC", "1", 1);
    initialize_pipeline_cache();
    assert(g_pipelineSync);
    setenv("MELEE_PIPELINE_SYNC", "0", 1);
    initialize_pipeline_cache();
    assert(!g_pipelineSync);
    reset();
    find_pipeline_impl(9, create, PipelinePriority::Background);
    assert(builds == 0 && g_backgroundPipelineQueue.size() == 1);
    // Re-requesting deferred work must not promote it into the draw queue.
    find_pipeline_impl(9, create, PipelinePriority::Background);
    assert(builds == 0 && g_pipelineQueue.empty());
    // A draw must not disappear just because its config was already queued.
    for (bool background : {false, true}) {
        reset();
        pending(background);
        find_pipeline_impl(7, create, PipelinePriority::Normal);
        assert(builds == 1 && g_pipelines.contains(7));
        assert(g_pipelinesPerFrame == 1 && queuedPipelines == 0);
        assert(g_pendingPipelines.empty());
        assert(g_pipelineQueue.empty() && g_backgroundPipelineQueue.empty());
        find_pipeline_impl(7, create, PipelinePriority::Normal);
        assert(builds == 1); // Repeated draws reuse the compiled pipeline.
    }
    reset();
    pending(true);
    g_pipelinesPerFrame = BuildPipelinesPerFrame;
    find_pipeline_impl(7, create, PipelinePriority::Normal);
    assert(builds == 0 && queuedPipelines == 1);
    assert(g_pipelineQueue.size() == 1 && g_backgroundPipelineQueue.empty());
    find_pipeline_impl(7, create, PipelinePriority::Blocking);
    assert(builds == 1 && queuedPipelines == 0); // Explicit sync still overrides the budget.
    reset();
    pending(true);
    g_hasPipelineThread = true;
    find_pipeline_impl(7, create, PipelinePriority::Normal);
    assert(builds == 0 && queuedPipelines == 1); // Workers keep their asynchronous path.
    reset();
    pending(true);
    find_pipeline_impl(7, create, PipelinePriority::Background);
    assert(builds == 0 && g_backgroundPipelineQueue.size() == 1);
}
'''
        with tempfile.TemporaryDirectory(prefix='inline-pipeline-') as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(source)
            subprocess.run(['c++', '-std=c++20', '-pthread', str(path / 'test.cpp'),
                            '-o', str(path / 'test')], check=True)
            result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
