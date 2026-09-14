# Licensing

This repository contains code under three different situations. Read all
three before redistributing any part of it.

## 1. Game code — NOT licensed

`src/melee/` and `src/sysdolphin/` are a decompilation of Super Smash Bros.
Melee, derived from [doldecomp/melee](https://github.com/doldecomp/melee) and
adapted here to a 64-bit little-endian data model.

Super Smash Bros. Melee is copyright Nintendo / HAL Laboratory. The upstream
decompilation project publishes no license, and neither this repository nor
its authors can grant one. **No permission to copy, modify or redistribute
this code is offered or implied.**

The GPL in `LICENSE` does not, and cannot, apply to this code. Because these
files cannot be relicensed, the repository as a whole is not distributable
under the GPL — only the port code in section 2 is.

No game assets are contained in this repository. Textures, audio, models,
text, and the two HSD font atlases are all read at runtime from a disc image
the user supplies; see `src/pc/discfont.c`.

## 2. Port code — GPL-3.0-or-later

`src/pc/`, `tools/`, `platforms/`, `cmake/` and `.github/` are original work
for this port.

    Copyright (C) 2026 the melee-pc contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

The full text is in [`LICENSE`](LICENSE).

## 3. Third-party components

- `extern/aurora/` — aurora, MIT. See `extern/aurora/LICENSE`.
- `resources/font.ttf`, `resources/font-bold.ttf` — Liberation Sans, SIL Open
  Font License. See `resources/FONT-LICENSE.txt`.
- `platforms/android/app/src/main/java/org/libsdl/` — SDL3, zlib license.
- Dawn, SDL3, nod, RmlUi and the other dependencies are fetched at build time
  under their own licenses.

MIT, zlib and the SIL OFL are all GPL-3.0 compatible, so combining them with
the port code in section 2 is fine.
