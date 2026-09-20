"""Compiler include paths shared by standalone netplay fixtures."""
import os
from pathlib import Path


def sdl_includes(root):
    build = Path(os.environ.get("MELEE_TEST_BUILD", root / "build"))
    candidates = [build / "_deps/sdl-src/include", Path("/usr/include/SDL3").parent]
    cache = build / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(("SDL3_SOURCE_DIR:", "FETCHCONTENT_SOURCE_DIR_SDL:")):
                value = line.split("=", 1)[1]
                if value:
                    candidates.insert(0, Path(value) / "include")
    include = next((p for p in candidates if (p / "SDL3/SDL.h").exists()), None)
    if include is None:
        raise RuntimeError("Configure the build first, or set MELEE_TEST_BUILD to its directory")
    return [build / "_deps/sdl-build/include-revision", include]


def standalone_compile_args(entry):
    """Reuse compiler flags without overwriting the build's outputs/depfiles."""
    import shlex
    original = entry.get("arguments") or shlex.split(entry["command"])
    result = []
    index = 0
    while index < len(original):
        arg = original[index]
        if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
            index += 2
            continue
        if (arg not in ("-MD", "-MMD", "-MP", "-DNDEBUG", entry["file"])
                and not arg.startswith(("-MF", "-MT", "-MQ"))):
            result.append(arg)
        index += 1
    return result
