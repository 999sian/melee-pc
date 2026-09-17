#include "lblanguage.h"

#include <melee/gm/gmmain_lib.h>
#include <melee/gm/types.h>
#ifdef TARGET_PC
#include "pc/region.h"
#endif

/* A PAL disc has no Japanese assets, so the language the save file carries
 * (a USA-only option, and JP on cards written before PAL was handled) must
 * read as US there. */
static inline enum_t saved_language(void)
{
#ifdef TARGET_PC
    if (pc_region_is_pal()) {
        return LANG_US;
    }
#endif
    return gmMainLib_GetGamePrefs()->saved_language;
}

enum_t lbLang_GetLanguageSetting(void)
{
    return gmMainLib_804D3EE0->language;
}

enum_t lbLang_SetLanguageSetting(enum_t language)
{
    if (language >= 0 && language < LANG_COUNT) {
        gmMainLib_804D3EE0->language = language;
    }

    return language;
}

bool lbLang_IsSettingJP(void)
{
    return gmMainLib_804D3EE0->language == LANG_JP ? true : false;
}

bool lbLang_IsSettingUS(void)
{
    return gmMainLib_804D3EE0->language == LANG_US ? true : false;
}

enum_t lbLang_GetSavedLanguage(void)
{
    return saved_language();
}

void lbLang_SetSavedLanguage(enum_t language)
{
    if (language >= 0 && language < LANG_COUNT) {
        gmMainLib_GetGamePrefs()->saved_language = language;
    }
}

bool lbLang_IsSavedLanguageJP(void)
{
    return saved_language() == LANG_JP ? true : false;
}

bool lbLang_IsSavedLanguageUS(void)
{
    return saved_language() == LANG_US ? true : false;
}
