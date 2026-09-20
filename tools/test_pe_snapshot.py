#!/usr/bin/env python3
"""Isolated MinGW fixture for PE rollback ranges; optional --wine execution.

No project build is touched. Verifies normal and per-symbol writable sections,
archive extraction, engine/audio exclusions, zero-fill, pointer relocations,
range coverage and save/mutate/restore at runtime when --wine is requested.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GAME = r'''
int game_value=73;
int game_zero[1024];
int* game_pointer=&game_value;
int* game_zero_pointer=game_zero;
int game_read(void) { return *game_pointer+*game_zero_pointer; }
int (*game_function)(void)=game_read;
'''
AUDIO = 'int audio_value=9; int audio_zero[1024];\n'
ENGINE = 'int engine_value=21; int engine_zero[32];\n'
MAIN = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern char __melee_data_start[],__melee_data_end[],__melee_bss_start[],__melee_bss_end[];
extern int game_value,game_zero[1024],*game_pointer,*game_zero_pointer;
extern int (*game_function)(void);
extern int audio_value,audio_zero[1024],engine_value,engine_zero[32];
extern int game_read(void);
static int inside(void* p,char* a,char* z) { return (uintptr_t)p>=(uintptr_t)a && (uintptr_t)p<(uintptr_t)z; }
static int tracked(void* p) { return inside(p,__melee_data_start,__melee_data_end) || inside(p,__melee_bss_start,__melee_bss_end); }
#define CHECK(x) do { if(!(x)) { printf("FAIL %d %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
 size_t d=__melee_data_end-__melee_data_start,b=__melee_bss_end-__melee_bss_start;
 CHECK(d>1 && b>0 && d<16384 && b<16384);
 CHECK(tracked(&game_value) && tracked(&game_pointer) && tracked(&game_zero_pointer) && tracked(&game_function));
 CHECK(tracked(game_zero) && tracked(&game_zero[1023]));
 CHECK(!tracked(&audio_value) && !tracked(audio_zero) && !tracked(&audio_zero[1023]));
 CHECK(!tracked(&engine_value) && !tracked(engine_zero));
 CHECK(game_function()==73 && game_pointer==&game_value && game_zero_pointer==game_zero);
 char* ds=malloc(d); char* bs=malloc(b); CHECK(ds && bs);
 memcpy(ds,__melee_data_start,d);memcpy(bs,__melee_bss_start,b);
 game_value=19;game_zero[0]=17;game_zero[1023]=29;game_pointer=&audio_value;game_zero_pointer=audio_zero;game_function=NULL;
 audio_value=81;audio_zero[0]=95;engine_value=42;engine_zero[0]=16;
 memcpy(__melee_data_start,ds,d);memcpy(__melee_bss_start,bs,b);
 CHECK(game_function()==73 && game_pointer==&game_value && game_zero_pointer==game_zero);
 CHECK(game_zero[0]==0 && game_zero[1023]==0);
 CHECK(audio_value==81 && audio_zero[0]==95 && engine_value==42 && engine_zero[0]==16);
 free(ds);free(bs);printf("PE snapshot restore passed (data=%zu, bss=%zu)\n",d,b);return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wine', action='store_true', help='execute fixtures under Wine (requires local socket permission)')
    args = parser.parse_args()
    names = ['gcc', 'ar', 'objcopy', 'objdump', 'nm']
    tools = {name: shutil.which('x86_64-w64-mingw32-' + name) for name in names}
    if not all(tools.values()):
        print('SKIP: MinGW x86-64 compiler/binutils unavailable')
        return 77
    wine = shutil.which('wine') if args.wine else None
    if args.wine and not wine:
        print('SKIP: Wine unavailable')
        return 77
    with tempfile.TemporaryDirectory(prefix='melee-pe-snapshot-') as temporary:
        root = Path(temporary)
        for name, contents in [('game.c', GAME), ('axdriver.c', AUDIO), ('engine.c', ENGINE), ('main.c', MAIN)]:
            (root/name).write_text(contents)
        launch = [sys.executable, str(ROOT/'tools/pe_snapshot_compile.py'), '--objcopy', tools['objcopy'], '--objdump', tools['objdump'], '--']
        run = lambda command, **kw: subprocess.run([str(x) for x in command], check=True, **kw)
        run([tools['gcc'], '-c', ROOT/'src/pc/melee_state_pe.c', '-o', root/'markers.o'])
        run([tools['gcc'], '-c', root/'engine.c', '-o', root/'engine.o'])
        # Production exclusion comes from ELF's EXCLUDE_FILE list.
        run([*launch, tools['gcc'], '-c', root/'axdriver.c', '-o', root/'axdriver.o'])
        audio_sections = subprocess.check_output([tools['objdump'], '-h', root/'axdriver.o'], text=True)
        assert '.mld$' not in audio_sections and '.mlb$' not in audio_sections
        for per_symbol in [False, True]:
            flags = ['-fdata-sections'] if per_symbol else []
            run([*launch, tools['gcc'], '-fno-common', *flags, '-c', root/'game.c', '-o', root/'game.o'])
            run([tools['ar'], 'rcs', root/'libmelee_game.a', root/'game.o', root/'axdriver.o'])
            exe = root/('split.exe' if per_symbol else 'normal.exe')
            run([tools['gcc'], '-fno-common', root/'main.c', root/'markers.o', root/'engine.o', root/'libmelee_game.a', '-Wl,--gc-sections', '-o', exe])
            symbols = subprocess.check_output([tools['nm'], '-n', exe], text=True)
            address = {}
            for line in symbols.splitlines():
                fields = line.split()
                if len(fields) == 3:
                    try: address[fields[2]] = int(fields[0], 16)
                    except ValueError: pass
            def tracked(symbol):
                at=address[symbol]
                return any(address[f'__melee_{kind}_start'] <= at < address[f'__melee_{kind}_end'] for kind in ['data','bss'])
            assert all(tracked(name) for name in ['game_value','game_zero','game_pointer','game_zero_pointer','game_function'])
            assert not any(tracked(name) for name in ['audio_value','audio_zero','engine_value','engine_zero'])
            run([sys.executable, ROOT/'tools/pe_snapshot_verify.py', '--nm', tools['nm'], exe,
                 '--tracked', 'game_value', 'game_zero', 'game_pointer', 'game_function',
                 '--excluded', 'audio_value', 'audio_zero', 'engine_value', 'engine_zero'])
            headers = subprocess.check_output([tools['objdump'], '-h', exe], text=True)
            assert '.mld' in headers and '.mlb' in headers
            if wine:
                prefix=root/'wine';prefix.mkdir(exist_ok=True)
                env=dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all')
                run([wine, exe], env=env, timeout=90)
                server=shutil.which('wineserver')
                if server:
                    run([server,'-k'],env=env,timeout=15)
                    run([server,'-w'],env=env,timeout=15)
        # Common symbols must fail closed rather than silently escape snapshots.
        failure = subprocess.run([*launch, tools['gcc'], '-fcommon', '-c', str(root/'game.c'), '-o', str(root/'common.o')], capture_output=True, text=True)
        assert failure.returncode != 0 and 'COMMON' in failure.stderr and not (root/'common.o').exists(), failure.stderr
    print('PE snapshot boundary/exclusion/relocation fixtures passed' + (' including Wine restore' if wine else ' (runtime not requested)'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
