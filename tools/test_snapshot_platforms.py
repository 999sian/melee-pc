#!/usr/bin/env python3
"""Cross-link the shipped snapshot adapters without building graphics libraries.

Mach-O x86-64/macOS and arm64/iOS, PE ARM64, and ELF AArch64 exercise actual
object layouts, relocations, archive extraction and linker garbage collection.
Run on Linux with LLVM and the Windows ARM64 GCC bridge installed. These are
link/coverage checks; they do not claim gameplay/device runtime validation.
"""
from pathlib import Path
import os
import shutil
import subprocess
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
LLVM_MINGW = Path(os.environ.get('LLVM_MINGW', '/home/sian/toolchains/llvm-mingw')) / 'bin'
GAME = '''
int game_value=73;
int game_zero[1024];
static int local_value=11, local_zero[17];
int *game_pointer=&game_value, *game_zero_pointer=game_zero;
int game_read(void) { return *game_pointer+*game_zero_pointer+local_value+local_zero[0]; }
int (*game_function)(void)=game_read;
void mutate(void) { local_value++; local_zero[0]++; }
'''
AUDIO = 'int audio_value=9; int audio_zero[1024];\n'
ENGINE = 'int engine_value=21; int engine_zero[32];\n'
# Refer to every range and fixture global so dead stripping cannot hide omissions.
MAIN = '''
#include "melee_state.h"
extern int game_value,game_zero[],*game_pointer,*game_zero_pointer;
extern int (*game_function)(void);
extern int audio_value,audio_zero[],engine_value,engine_zero[];
volatile void* observed[12];
void start(void) {
 observed[0]=__melee_data_start; observed[1]=__melee_data_end;
 observed[2]=__melee_bss_start; observed[3]=__melee_bss_end;
 observed[4]=&audio_value; observed[5]=audio_zero;
 observed[6]=&engine_value; observed[7]=engine_zero;
 observed[8]=game_zero; observed[9]=game_pointer;
 observed[10]=game_zero_pointer; observed[11]=&game_function;
 game_value=game_function();
}
'''
TRACKED = ['game_value', 'game_zero', 'game_pointer', 'game_zero_pointer', 'game_function']
EXCLUDED = ['audio_value', 'audio_zero', 'engine_value', 'engine_zero', 'observed']


