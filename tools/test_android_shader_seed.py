#!/usr/bin/env python3
"""Check starter provenance and Android pipeline error recovery."""
from pathlib import Path
import gzip
import importlib.util
import sqlite3
import re
import ast
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parent.parent

class Starter(unittest.TestCase):
    def test_export_requires_successful_device_build(self):
        spec=importlib.util.spec_from_file_location('exporter',ROOT/'tools/gen_android_pipeline_cache.py')
        exporter=importlib.util.module_from_spec(spec);spec.loader.exec_module(exporter)
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'device.db'; out=Path(tmp)/'seed.gz'
            with sqlite3.connect(src) as c:
                c.executescript('''CREATE TABLE pipeline_cache(type,hash,config_version,config_size,config,first_frame_used);
                    CREATE TABLE pipeline_device(type,hash);
                    INSERT INTO pipeline_cache VALUES(1,1,13,1,x'01',5),(1,2,13,1,x'02',6),(2,3,3,1,x'03',7);
                    INSERT INTO pipeline_device VALUES(1,1),(2,3);''')
            self.assertEqual(exporter.export(src,out),1)
            db=Path(tmp)/'seed.db';db.write_bytes(gzip.decompress(out.read_bytes()))
            with sqlite3.connect(db) as c:
                self.assertEqual(c.execute('SELECT hash FROM pipeline_cache').fetchall(),[(1,)])
                self.assertEqual(c.execute('SELECT value FROM melee_portable_starter').fetchone(),(1,))
            with sqlite3.connect(src) as c:c.execute('DELETE FROM pipeline_device')
            with self.assertRaises(ValueError):exporter.export(src,out)

    def test_prune_preserves_only_built_or_starter_rows(self):
        source=(ROOT/'extern/aurora/lib/gfx/pipeline_cache.cpp').read_text()
        begin=source.index('"DELETE FROM pipeline_device WHERE NOT EXISTS')
        end=source.index('");',begin)+1
        sql=''.join(ast.literal_eval(token) for token in re.findall(r'"[^"\n]*"',source[begin:end]))
        with sqlite3.connect(':memory:') as db:
            db.executescript('''CREATE TABLE pipeline_cache(type,hash);
                CREATE TABLE pipeline_device(type,hash);
                CREATE TEMP TABLE pipeline_starter(type,hash);
                INSERT INTO pipeline_cache VALUES(1,1),(1,2),(1,3);
                INSERT INTO pipeline_device VALUES(1,1),(1,4);
                INSERT INTO pipeline_starter VALUES(1,2);''')
            db.executescript(sql)
            self.assertEqual(db.execute('SELECT hash FROM pipeline_cache ORDER BY hash').fetchall(),[(1,),(2,)])
            self.assertEqual(db.execute('SELECT hash FROM pipeline_device').fetchall(),[(1,)])

    def test_stage_fighter_recording_coverage(self):
        spec=importlib.util.spec_from_file_location('recorder',ROOT/'tools/gen_pipeline_cache.py')
        recorder=importlib.util.module_from_spec(spec);spec.loader.exec_module(recorder)
        cases=recorder.coverage_cases()
        self.assertEqual(len(cases),203)
        for stage in set(range(2,33))-{21,26}:
            fighters={(base+i)%26 for st,base in cases if st==stage for i in range(4)}
            self.assertEqual(fighters,set(range(26)))
        source=(ROOT/'src/melee/gm/gmvsmode.c').read_text()
        function=source[source.index('void onEnterDebugVs(GameModeState* state)'):source.index('void onEnterCss(GameModeState* state)')]
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define TARGET_PC
#define Gm_Player_NumMax 6
#define Gm_PKind_Human 0
#define Gm_PKind_Cpu 1
#define Gm_PKind_NA 3
#define MatchKind_Time 0
#define MatchKind_Stock 1
#define CKind_Link 6
#define CKind_Mario 8
#define CKind_Fox 2
#define CKind_Donkey 1
#define CKind_Playable_Count 26
#define St_Kind_Test 1
#define St_Kind_Last 32
typedef int CharacterKind;
typedef int StKind;
typedef int GameModeState;
struct Rules {int stkind,item_freq,sd_penalty,match_kind,is_stock;};
struct Player {int stocks,cpu_kind,ckind,slot_type;bool rumble_enabled;};
typedef struct {struct Rules rules;struct Player players[6];} StartMeleeData;
StartMeleeData data;
StartMeleeData* gm_GetGameModeStateEnterData(GameModeState* s) {return &data;}
void gm_SetupRulesDefaults(struct Rules* r) {memset(r,0,sizeof(*r));}
void gm_SetupPlayerDefaults(struct Player* p) {memset(p,0,sizeof(*p));}
void gm_LoadAnnouncer(void) {}
''' + function + r'''
int main(void) {
 setenv("MELEE_DEBUG_VS","cpu4",1);
 setenv("MELEE_DEBUG_VS_STAGE","32",1);
 setenv("MELEE_DEBUG_VS_FIGHTER_BASE","24",1);
 onEnterDebugVs(NULL);
 assert(data.rules.stkind==32);
 for(int i=0;i<4;++i) {assert(data.players[i].ckind==(24+i)%26);assert(data.players[i].slot_type==1);}
 const char* invalid[]={"","26","-1","2oops"};
 for(int n=0;n<4;++n) {setenv("MELEE_DEBUG_VS_FIGHTER_BASE",invalid[n],1);onEnterDebugVs(NULL);assert(data.players[0].ckind==CKind_Link);}
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'test.c';cpp.write_text(harness);binary=Path(tmp)/'test'
            subprocess.run(['cc',str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_compile_errors_drain_all_scopes(self):
        source=(ROOT/'extern/aurora/lib/gfx/pipeline_cache.cpp').read_text()
        self.assertTrue('static CompiledPipeline checked_android_pipeline(' in source,
                        'Android precompilation needs checked error scopes')
        helper=source[source.index('static CompiledPipeline checked_android_pipeline('):source.index('\n#endif',source.index('static CompiledPipeline checked_android_pipeline('))]
        harness=r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
struct CompiledPipeline { bool main=false; };
using NewPipelineCallback=std::function<CompiledPipeline()>;
namespace wgpu {
enum class ErrorFilter {Validation,Internal,OutOfMemory};
enum class CallbackMode {WaitAnyOnly};
enum class PopErrorScopeStatus {Success,Error};
enum class ErrorType {NoError,Internal};
enum class WaitStatus {Success,TimedOut};
using StringView=const char*;
}
int scopes=0,pops=0;bool fail=false,timeout=false;
namespace webgpu {
struct {
 void PushErrorScope(wgpu::ErrorFilter) {++scopes;}
 template<class F> int PopErrorScope(wgpu::CallbackMode,F callback) {
  assert(scopes>0);--scopes;++pops;
  callback(wgpu::PopErrorScopeStatus::Success,fail?wgpu::ErrorType::Internal:wgpu::ErrorType::NoError,"test");return 1;
 }
} g_device;
struct {wgpu::WaitStatus WaitAny(int,uint64_t) {return timeout?wgpu::WaitStatus::TimedOut:wgpu::WaitStatus::Success;}} g_instance;
}
struct {template<class... T>void warn(const char*,T...) {}} Log;
'''+helper+r'''
int main() {
 auto create=[] {assert(scopes==3);return CompiledPipeline{true};};
 assert(checked_android_pipeline(create).main && scopes==0 && pops==3);
 fail=true;assert(!checked_android_pipeline(create).main && scopes==0 && pops==6);
 fail=false;timeout=true;assert(!checked_android_pipeline(create).main && scopes==0 && pops==9);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp=Path(tmp)/'test.cpp';cpp.write_text(harness);binary=Path(tmp)/'test'
            subprocess.run(['c++','-std=c++17',str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main()
