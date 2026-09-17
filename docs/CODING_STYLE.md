# melee-pc Coding Style & Engineering Best Practices Guide

This document establishes the official coding standards, architectural guidelines, portability practices, and readability conventions for the **melee-pc** project.

All contributors and maintainers should follow these guidelines to keep the codebase maintainable, portable across desktop and mobile platforms, and easy for humans to read and reason about.

---

## 1. Architectural Overview & The Two Layers

The `melee-pc` codebase is divided into two distinct architectural domains:

```
                          ┌────────────────────────┐
                          │   Retail Game Files    │
                          │ (.dat, .usd, opening)  │
                          └───────────┬────────────┘
                                      │ (Big-Endian in MEM1)
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 1. Game Decompilation Layer (src/melee, src/sysdolphin)                     │
│    - C11 (GNU dialect, GCC required for big-endian disc structs)            │
│    - Reconstructed from Nintendo / HAL Laboratory GameCube DOL               │
│    - Dolphin SDK types (u8, u32, f32, Vec3, etc.)                           │
│    - Must maintain symbol and structural parity with doldecomp/melee        │
└─────────────────────────────────────┬───────────────────────────────────────┘
                                      │ (Hardware API hooks / simulation)
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 2. PC Platform & Systems Layer (src/pc)                                     │
│    - C11 (Platform glue: main, vi, gx, audio, os, touch, widescreen)        │
│    - C++20 (Subsystems: launcher, updater, version, RmlUi integration)      │
│    - Standard types (uint8_t, uint32_t, size_t, std::filesystem, etc.)     │
│    - Cross-platform: Linux (X11/Wayland), Windows (MinGW), Android (arm64)  │
└─────────────────────────────────────┬───────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 3. External Dependencies & Backend (extern/aurora, SDL3, WebGPU/Dawn)       │
└─────────────────────────────────────────────────────────────────────────────┘
```

### The Boundary Rule
- **Decomp code (`src/melee/`, `src/sysdolphin/`)** must remain syntactically and structurally aligned with upstream `doldecomp/melee`. Do not perform cosmetic reformatting or mass renaming here.
- **Platform code (`src/pc/`)** is our domain. It must adhere to modern C11/C++20 standards, clean RAII patterns, strict warnings, and consistent formatting.
- **One-Way Dependencies**: `src/pc` may include headers from `src/melee` and `src/sysdolphin`, but decomp code should only interact with the platform through standard GameCube OS/GX/VI/AX interfaces or minimal, well-documented `pc_*` hooks.

---

## 2. Guiding Principles

1. **Safety Over Cleverness**: Explicit type conversions, bounds checks, and robust error returns prevent hard-to-diagnose memory corruption.
2. **Deterministic & Reproducible**: Avoid unspecified or compiler-dependent behaviors. The game loop must produce identical logic across all supported platforms.
3. **Respect 64-Bit & Endian Constraints**: The GameCube is 32-bit big-endian; modern hosts are 64-bit little-endian. Never mix pointers and 32-bit integers without the proper abstraction.
4. **Document Non-Obvious Porting Decisions**: Always explain *why* a patch was necessary for PC portability (e.g. alignment shifts, hardware registers, audio sample buffers).

---

## 3. Formatting & Tooling

Automated tools ensure consistent formatting and catch bugs before code reaches review.

### Tooling Configuration Files
- **`.editorconfig`**: Root configuration guaranteeing 4-space indentation, UTF-8 character encoding, LF line endings, and trailing whitespace trimming.
- **`.clang-format`**: Project-wide formatting based on C++20 / C11 standards with a 100-column limit and left pointer alignment (`Type* ptr`).
- **`.clang-tidy`**: Static analysis catching narrowing conversions, dangling pointers, redundant expressions, and deprecated patterns.

### Formatting Rules Summary
| Rule | Setting | Example |
|---|---|---|
| **Indentation** | 4 spaces (no tabs) | `    int count = 0;` |
| **Line Length** | 100 characters max | Break long argument lists or conditions cleanly |
| **Pointers & Refs** | Left-aligned | `void* buffer;`, `const std::string& path;` |
| **Control Statements** | Space before paren | `if (condition) {` (not `if(condition){`) |
| **Braces** | Egyptian / attached braces | `void func() {` and `if (x) { ... } else { ... }` |
| **Empty Blocks** | No multi-line sprawl | `{}` |

