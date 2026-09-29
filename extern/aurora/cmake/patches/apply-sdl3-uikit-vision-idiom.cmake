# SDL's UIKit form-factor switch names UIUserInterfaceIdiomVision unguarded.
# That constant only exists in the iOS 17 SDK, and the only freely available
# SDK trees (theos/sdks) stop at 16.5, so an iOS build against them fails with
# "use of undeclared identifier 'UIUserInterfaceIdiomVision'" and a bogus
# "duplicate case value" for Phone. The case is unreachable on a 16.5 SDK
# anyway, so guard it on the SDK that defines it.
if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_uikit "${SDL_SOURCE_DIR}/src/video/uikit/SDL_uikitvideo.m")
if (NOT EXISTS "${_uikit}")
  message(FATAL_ERROR "SDL UIKit video source is missing: ${_uikit}")
endif ()

file(READ "${_uikit}" _source)

set(_old "    case UIUserInterfaceIdiomVision:
        return SDL_FORMFACTOR_HEADSET;
")

set(_new "    // guard: iOS 17 SDK only (see apply-sdl3-uikit-vision-idiom.cmake)
#if defined(__IPHONE_17_0) && __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_17_0
    case UIUserInterfaceIdiomVision:
        return SDL_FORMFACTOR_HEADSET;
#endif
")

string(FIND "${_source}" "apply-sdl3-uikit-vision-idiom" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 UIKit vision idiom patch already applied")
  return()
endif ()

string(FIND "${_source}" "${_old}" _patch_site)
if (_patch_site EQUAL -1)
  message(FATAL_ERROR "Failed to apply SDL3 UIKit vision idiom patch: expected switch case not found")
endif ()

string(REPLACE "${_old}" "${_new}" _patched_source "${_source}")
file(WRITE "${_uikit}" "${_patched_source}")
message(STATUS "Applied SDL3 UIKit vision idiom patch")
