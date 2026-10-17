// Interface strings in 17 languages. The tables come from src/lang/<code>.txt through tools/gen_lang.py
// (lang_table.inc); the order follows enum Str in c2s.h. The language is the Windows display language unless the
// user picks another one (Options > Language), otherwise English.
#include "c2s.h"
#include "lang_table.inc"

static_assert(sizeof(kStrings[0]) / sizeof(kStrings[0][0]) == S__COUNT, "lang_table.inc does not match enum Str: run tools/gen_lang.py");

// Primary Windows language id of each table (same order as kLangNames).
static const WORD kPrimary[kLangCount] = {
    LANG_ENGLISH, LANG_RUSSIAN, LANG_UKRAINIAN, LANG_BELARUSIAN, LANG_GERMAN, LANG_FRENCH, LANG_SPANISH, LANG_ITALIAN,
    LANG_PORTUGUESE, LANG_POLISH, LANG_DUTCH, LANG_TURKISH, LANG_INDONESIAN, LANG_VIETNAMESE, LANG_CHINESE,
    LANG_JAPANESE, LANG_KOREAN
};

static int g_lang = 0;           // index in the tables

int LangCount() { return kLangCount; }

const wchar_t* LangName(int index)
{
    return (index >= 0 && index < kLangCount) ? kLangNames[index] : L"";
}

static int IndexForId(LANGID id)
{
    WORD primary = PRIMARYLANGID(id);
    for (int i = 0; i < kLangCount; ++i)
        if (kPrimary[i] == primary) return i;
    return -1;
}

int LangAutoIndex()
{
    int i = IndexForId(GetUserDefaultUILanguage());       // the display language, then the regional settings
    if (i < 0) i = IndexForId(GetUserDefaultLangID());
    return i < 0 ? 0 : i;
}

void LangSelect(int setting)
{
    if (setting >= 1 && setting <= kLangCount) g_lang = setting - 1;
    else g_lang = LangAutoIndex();
}

const wchar_t* T(Str s)
{
    if ((unsigned)s >= (unsigned)S__COUNT) return L"";
    const wchar_t* t = kStrings[g_lang][s];
    return (t && *t) ? t : kStrings[0][s];
}