### Applying Formatting
Format specific files before committing:
```bash
clang-format -i src/pc/my_file.cpp src/pc/my_file.hpp
```
To check all platform files:
```bash
python3 tools/check_style.py
```

---

## 4. Naming Conventions

Consistency in naming communicates intent immediately:

### Platform Layer C (`src/pc/*.c`, `src/pc/*.h`)
- **Functions**: `snake_case` with `pc_` prefix for exported functions:
  ```c
  void pc_audio_init(void);
  void pc_log_line(const char* fmt, ...);
  ```
- **Static / Internal Functions**: `snake_case`:
  ```c
  static void deliver_pending(void);
  ```
- **Global & Static Variables**: Prefixed with `g_` (global) or `s_` (file-static):
  ```c
  static pthread_mutex_t s_intr_mutex;
  static __thread int s_intr_depth;
  ```
- **Types & Structs**: `PascalCase` or `snake_case_t`:
  ```c
  typedef struct PcAudioStream PcAudioStream;
  ```
- **Constants & Macros**: `UPPER_SNAKE_CASE`:
  ```c
  #define MAX_AUDIO_BUFFERS 16
  ```

### Platform Layer C++ (`src/pc/*.cpp`, `src/pc/*.hpp`)
- **Namespaces**: Lowercase `snake_case`:
  ```cpp
  namespace launcher { ... }
  namespace melee::pc { ... }
  ```
- **Classes / Structs**: `PascalCase`:
  ```cpp
  class LauncherWindow;
  struct GamePadMapping;
  ```
- **Methods & Functions**: `snake_case`:
  ```cpp
  bool load_preferences();
  std::string get_disc_label();
  ```
- **Member Variables**: `m_` prefix or trailing underscore:
  ```cpp
  std::string m_file_path;
  int m_render_scale = 0;
  ```
- **Constants**: `kPascalCase` or `UPPER_SNAKE_CASE`:
  ```cpp
  constexpr int kDefaultSampleRate = 32000;
  ```

### Game Decomp Layer (`src/melee/`, `src/sysdolphin/`)
- Preserve existing dolphin/HAL conventions:
  - Functions: `HSD_*`, `lb*`, `ft*`, `ef*`
  - Types: `HSD_JObj`, `Fighter`, `Item`
  - Primitives: `u8`, `u16`, `u32`, `u64`, `s8`, `s16`, `s32`, `s64`, `f32`, `f64`, `BOOL`

---

## 5. Portability & Architecture Rules

Porting a 32-bit big-endian GameCube title to 64-bit little-endian x86-64 and ARM64 hosts introduces specific failure modes. Follow these mandatory rules:

### 1. Integer Types & Width Safety
- **BAN Bare `long` and `unsigned long`**:
  `long` is 32-bit on Windows (LLP64) and 64-bit on Linux/macOS/Android (LP64). Using `long` causes subtle cross-platform memory corruption.
  - In `src/pc`: use `<cstdint>` / `<stdint.h>` (`int32_t`, `uint32_t`, `int64_t`, `uint64_t`, `size_t`, `uintptr_t`).
  - In `src/melee`: use `<dolphin/types.h>` (`s32`, `u32`, `s64`, `u64`).
- **Pointer Math**: Always use `uintptr_t` or `size_t` when casting pointers for arithmetic. Never cast a pointer to `u32` or `uint32_t` directly.

### 2. On-Disc Data Model & Endianness
Disc archives (`.dat`, `.usd`) remain verbatim big-endian in memory.
```c
/* Correct: Mark disc-mapped structures with DISC_STRUCT */
typedef struct DISC_STRUCT {
    DiscU32 flags;
    DiscF32 x, y, z;
    DISC_PTR(char) name;
} StageDiscHeader;

/* Reading pointer slots */
char* name = DP(char, header->name);

/* Writing pointer slots */
DP_SET(header->name, host_name_ptr);

/* Asserting structure size matches GameCube DOL layout */
DISC_ASSERT_SIZE(StageDiscHeader, 0x14);
```
- **Taking addresses of disc scalars**: Taking the address of a member of a `DISC_STRUCT` is a compiler error. Copy to a local variable first, mutate, and copy back.
- **Scalar arrays**: Within disc structs, wrap scalars with `DiscF32`, `DiscU32`, `DiscS16` so GCC's endian attributes swap them on `.v` member access.

