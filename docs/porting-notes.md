# Porting notes

The data model and the bug classes that come with it. Coding rules that follow
from these live in [CODING_STYLE.md](../CODING_STYLE.md); the harnesses that
find the next batch are in [testing.md](testing.md#harnesses-for-port-bugs).

## Data model

Disc data stays big-endian in memory and is described with `DISC_STRUCT` and
`DISC_PTR` (see `src/pc/disc.h`). Structs mapping archive contents are byte-swapped
on access by GCC, disc pointers are 32-bit slots relocated to host addresses, and
MEM1 is mapped at `0x80000000` so those slots always fit. The whole 4 GB range is
game-addressable through `-no-pie` with text at `0x10000000` (the memory map is
in [architecture.md](architecture.md#memory-map)).

## Bug classes

Bug classes that keep coming back when bringing up a new scene:

- Runtime structs have 8-byte pointers, so any hard-coded GameCube offset
  (`(u8*)gp + 0xD8`, `memzero(p, 0x74)`, padded overlay structs) has to become a
  real field or `sizeof`.
- Statics are not adjacent on x86-64. A cast to a bigger struct to reach the next
  static must name the neighbour instead.
- Bitfields are LSB-first. Unions overlaying bitfields with an integer view need
  `DISC_STRUCT` on the union and every nested struct.
- `UNK_T` is `void*`, so unnamed words are 8 bytes. Inside a union view that is a
  layout change; retype numeric ones `u32`.
- Motion-variable unions (`Fighter::mv`) have the same problem one level down: the
  game writes one view and reads another, so a pointer inside a view shifts every
  member below it. Where no position survives, move the field into `Fighter`.
- Two struct views over the same disc blob must both carry `DISC_STRUCT`. A
  native view beside a `DISC_STRUCT` one reads every field byte-swapped; a small
  int coming back as `0x??000000` is the tell.
- Retail `GXEnd` is empty and the decomp omits it, but aurora's `GXEnd` submits
  the draw. Every `GXBegin` needs one.
- `Mtx` is 48 bytes and `Mtx44` is 64; `MTXOrtho` and `MTXPerspective` take `Mtx44`.
- `bool` in a decomp signature usually means "int the decompiler could not name".
  Under `_Bool` every value above 1 clamps, so an index or scene id silently
  becomes 1.
- Non-void functions that fall off the end returned whatever the last call left
  in `r3`/`f1` on PowerPC; on x86-64 that register holds garbage. Compile for
  real with `-Wreturn-type` (it is not emitted under `-fsyntax-only`).
- Walking an object as `void**` and indexing by GameCube word number scales by 8,
  so `p[0x14]` for byte 0x50 lands at byte 160. Index by name.
- Japanese string literals are Shift-JIS at runtime; the build passes
  `-fexec-charset=CP932`.
- Plain `char` is signed on GameCube (mwcc defaults to `-char signed`) and on
  x86-64, but unsigned on AArch64 Linux and Android, and every ARM target
  (Android, iOS, Windows ARM64) builds the game with `aarch64-linux-gnu-gcc`.
  The build passes `-fsigned-char` to all first-party targets; without it
  `char x = -1; if (x < 5)` (ifPrize's intro counter) never counts on ARM.

Tag every such divergence from upstream with a `/* PORT: ... */` comment
(see [CODING_STYLE.md](../CODING_STYLE.md#tagging-porting-divergences)).

## Regions

**Melee USA revision 2 (NTSC-U 1.02, GALE01)** is the supported disc. A
**Europe (PAL, GALP01)** image also boots, experimentally: the game code is
still the USA build, the DVD layer serves the English (UK) `.ukd` text files
where the code asks for `.usd`, and the USA-only trophy tables missing from
`TyDatai` get empty stand-ins (`src/pc/region.c`). Gameplay is therefore
NTSC (60 Hz) on PAL data.
