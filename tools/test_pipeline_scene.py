#!/usr/bin/env python3
"""Exercise production scene queue selection without compiling GPU shaders."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class PipelineScene(unittest.TestCase):
    def test_scene_priority_preserves_queues_and_budget(self):
        text = (ROOT / 'extern/aurora/lib/gfx/pipeline_cache.cpp').read_text()
        self.assertTrue('uint32_t set_pipeline_scene(' in text, 'scene selection is missing')
        code = text[text.index('uint32_t set_pipeline_scene('):text.index('void initialize_pipeline_cache()')]
        source = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
using HashType = uint64_t;
using PipelineRef = uint64_t;
template<class T> using HashSet = std::set<T>;
namespace absl { template<class T> using flat_hash_set = HashSet<T>; }
struct PendingPipeline { PipelineRef hash; };
std::deque<PendingPipeline> g_backgroundPipelineQueue, g_pipelineQueue;
std::set<PipelineRef> g_pendingPipelines;
std::map<uint64_t, HashSet<HashType>> g_scenePipelines;
std::mutex g_pipelineMutex;
uint64_t g_pipelineScene;
bool env_flag(const char* n) { auto* p = getenv(n); return p && *p == '1'; }
struct Layout { uint64_t key = 100; int colorAttachmentCount = 2, sampleCount = 4; };
Layout scene_render_target_layout() { return {}; }
namespace detail { void finalize_render_target_layout(Layout& l) { l.key = 200; } }
uint64_t xxh3_hash(uint64_t layout, uint64_t config) { return layout + config; }
struct { template<class... T> void info(const char*, T...) {} } Log;
int builds;
uint32_t wait_pipelines(uint32_t) {
    auto& queue = g_pipelineQueue.empty() ? g_backgroundPipelineQueue : g_pipelineQueue;
    assert(!queue.empty());
    g_pendingPipelines.erase(queue.front().hash);
    queue.pop_front();
    ++builds;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return g_pendingPipelines.size();
}
''' + code + r'''
int main() {
    g_scenePipelines[1] = {1, 2};
    g_scenePipelines[2] = {3};
    g_backgroundPipelineQueue = {{103}, {101}, {104}, {202}, {102}, {201}};
    g_pipelineQueue = {{999}};
    g_pendingPipelines.insert(999);
    for (auto p : g_backgroundPipelineQueue) g_pendingPipelines.insert(p.hash);
    assert(set_pipeline_scene(1, 0) == 4);
    uint64_t expected[] = {101, 202, 102, 201, 103, 104};
    for (int i=0; i<6; ++i) assert(g_backgroundPipelineQueue[i].hash == expected[i]);
    assert(g_pipelineQueue.front().hash == 999); // never demote demanded draws
    assert(g_pendingPipelines.size() == 7 && builds == 0);
    assert(set_pipeline_scene(1, 1) == 4 && builds == 1); // demanded draw goes first
    assert(g_pipelineQueue.empty());
    assert(set_pipeline_scene(1, 100) == 0 && builds == 5);
    assert(g_backgroundPipelineQueue.size() == 2); // leave unrelated rows alone
    assert(set_pipeline_scene(99, 100) == 0 && builds == 5);
    set_pipeline_scene(0, 0);
    assert(g_pipelineScene == 0);
    setenv("MELEE_SCENE_PREWARM", "0", 1);
    assert(set_pipeline_scene(2, 100) == 0 && g_pipelineScene == 0 && builds == 5);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'test.cpp').write_text(source)
            subprocess.run(['c++', '-std=c++20', str(path / 'test.cpp'), '-pthread',
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