### 3. 64-Bit Pointer Shifts in Runtime Structs
- On GameCube, `sizeof(void*) == 4`. On x86-64 / ARM64, `sizeof(void*) == 8`.
- **Never use hardcoded byte offsets**:
  ```c
  /* WRONG: Breaks on 64-bit host */
  u32 val = *(u32*)((u8*)player + 0xD8);

  /* CORRECT: Access named field or use offsetof */
  u32 val = player->flags;
  ```
- **Union Overlays**: If a union overlays a pointer with integer views (such as motion variables `Fighter::mv`), the 64-bit pointer shifts all following union members. Move the pointer out of the union into the enclosing struct if needed, and document the change.

### 4. Cross-Platform OS Services
- **Filesystem**: Use `std::filesystem` in C++ or SDL3 filesystem APIs (`SDL_GetPrefPath`, `SDL_GetBasePath`). Avoid raw POSIX `<sys/stat.h>` or Win32 `GetFileAttributes` outside isolated platform wrappers.
- **Paths**: Use standard `/` forward slashes. Both modern Windows APIs and POSIX accept forward slashes.
- **Concurrency & Threads**: Use C++ `<mutex>` / `<thread>` / `<atomic>` or SDL3 thread primitives. In the C platform layer, synchronize simulated GameCube interrupts using `s_intr_mutex` and `OSDisableInterrupts()` / `OSRestoreInterrupts()`.

---

## 6. Human Readability & Clean Code

### Self-Documenting Code
- Replace magic numbers with named constants:
  ```cpp
  // Bad
  if (scale > 10) scale = 10;

  // Good
  constexpr int kMaxRenderScale = 10;
  if (scale > kMaxRenderScale) scale = kMaxRenderScale;
  ```
- Use `enum class` in C++ for type-safe options:
  ```cpp
  enum class ResamplerMode : uint8_t {
      Bilinear = 0,
      AreaSampling = 1,
      CrtScanlines = 2,
      Vibrant = 3
  };
  ```

### Tagging Porting Divergences
Whenever you modify decompiled game code in `src/melee` to resolve a 64-bit layout shift or PC quirk, tag the line with a clear comment:
```c
/* PORT: 64-bit host fix: pointer widening shifted motion var union.
 * Moved throw_thrower pointer into Fighter struct to preserve member offsets. */
```
This enables maintainers to quickly trace divergences when comparing against upstream `doldecomp/melee`.

---

## 7. Memory Management & Error Handling

### Memory Ownership
- **C++ Layer**: Strictly follow **RAII** (Resource Acquisition Is Initialization).
  - Use `std::unique_ptr` for exclusive ownership.
  - Use `std::shared_ptr` only when multiple owners genuinely exist.
  - Never call `delete` on raw pointers.
- **C Layer**: Explicitly pair every allocation function with its deallocation counterpart:
  - Document ownership in function headers: `/* Returns newly allocated buffer; caller must free with free() */`.
  - Prefer arenas or stack buffers for temporary operations.

### Logging
All diagnostics should pass through `pc_log_line()`:
```c
pc_log_line("[Audio] Initialized SDL3 audio stream: %d Hz, %d channels", freq, channels);
```
- Includes automatic millisecond timestamps from program startup.
- Automatically mirrors to `stderr` and the persistent log file (`MELEE_LOG_FILE` / `melee-pc.log` on Windows).
- Never use raw `printf` or `std::cout` in production code.

### Assertions
- Use `DISC_ASSERT_SIZE(Type, expected_size)` to verify struct packing at compile time.
- Use `assert()` or `HSD_ASSERT()` for runtime invariants that must never fail in debug builds.
- Runtime assertions must not contain side effects (e.g. `assert(init() == 0)` is invalid; assign to a status variable first).

---

## 8. Verification & CI Workflow

Before submitting changes, run the local verification suite:

```bash
# 1. Check style and formatting
python3 tools/check_style.py

# 2. Syntax-check all game translation units
python3 tools/compile_check.py

# 3. Check struct size and offset ABI matches against GameCube
python3 tools/lint_sweep.py build

# 4. Build and run unit test targets
ninja -C build launcher_data_test version_test
./build/launcher_data_test
./build/version_test
```

Continuous Integration automatically validates these checks on every Pull Request and tag commit across Linux, Windows (MinGW cross-compilation), and Android.