class SnapshotPlatforms(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='melee_game-snapshot-')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        for name, body in [('game.c', GAME), ('axdriver.c', AUDIO), ('engine.c', ENGINE), ('main.c', MAIN)]:
            (self.work / name).write_text(body)

    def run_tool(self, *command, **kwargs):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def tool(self, name):
        tool = shutil.which(name)
        if not tool:
            self.skipTest(f'{name} unavailable')
        return tool

    def macho(self, target, arch, platform, minimum, bridge=False):
        cc, ld, ar = self.tool('clang'), self.tool('ld64.lld'), self.tool('llvm-ar')
        launcher = [sys.executable, TOOLS/'macho_snapshot_compile.py', '--']
        if bridge:
            launcher += [sys.executable, TOOLS/'gcc_ios_launcher.py']
        flags = [f'--target={target}', '-fno-common', '-fno-stack-protector', '-I'+str(ROOT/'src/pc')]
        for name in ['game', 'axdriver']:
            self.run_tool(*launcher, cc, *flags, '-c', self.work/(name+'.c'), '-o', self.work/(name+'.o'))
        for name in ['engine', 'main']:
            self.run_tool(cc, *flags, '-c', self.work/(name+'.c'), '-o', self.work/(name+'.o'))
        self.run_tool(cc, *flags, '-c', ROOT/'src/pc/melee_state_macho.c', '-o', self.work/'markers.o')
        self.run_tool(ar, 'rcs', self.work/'libmelee_game.a', self.work/'game.o', self.work/'axdriver.o')
        image = self.work/'game'
        self.run_tool(ld, '-arch', arch, '-platform_version', platform, minimum, minimum,
                      '-e', '_start', '-dead_strip', '-o', image, self.work/'main.o',
                      self.work/'markers.o', self.work/'engine.o', self.work/'libmelee_game.a')
        self.run_tool(sys.executable, TOOLS/'macho_snapshot_verify.py', image,
                      '--tracked', *TRACKED, '--excluded', *EXCLUDED)
        # A common symbol must never pass through with no snapshot allocation.
        failure = subprocess.run([*map(str, launcher), cc, f'--target={target}', '-fcommon',
                                  '-c', str(self.work/'game.c'), '-o', str(self.work/'common.o')],
                                 capture_output=True, text=True)
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn('COMMON', failure.stderr)
        self.assertFalse((self.work/'common.o').exists())

    def test_macos_x86_64(self):
        self.macho('x86_64-apple-macos11', 'x86_64', 'macos', '11.0')

    def test_ios_arm64(self):
        self.macho('arm64-apple-ios14', 'arm64', 'ios', '14.0')

    def test_ios_game_compiler_bridge(self):
        if not Path('/home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc').exists():
            self.skipTest('GCC AArch64 bridge unavailable')
        self.macho('arm64-apple-ios14', 'arm64', 'ios', '14.0', bridge=True)

    def windows_arm64(self, bridge=False, per_symbol=False):
        cc = LLVM_MINGW/'aarch64-w64-mingw32-clang'
        if not cc.exists():
            self.skipTest('Windows ARM64 toolchain unavailable')
        objdump, objcopy, nm = [LLVM_MINGW/('llvm-'+name) for name in ['objdump', 'objcopy', 'nm']]
        launcher = [sys.executable, TOOLS/'pe_snapshot_compile.py', '--objcopy', objcopy,
                    '--objdump', objdump, '--']
        if bridge:
            launcher += [sys.executable, TOOLS/'gcc_windows_arm64_launcher.py']
        flags = ['-fdata-sections'] if per_symbol else []
        for name in ['game', 'axdriver']:
            self.run_tool(*launcher, cc, '-fno-common', *flags, '-c', self.work/(name+'.c'), '-o', self.work/(name+'.o'))
        for name in ['engine', 'main']:
            self.run_tool(cc, '-I'+str(ROOT/'src/pc'), '-c', self.work/(name+'.c'), '-o', self.work/(name+'.o'))
        self.run_tool(cc, '-c', ROOT/'src/pc/melee_state_pe.c', '-o', self.work/'markers.o')
        image = self.work/'game.exe'
        self.run_tool(cc, '-nostdlib', '-Wl,--entry,start', self.work/'main.o', self.work/'markers.o',
                      self.work/'engine.o', self.work/'game.o', self.work/'axdriver.o', '-o', image)
        self.run_tool(sys.executable, TOOLS/'pe_snapshot_verify.py', '--nm', nm, image,
                      '--tracked', *TRACKED, '--excluded', *EXCLUDED)

    def test_windows_arm64(self):
        self.windows_arm64()

    def test_windows_arm64_split_sections(self):
        self.windows_arm64(per_symbol=True)

    def test_windows_arm64_game_compiler_bridge(self):
        self.windows_arm64(bridge=True)

    def test_macho_universal_preserves_relocations(self):
        from macho_snapshot import images, rewrite
        cc = self.tool('clang')
        chunks = []
        for target in ['x86_64-apple-macos11', 'arm64-apple-macos11']:
            obj = self.work/(target+'.o')
            self.run_tool(cc, '--target='+target, '-fno-common', '-c', self.work/'game.c', '-o', obj)
            chunks.append(obj.read_bytes())
        offsets = [48, (48+len(chunks[0])+7) & ~7]
        header = struct.pack('>II', 0xcafebabe, 2)
        for cpu, offset, chunk in zip([0x01000007, 0x0100000c], offsets, chunks):
            header += struct.pack('>5I', cpu, 0, offset, len(chunk), 3)
        before = header+chunks[0]+bytes(offsets[1]-offsets[0]-len(chunks[0]))+chunks[1]
        obj = self.work/'universal.o'
        obj.write_bytes(before)
        allowed = set()
        for image in images(before):
            for at, segment, _, _, _, _ in image.sections:
                if segment == '__DATA':
                    allowed.update(range(image.base+at, image.base+at+32))
        rewrite(obj)
        after = obj.read_bytes()
        self.assertEqual(len(before), len(after))
        changed = {i for i, (a, b) in enumerate(zip(before, after)) if a != b}
        self.assertTrue(changed)
        self.assertTrue(changed <= allowed, 'relocations, symbols or section layout changed')
        for image in images(after):
            self.assertIn('__melee_data', [s[2] for s in image.sections])
            self.assertIn('__melee_bss', [s[2] for s in image.sections])

    def test_macho_rejects_uncovered_thread_state(self):
        cc = self.tool('clang')
        source, obj = self.work/'thread.c', self.work/'thread.o'
        source.write_text('__thread int value; int read(void) { return value; }')
        result = subprocess.run([sys.executable, str(TOOLS/'macho_snapshot_compile.py'), '--',
            cc, '--target=x86_64-apple-macos11', '-fno-common', '-c', str(source), '-o', str(obj)],
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('uncovered writable section', result.stderr)
        self.assertFalse(obj.exists())

    def cmake_integration(self, apple):
        cmake, ninja = self.tool('cmake'), self.tool('ninja')
        if apple:
            cc = self.tool('clang')
            linker = self.tool('ld64.lld')
            system, cpu, module, function = 'Darwin', 'arm64', 'AppleSnapshot', 'apple'
            bridge = TOOLS/'gcc_ios_launcher.py'
            extra = (f'set(CMAKE_C_COMPILER_TARGET arm64-apple-ios14.0)\n'
                     f'set(CMAKE_EXE_LINKER_FLAGS "-nostdlib -fuse-ld={linker} -Wl,-e,_start -Wl,-dead_strip -Wl,-arch,arm64 -Wl,-platform_version,ios,14.0,16.5")\n')
        else:
            cc = LLVM_MINGW/'aarch64-w64-mingw32-clang'
            if not cc.exists():
                self.skipTest('Windows ARM64 compiler unavailable')
            system, cpu, module, function = 'Windows', 'ARM64', 'WindowsSnapshot', 'windows'
            bridge = TOOLS/'gcc_windows_arm64_launcher.py'
            extra = (f'set(CMAKE_OBJCOPY "{LLVM_MINGW / "llvm-objcopy"}")\n'
                     f'set(CMAKE_OBJDUMP "{LLVM_MINGW / "llvm-objdump"}")\n'
                     f'set(CMAKE_NM "{LLVM_MINGW / "llvm-nm"}")\n'
                     'set(CMAKE_EXE_LINKER_FLAGS "-nostdlib -Wl,--entry,start -Wl,--gc-sections")\n')
        gcc = os.environ.get('GCC_AARCH64_BIN') or shutil.which('aarch64-linux-gnu-gcc')
        if not gcc and not Path('/home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc').exists():
            self.skipTest('GCC AArch64 bridge unavailable')
        env = dict(os.environ)
        if gcc:
            env['GCC_AARCH64_BIN'] = gcc
        (self.work/'toolchain.cmake').write_text(
            f'set(CMAKE_SYSTEM_NAME {system})\nset(CMAKE_SYSTEM_PROCESSOR {cpu})\n'
            f'set(CMAKE_C_COMPILER "{cc}")\nset(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n'+extra)
        required = ['HSD_PadLibData', 'HSD_Rumble_804C22E0', 'gmVsMelee_StartData', 'net', 'HSD_Synth_804D6018']
        (self.work/'game.c').write_text(GAME+'int '+','.join(required[:3])+';\n')
        (self.work/'engine.c').write_text(ENGINE+'int net;\n')
        (self.work/'axdriver.c').write_text(AUDIO+'int HSD_Synth_804D6018;\n')
        (self.work/'main.c').write_text(('extern int '+','.join(required)+';\n')+
            MAIN.replace('game_value=game_function();', 'game_value=game_function()+'+'+'.join(required)+';'))
        (self.work/'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(snapshot C)\n'
            'add_library(melee_game STATIC game.c axdriver.c)\n'
            f'set_property(TARGET melee_game PROPERTY C_COMPILER_LAUNCHER "{sys.executable};{bridge}")\n'
            'add_executable(melee main.c engine.c)\n'
            f'target_include_directories(melee PRIVATE "{ROOT / "src/pc"}")\n'
            'target_link_libraries(melee PRIVATE melee_game)\n'
            f'include("{ROOT / "cmake" / (module+".cmake")}")\n'
            f'melee_enable_{function}_snapshots(melee melee_game)\n'
            'if(NOT MELEE_STATE_SECTIONS)\nmessage(FATAL_ERROR "rollback missing")\nendif()\n')
        self.run_tool(cmake, '-S', self.work, '-B', self.work/'build', '-G', 'Ninja',
                      '-DCMAKE_TOOLCHAIN_FILE='+str(self.work/'toolchain.cmake'), env=env)
        self.run_tool(cmake, '--build', self.work/'build', env=env)

    def test_apple_cmake_integration(self):
        self.cmake_integration(apple=True)

    def test_windows_arm64_cmake_integration(self):
        self.cmake_integration(apple=False)

    @unittest.skipUnless(sys.platform == 'darwin', 'native macOS execution requires macOS')
    def test_native_mac_restore(self):
        cc, ar = self.tool('clang'), self.tool('ar')
        launcher = [sys.executable, TOOLS/'macho_snapshot_compile.py', '--',
                    sys.executable, TOOLS/'gcc_launcher.py']
        for name in ['game', 'axdriver']:
            self.run_tool(*launcher, cc, '-fno-common', '-c', self.work/(name+'.c'),
                          '-o', self.work/(name+'.o'))
        self.run_tool(cc, '-c', self.work/'engine.c', '-o', self.work/'engine.o')
        self.run_tool(ar, 'rcs', self.work/'libmelee_game.a', self.work/'game.o', self.work/'axdriver.o')
        image = self.work/'restore'
        self.run_tool(cc, '-I'+str(ROOT/'src/pc'), TOOLS/'snapshot_restore_fixture.c',
                      ROOT/'src/pc/melee_state_macho.c', self.work/'engine.o',
                      self.work/'libmelee_game.a', '-Wl,-dead_strip', '-o', image)
        self.run_tool(sys.executable, TOOLS/'macho_snapshot_verify.py', image,
                      '--tracked', *TRACKED, '--excluded', *EXCLUDED[:-1])
        self.run_tool(image)

    def android(self, arch):
        ndk = Path(os.environ.get('ANDROID_NDK_HOME', '/home/sian/Android/ndk/26.3.11579264'))
        binaries = ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
        cc = binaries/(arch+'-linux-android26-clang')
        if not cc.exists():
            self.skipTest(f'Android NDK {arch} compiler unavailable')
        gcc = os.environ.get('GCC_AARCH64_BIN') or shutil.which('aarch64-linux-gnu-gcc')
        if not gcc and Path('/home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc').exists():
            gcc = '/home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc'
        if arch == 'aarch64' and not gcc:
            self.skipTest('AArch64 GCC unavailable')
        env = dict(os.environ, ANDROID_NDK_HOME=str(ndk))
        if gcc:
            env['GCC_AARCH64_BIN'] = gcc
        flags = ['-fno-common', '-fno-stack-protector', '-ffreestanding', '-fno-builtin']
        for name in ['game', 'axdriver']:
            self.run_tool(sys.executable, TOOLS/'gcc_launcher.py', cc, *flags, '-c',
                          self.work/(name+'.c'), '-o', self.work/(name+'.c.o'), env=env)
        self.run_tool(cc, *flags, '-c', self.work/'engine.c', '-o', self.work/'engine.o')
        self.run_tool(binaries/'llvm-ar', 'rcs', self.work/'libmelee_game.a',
                      self.work/'game.c.o', self.work/'axdriver.c.o')
        image = self.work/'restore'
        self.run_tool(cc, *flags, '-DSNAPSHOT_ELF_ENTRY', '-I'+str(ROOT/'src/pc'),
                      TOOLS/'snapshot_restore_fixture.c', self.work/'engine.o',
                      self.work/'libmelee_game.a', '-nostdlib', '-static', '-Wl,-e,_start',
                      '-Wl,-T,'+str(ROOT/'src/pc/melee_state.ld'), '-o', image)
        # The same symbol/range checker applies to ELF's linker-defined names.
        self.run_tool(sys.executable, TOOLS/'pe_snapshot_verify.py', '--nm', binaries/'llvm-nm',
                      image, '--tracked', *TRACKED, '--excluded', *EXCLUDED[:-1])
        if arch == 'aarch64':
            self.run_tool(self.tool('qemu-aarch64'), image)
        else:
            self.run_tool(image)

    @unittest.skipUnless(sys.platform == 'linux', 'freestanding Android execution requires Linux')
    def test_android_arm64_restore(self):
        self.android('aarch64')

    @unittest.skipUnless(sys.platform == 'linux', 'freestanding Android execution requires Linux')
    def test_android_x86_64_restore(self):
        self.android('x86_64')


if __name__ == '__main__':
    unittest.main()
