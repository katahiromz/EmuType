// emutype.cpp --- Emulate the Windows font engine
// Author: katahiromz
// License: MIT
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <strsafe.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CACHE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include FT_WINFONTS_H
#include FT_LCD_FILTER_H

#include "SaveBitmapToFile.h"
#include "nearly_equal_bitmap.h"

#define MAKE_SURROGATE_PAIR(w1, w2) \
    (0x10000 + (((DWORD)(w1) - HIGH_SURROGATE_START) << 10) + ((DWORD)(w2) - LOW_SURROGATE_START));

#define _TMPF_VARIABLE_PITCH TMPF_FIXED_PITCH // TMPF_FIXED_PITCH is a brain-dead API

const int WIDTH  = 300;
const int HEIGHT = 100;
const COLORREF BG = RGB(255, 255, 0);
const COLORREF FG = RGB(0,   0,   0);
PCWSTR FONT_NAME = L"Tahoma";
const LONG FONT_SIZE = 30;
const WCHAR* text = L"EmuType Draw";
const COLORREF color1 = RGB(0, 0, 0);
const COLORREF color2 = RGB(255, 255, 0);

// Using a different registry key for this test.
const WCHAR* reg_key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontsEmulated";

/*
 *  For TranslateCharsetInfo
 */
#define CP_SYMBOL   42
#define MAXTCIINDEX 32
static const CHARSETINFO g_FontTci[MAXTCIINDEX] =
{
    /* ANSI */
    { ANSI_CHARSET, 1252, {{0,0,0,0},{FS_LATIN1,0}} },
    { EASTEUROPE_CHARSET, 1250, {{0,0,0,0},{FS_LATIN2,0}} },
    { RUSSIAN_CHARSET, 1251, {{0,0,0,0},{FS_CYRILLIC,0}} },
    { GREEK_CHARSET, 1253, {{0,0,0,0},{FS_GREEK,0}} },
    { TURKISH_CHARSET, 1254, {{0,0,0,0},{FS_TURKISH,0}} },
    { HEBREW_CHARSET, 1255, {{0,0,0,0},{FS_HEBREW,0}} },
    { ARABIC_CHARSET, 1256, {{0,0,0,0},{FS_ARABIC,0}} },
    { BALTIC_CHARSET, 1257, {{0,0,0,0},{FS_BALTIC,0}} },
    { VIETNAMESE_CHARSET, 1258, {{0,0,0,0},{FS_VIETNAMESE,0}} },
    /* reserved by ANSI */
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    /* ANSI and OEM */
    { THAI_CHARSET, 874, {{0,0,0,0},{FS_THAI,0}} },
    { SHIFTJIS_CHARSET, 932, {{0,0,0,0},{FS_JISJAPAN,0}} },
    { GB2312_CHARSET, 936, {{0,0,0,0},{FS_CHINESESIMP,0}} },
    { HANGEUL_CHARSET, 949, {{0,0,0,0},{FS_WANSUNG,0}} },
    { CHINESEBIG5_CHARSET, 950, {{0,0,0,0},{FS_CHINESETRAD,0}} },
    { JOHAB_CHARSET, 1361, {{0,0,0,0},{FS_JOHAB,0}} },
    /* Reserved for alternate ANSI and OEM */
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    /* Reserved for system */
    { DEFAULT_CHARSET, 0, {{0,0,0,0},{FS_LATIN1,0}} },
    { SYMBOL_CHARSET, CP_SYMBOL, {{0,0,0,0},{FS_SYMBOL,0}} }
};

struct FontInfo {
    WCHAR wide_path[MAX_PATH];
    CHAR ansi_path[MAX_PATH];
    FT_Long face_index;
    std::wstring family_name;
    std::wstring english_name;
    FT_Long style_flags; // See FT_Face.style_flags
    BYTE charset;
    INT raster_height;
    INT raster_internal_leading;
};
std::vector<FontInfo*> registered_fonts;

WCHAR fonts_dir[MAX_PATH];
FT_Library library;
FTC_Manager cache_manager;

HBITMAP hbmMask_cache = NULL;
int hbmMask_cache_w = 0, hbmMask_cache_h = 0;

HRESULT
_StringCchWideFromAnsi(UINT codepage, PWSTR wide, INT cchWide, PCSTR ansi)
{
    if (!wide || cchWide <= 0)
        return E_INVALIDARG;

    MultiByteToWideChar(codepage, 0, ansi, -1, wide, cchWide);
    wide[cchWide - 1] = UNICODE_NULL;
    return S_OK;
}

HRESULT
_StringCchAnsiFromWide(UINT codepage, PSTR ansi, INT cchAnsi, PCWSTR wide)
{
    if (!ansi || cchAnsi <= 0)
        return E_INVALIDARG;

    WideCharToMultiByte(codepage, 0, wide, -1, ansi, cchAnsi, NULL, NULL);
    ansi[cchAnsi - 1] = ANSI_NULL;
    return S_OK;
}


// Helper to determine font type
static bool is_raster_font(const std::wstring& path)
{
    LPCWSTR ext = PathFindExtensionW(path.c_str());
    return lstrcmpiW(ext, L".fon") == 0 || lstrcmpiW(ext, L".fnt") == 0;
}

bool is_supported_font(const WCHAR *filename) {
    LPCWSTR pchDotExt = PathFindExtensionW(filename);
    return lstrcmpiW(pchDotExt, L".ttf") == 0 ||
           lstrcmpiW(pchDotExt, L".ttc") == 0 ||
           lstrcmpiW(pchDotExt, L".otf") == 0 ||
           lstrcmpiW(pchDotExt, L".otc") == 0 ||
           lstrcmpiW(pchDotExt, L".fon") == 0 ||
           lstrcmpiW(pchDotExt, L".fnt") == 0;
}

// ---------------------------------------------------------------------------
// VDMX (Vertical Device Metrics) table support
// Mirrors Wine's load_VDMX() in dlls/win32u/freetype.c
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct VDMX_Header {
    WORD version;
    WORD numRecs;
    WORD numRatios;
};
struct VDMX_Ratio {
    BYTE bCharSet;
    BYTE xRatio;
    BYTE yStartRatio;
    BYTE yEndRatio;
};
struct VDMX_Group {
    WORD recs;
    BYTE startsz;
    BYTE endsz;
};
struct VDMX_vTable {
    WORD yPelHeight;
    WORD yMax;        // signed SHORT stored as WORD (big-endian)
    WORD yMin;        // signed SHORT stored as WORD (big-endian)
};
#pragma pack(pop)

static inline WORD be16(WORD x) {
    return (WORD)(((x & 0xFF) << 8) | ((x >> 8) & 0xFF));
}

// Result of a successful VDMX lookup
struct VdmxEntry {
    int  ppem;
    int  yMax;   // pixel ascent  (positive, from baseline upward)
    int  yMin;   // pixel descent (negative, from baseline downward)
};

// Load and search the VDMX table embedded in an SFNT font.
// lfHeight > 0 : search for the entry where yMax + (-yMin) == lfHeight
//                (Windows "cell height" semantics)
// lfHeight < 0 : search for the entry where yPelHeight == |lfHeight|
//                (Windows "em height" / ppem semantics)
// Returns true and fills *out on success.
static bool load_VDMX(FT_Face face, int lfHeight, VdmxEntry* out)
{
    if (!FT_IS_SFNT(face)) return false;

    const FT_ULong VDMX_TAG = FT_MAKE_TAG('V','D','M','X');

    // Query table size
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(face, VDMX_TAG, 0, NULL, &len) != 0 || len == 0)
        return false;

    std::vector<BYTE> buf(len);
    if (FT_Load_Sfnt_Table(face, VDMX_TAG, 0, buf.data(), &len) != 0)
        return false;

    if (len < sizeof(VDMX_Header)) return false;

    const BYTE* p = buf.data();
    VDMX_Header hdr;
    memcpy(&hdr, p, sizeof(hdr));
    WORD numRatios = be16(hdr.numRatios);
    WORD numRecs   = be16(hdr.numRecs);

    // Find a matching ratio record.
    // We use device ratio 1:1 (same as Wine's fixed devXRatio/devYRatio = 1).
    // A record is acceptable when:
    //   (xRatio == 0 && yStartRatio == 0 && yEndRatio == 0)  [catch-all]
    //   OR (xRatio == 1 && yStartRatio <= 1 <= yEndRatio)
    FT_ULong group_offset_file = (FT_ULong)-1;
    const FT_ULong ratios_base = sizeof(VDMX_Header);
    const FT_ULong offsets_base = ratios_base + (FT_ULong)numRatios * sizeof(VDMX_Ratio);

    for (WORD i = 0; i < numRatios; ++i)
    {
        FT_ULong roff = ratios_base + (FT_ULong)i * sizeof(VDMX_Ratio);
        if (roff + sizeof(VDMX_Ratio) > len) break;

        VDMX_Ratio ratio;
        memcpy(&ratio, p + roff, sizeof(ratio));

        if (!ratio.bCharSet) continue;   // skip records with bCharSet == 0

        bool match = (ratio.xRatio == 0 && ratio.yStartRatio == 0 && ratio.yEndRatio == 0)
                  || (ratio.xRatio == 1 && ratio.yStartRatio <= 1 && 1 <= ratio.yEndRatio);
        if (!match) continue;

        FT_ULong ooff = offsets_base + (FT_ULong)i * sizeof(WORD);
        if (ooff + sizeof(WORD) > len) break;

        WORD go;
        memcpy(&go, p + ooff, sizeof(go));
        group_offset_file = be16(go);
        break;
    }

    if (group_offset_file == (FT_ULong)-1 || group_offset_file + sizeof(VDMX_Group) > len)
        return false;

    VDMX_Group group;
    memcpy(&group, p + group_offset_file, sizeof(group));
    WORD recs    = be16(group.recs);
    BYTE startsz = group.startsz;
    BYTE endsz   = group.endsz;

    FT_ULong vtable_off = group_offset_file + sizeof(VDMX_Group);
    if (vtable_off + (FT_ULong)recs * sizeof(VDMX_vTable) > len)
        return false;

    const VDMX_vTable* vt = reinterpret_cast<const VDMX_vTable*>(p + vtable_off);

    if (lfHeight > 0)
    {
        // Cell-height mode: find entry where yMax + (-yMin) == lfHeight.
        // Iterate in ascending ppem order; if we overshoot use the previous.
        int prev_ppem = 0, prev_yMax = 0, prev_yMin = 0;
        for (WORD i = 0; i < recs; ++i)
        {
            int ppem = (int)be16(vt[i].yPelHeight);
            int yMax = (int)(SHORT)be16(vt[i].yMax);
            int yMin = (int)(SHORT)be16(vt[i].yMin);
            int cell = yMax + (-yMin);

            if (cell == lfHeight)
            {
                out->ppem = ppem;
                out->yMax = yMax;
                out->yMin = yMin;
                return true;
            }
            if (cell > lfHeight)
            {
                if (prev_ppem == 0) return false;
                out->ppem = prev_ppem;
                out->yMax = prev_yMax;
                out->yMin = prev_yMin;
                return true;
            }
            prev_ppem = ppem;
            prev_yMax = yMax;
            prev_yMin = yMin;
        }
        return false;   // no entry found within range
    }
    else
    {
        // Em-height / ppem mode: |lfHeight| must be in [startsz, endsz]
        int target = -lfHeight;
        if (target < startsz || target > endsz) return false;

        for (WORD i = 0; i < recs; ++i)
        {
            int ppem = (int)be16(vt[i].yPelHeight);
            if (ppem > target) return false;   // past our target
            if (ppem == target)
            {
                out->ppem = ppem;
                out->yMax = (int)(SHORT)be16(vt[i].yMax);
                out->yMin = (int)(SHORT)be16(vt[i].yMin);
                return true;
            }
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// calc_ppem_for_height  (Wine-compatible)
//
// Converts a LOGFONT lfHeight value to a FreeType ppem value.
//
//   lfHeight > 0  Å® cell height  Å®  ppem = units_per_EM * lfHeight / (winAscent+winDescent)
//   lfHeight < 0  Å® em   height  Å®  ppem = |lfHeight|
//   lfHeight == 0 Å® default 16px em
//
// Uses usWinAscent/usWinDescent from OS/2 table exactly as Windows does.
// Falls back to hhea Ascender/Descender when the OS/2 values sum to zero.
// ---------------------------------------------------------------------------
static int calc_ppem_for_height(FT_Face face, int lfHeight)
{
    if (lfHeight == 0) lfHeight = -16;   // Windows default

    if (lfHeight < 0)
        return -lfHeight;   // em-height: ppem == |lfHeight|

    // Cell-height path: derive ppem from usWinAscent + usWinDescent
    TT_OS2*        pOS2  = (TT_OS2*)       FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    TT_HoriHeader* pHori = (TT_HoriHeader*)FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);

    int units;
    if (pOS2)
    {
        // Some broken fonts store a huge negative value in usWinDescent
        // (signed overflow). Take the absolute value to compensate.
        int winAscent  = (int)pOS2->usWinAscent;
        int winDescent = (int)abs((SHORT)pOS2->usWinDescent);

        if (winAscent + winDescent != 0)
            units = winAscent + winDescent;
        else if (pHori)
            units = (int)pHori->Ascender - (int)pHori->Descender;
        else
            units = (int)face->units_per_EM;
    }
    else if (pHori)
        units = (int)pHori->Ascender - (int)pHori->Descender;
    else
        units = (int)face->units_per_EM;

    // ppem = units_per_EM * lfHeight / units
    int ppem = (int)FT_MulDiv((FT_Long)face->units_per_EM, (FT_Long)lfHeight, (FT_Long)units);

    // If rounding caused us to exceed the requested height, step down by one
    if (ppem > 1 && (int)FT_MulDiv((FT_Long)units, (FT_Long)ppem, (FT_Long)face->units_per_EM) > lfHeight)
        --ppem;

    return (ppem > 0) ? ppem : 1;
}

FT_Error
requester(FTC_FaceID  face_id,
          FT_Library  lib,
          FT_Pointer  req_data,
          FT_Face*    aface)
{
    FontInfo* info = static_cast<FontInfo*>(face_id);
    return FT_New_Face(lib, info->ansi_path, info->face_index, aface);
}

static std::wstring get_family_name(FT_Face face, FT_UShort name_id, bool localized, PCWSTR default_value)
{
    if (!FT_IS_SFNT(face))
        return default_value;

    FT_UInt count = FT_Get_Sfnt_Name_Count(face);

    int best_priority = -1;
    std::wstring best_name;

    for (FT_UInt i = 0; i < count; ++i)
    {
        FT_SfntName sname;
        if (FT_Get_Sfnt_Name(face, i, &sname) != 0)
            continue;
        if (sname.name_id != name_id)
            continue;

        int priority = -1;
        std::wstring name;

        if (sname.platform_id == TT_PLATFORM_MICROSOFT &&
            sname.encoding_id == TT_MS_ID_UNICODE_CS)
        {
            int wlen = (int)sname.string_len / 2;
            std::vector<wchar_t> wbuf(wlen + 1, 0);
            for (int j = 0; j < wlen; ++j) {
                wbuf[j] = (wchar_t)((sname.string[j * 2] << 8) | sname.string[j * 2 + 1]);
            }
            name = std::wstring(wbuf.data(), wlen);

            if (sname.language_id == GetUserDefaultLangID() && localized)
                priority = 3;
            else if (sname.language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES)
                priority = 2;
            else
                priority = 1;
        }
        else if (sname.platform_id == TT_PLATFORM_MACINTOSH &&
                 sname.encoding_id == TT_MAC_ID_ROMAN)
        {
            // Mac Roman: usable as-is within the ASCII range
            std::string aname = std::string(reinterpret_cast<const char*>(sname.string), sname.string_len);
            WCHAR szWide[MAX_PATH];
            _StringCchWideFromAnsi(CP_ACP, szWide, _countof(szWide), aname.c_str());
            name = szWide;
            priority = 0;
        }

        if (priority > best_priority)
        {
            best_priority = priority;
            best_name = name;
        }
    }

    if (!best_name.empty())
        return best_name;

    return default_value;
}

static std::string get_style_name(FT_Face face, bool localized)
{
    if (!FT_IS_SFNT(face))
        return face->style_name ? face->style_name : "";

    FT_UInt count = FT_Get_Sfnt_Name_Count(face);

    int best_priority = -1;
    std::string best_name;

    for (FT_UInt i = 0; i < count; ++i)
    {
        FT_SfntName sname;
        if (FT_Get_Sfnt_Name(face, i, &sname) != 0)
            continue;

        // TT_NAME_ID_FONT_SUBFAMILY refers to style names such as "Regular" or "Bold"
        if (sname.name_id != TT_NAME_ID_FONT_SUBFAMILY)
            continue;

        int priority = -1;
        std::string name;

        if (sname.platform_id == TT_PLATFORM_MICROSOFT &&
            sname.encoding_id == TT_MS_ID_UNICODE_CS)
        {
            // Convert from UTF-16BE to UTF-8
            int wlen = static_cast<int>(sname.string_len) / 2;
            std::vector<wchar_t> wbuf(wlen + 1, 0);
            for (int j = 0; j < wlen; ++j)
            {
                wbuf[j] = static_cast<wchar_t>(
                    (sname.string[j * 2] << 8) | sname.string[j * 2 + 1]);
            }
            int mblen = WideCharToMultiByte(CP_UTF8, 0, &wbuf[0], wlen, NULL, 0, NULL, NULL);
            if (mblen > 0)
            {
                std::string mbstr(mblen, '\0');
                WideCharToMultiByte(CP_UTF8, 0, &wbuf[0], wlen, &mbstr[0], mblen, NULL, NULL);
                name = mbstr;
            }

            if (sname.language_id == GetUserDefaultLangID() && localized)
                priority = 3;
            else if (sname.language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES)
                priority = 2;
            else
                priority = 1;
        }
        else if (sname.platform_id == TT_PLATFORM_MACINTOSH &&
                 sname.encoding_id == TT_MAC_ID_ROMAN)
        {
            name = std::string(reinterpret_cast<const char*>(sname.string), sname.string_len);
            priority = 0;
        }

        if (priority > best_priority)
        {
            best_priority = priority;
            best_name = name;
        }
    }

    if (!best_name.empty())
        return best_name;

    // Fallback: use the style name held by FreeType by default
    return face->style_name ? face->style_name : "";
}

TEXTMETRICW* get_raster_text_metrics(FT_Face face, FontInfo* info) {
    FT_WinFNT_HeaderRec WinFNT;
    if (FT_Get_WinFNT_Header(face, &WinFNT) != 0)
        return NULL;

    TEXTMETRICW* ptm = (TEXTMETRICW*)calloc(1, sizeof(TEXTMETRICW));
    if (!ptm)
        return NULL;

    ptm->tmHeight           = WinFNT.pixel_height;
    ptm->tmAscent           = WinFNT.ascent;
    ptm->tmDescent          = ptm->tmHeight - ptm->tmAscent;
    ptm->tmInternalLeading  = WinFNT.internal_leading;
    ptm->tmExternalLeading  = WinFNT.external_leading;
    ptm->tmAveCharWidth     = WinFNT.avg_width;
    ptm->tmMaxCharWidth     = WinFNT.max_width;
    ptm->tmWeight           = WinFNT.weight;
    ptm->tmItalic           = WinFNT.italic;
    ptm->tmUnderlined       = WinFNT.underline;
    ptm->tmStruckOut        = WinFNT.strike_out;
    ptm->tmFirstChar        = WinFNT.first_char;
    ptm->tmLastChar         = WinFNT.last_char;
    ptm->tmDefaultChar      = WinFNT.default_char;
    ptm->tmBreakChar        = WinFNT.break_char;
    ptm->tmCharSet          = WinFNT.charset;
    ptm->tmPitchAndFamily   = WinFNT.pitch_and_family;
    return ptm;
}

// Convert a UTF-8 string to std::wstring (UTF-16)
static std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return L"";
    std::vector<wchar_t> buf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf.data(), wlen);
    return std::wstring(buf.data());
}

static void copy_str_to_otm(const std::wstring& src, PSTR& dest_member, BYTE*& pStrBase, BYTE* pBaseAddr) {
    size_t bsize = (src.length() + 1) * sizeof(WCHAR);
    memcpy(pStrBase, src.c_str(), bsize);
    dest_member = (PSTR)(pStrBase - (BYTE*)0);
    pStrBase += bsize; // Advance write position to next slot
}

OUTLINETEXTMETRICW* get_outline_text_metrics(FT_Face face, BYTE charset) {
    if (!FT_IS_SFNT(face))
        return NULL;

    WCHAR szFamilyName[MAX_PATH];
    _StringCchWideFromAnsi(CP_ACP, szFamilyName, _countof(szFamilyName), face->family_name);
    WCHAR szStyleName[MAX_PATH];
    _StringCchWideFromAnsi(CP_ACP, szStyleName, _countof(szStyleName), face->style_name);

    std::wstring wFamily = get_family_name(face, TT_NAME_ID_FONT_FAMILY, true, szFamilyName);
    std::wstring wStyle  = get_family_name(face, TT_NAME_ID_FONT_SUBFAMILY, true, szStyleName);
    std::wstring wFace   = wFamily + (wStyle.size() ? L"" : L" " + wStyle);
    std::wstring wFull = get_family_name(face, TT_NAME_ID_FULL_NAME, true, szFamilyName);

    size_t strings_size = (wFamily.length() + 1 + wFace.length() + 1 +
                           wStyle.length() + 1 + wFull.length() + 1) * sizeof(WCHAR);
    size_t total_size = sizeof(OUTLINETEXTMETRICW) + strings_size;

    POUTLINETEXTMETRICW potm = (OUTLINETEXTMETRICW*)calloc(1, total_size);
    if (!potm)
        return NULL;
    potm->otmSize = (UINT)total_size;

    // Retrieve tables
    TT_OS2* pOS2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    TT_Postscript* post = (TT_Postscript*)FT_Get_Sfnt_Table(face, FT_SFNT_POST);

    // --- Scaling macros ---
    // GDI's OUTLINETEXTMETRIC returns pixel values based on the current device context resolution.
    FT_Fixed x_scale = face->size->metrics.x_scale;
    FT_Fixed y_scale = face->size->metrics.y_scale;
    auto ScaleX = [&](FT_Short v) { return (LONG)((FT_MulFix(v, x_scale) + 32) >> 6); };
    auto ScaleY = [&](FT_Short v) { return (LONG)((FT_MulFix(v, y_scale) + 32) >> 6); };

    // --- 1. TEXTMETRICW (device-dependent pixel values) ---
    TEXTMETRICW* tm = &potm->otmTextMetrics;
    tm->tmAscent  = (LONG)((face->size->metrics.ascender + 32) >> 6);
    tm->tmDescent = (LONG)((-face->size->metrics.descender + 32) >> 6);
    tm->tmHeight  = tm->tmAscent + tm->tmDescent;
    tm->tmInternalLeading = tm->tmHeight - ScaleY(face->units_per_EM);
    if (tm->tmInternalLeading < 0) tm->tmInternalLeading = 0;
    tm->tmExternalLeading = (LONG)((face->size->metrics.height + 32) >> 6) - tm->tmHeight;
    
    // Average character width calculation (use xAvgCharWidth from OS/2 table if available)
    tm->tmAveCharWidth = (pOS2) ? ScaleX(pOS2->xAvgCharWidth) : (LONG)((face->size->metrics.max_advance + 32) >> 12); // simplified calculation
    tm->tmMaxCharWidth = (LONG)((face->size->metrics.max_advance + 32) >> 6);

    tm->tmWeight    = (pOS2) ? pOS2->usWeightClass : ((face->style_flags & FT_STYLE_FLAG_BOLD) ? FW_BOLD : FW_NORMAL);
    tm->tmItalic    = (face->style_flags & FT_STYLE_FLAG_ITALIC) ? 1 : 0;
    tm->tmUnderlined = (pOS2 && (pOS2->fsSelection & 0x80)) ? 1 : 0;
    tm->tmStruckOut  = (pOS2 && (pOS2->fsSelection & 0x10)) ? 1 : 0;
    tm->tmPitchAndFamily = (face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) ? 0 : (_TMPF_VARIABLE_PITCH | TMPF_TRUETYPE | TMPF_VECTOR);
    tm->tmCharSet = charset;

    // --- 2. OUTLINETEXTMETRICW (detailed metrics) ---
    potm->otmEMSquare = face->units_per_EM;
    potm->otmfsSelection = (pOS2) ? pOS2->fsSelection : 0;
    potm->otmfsType = (pOS2) ? pOS2->fsType : 0;

    // Underline and strikeout (in pixels, not EM units)
    if (pOS2) {
        potm->otmsStrikeoutSize = ScaleY(pOS2->yStrikeoutSize);
        potm->otmsStrikeoutPosition = ScaleY(pOS2->yStrikeoutPosition);
    }
    if (post) {
        potm->otmsUnderscoreSize = ScaleY(post->underlineThickness);
        potm->otmsUnderscorePosition = ScaleY(post->underlinePosition);
    }

    // FontBox (in pixels)
    potm->otmrcFontBox.left   = ScaleX(face->bbox.xMin);
    potm->otmrcFontBox.right  = ScaleX(face->bbox.xMax);
    potm->otmrcFontBox.top    = ScaleY(face->bbox.yMax);
    potm->otmrcFontBox.bottom = ScaleY(face->bbox.yMin);

    // Typographic values (convert sTypo* to pixels)
    if (pOS2) {
        potm->otmAscent  = ScaleY(pOS2->sTypoAscender);
        potm->otmDescent = ScaleY(pOS2->sTypoDescender);
        potm->otmLineGap = ScaleY(pOS2->sTypoLineGap);
        potm->otmMacAscent  = ScaleY(pOS2->sTypoAscender); // Windows commonly stores Typo values here
        potm->otmMacDescent = ScaleY(pOS2->sTypoDescender);
        potm->otmMacLineGap = ScaleY(pOS2->sTypoLineGap);
        memcpy(&potm->otmPanoseNumber, &pOS2->panose, 10);
    }

    // --- 3. Set string pointers ---
    // Store byte offsets from the start of the structure in otmpFamilyName etc.
    BYTE* pStrBase = (BYTE*)potm + sizeof(OUTLINETEXTMETRICW);
    auto SetString = [&](const std::wstring& s, PSTR& member) {
        size_t len = (s.length() + 1) * sizeof(WCHAR);
        memcpy(pStrBase, s.c_str(), len);
        member = (PSTR)(pStrBase - (BYTE*)potm); // Store as offset
        pStrBase += len;
    };

    SetString(wFamily, potm->otmpFamilyName);
    SetString(wFace,   potm->otmpFaceName);
    SetString(wStyle,  potm->otmpStyleName);
    SetString(wFull,   potm->otmpFullName);

    return potm;
}

bool load_font(PCWSTR path, int face_index)
{
    CHAR ansi_path[MAX_PATH];
    _StringCchAnsiFromWide(CP_ACP, ansi_path, _countof(ansi_path), path);

    int iStart = (face_index == -1) ? 0 : face_index;
    FT_Face face;
    FT_Error error = FT_New_Face(library, ansi_path, iStart, &face);
    if (error)
        return false;

    FT_Long num_faces = face->num_faces;
    std::vector<BYTE> charsets;

    TT_OS2* pOS2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (pOS2) {
        // Scan bits of ulCodePageRange1 and compare against g_FontTci
        for (int i = 0; i < MAXTCIINDEX; i++) {
            if (g_FontTci[i].fs.fsCsb[0] & (1 << i)) { // simplified check
                charsets.push_back(g_FontTci[i].ciCharset);
            }
        }
    }

    FT_WinFNT_HeaderRec WinFNT;
    INT raster_height = 0, raster_internal_leading = 0;
    if (FT_Get_WinFNT_Header(face, &WinFNT) == 0)
    {
        charsets.push_back(WinFNT.charset);
        raster_height = WinFNT.pixel_height;
        raster_internal_leading = WinFNT.internal_leading;
    }

    if (charsets.empty())
        charsets.push_back(DEFAULT_CHARSET);

    WCHAR szFamilyName[MAX_PATH];
    _StringCchWideFromAnsi(CP_ACP, szFamilyName, _countof(szFamilyName), face->family_name);

    for (BYTE cs : charsets) {
        if (face->num_fixed_sizes > 0) {
            for (int i = 0; i < face->num_fixed_sizes; ++i)
            {
                int height = face->available_sizes[i].height;
                FT_Select_Size(face, i);

                if (FT_Get_WinFNT_Header(face, &WinFNT) == 0)
                {
                    raster_height = WinFNT.pixel_height;
                    raster_internal_leading = WinFNT.internal_leading;
                }
                FontInfo* info = new FontInfo();
                _StringCchAnsiFromWide(CP_ACP, info->ansi_path, _countof(info->ansi_path), path);
                StringCchCopyW(info->wide_path, _countof(info->wide_path), path);

                info->face_index = iStart;
                info->family_name = get_family_name(face, TT_NAME_ID_FONT_FAMILY, true, szFamilyName);
                info->english_name = get_family_name(face, TT_NAME_ID_FONT_FAMILY, false, szFamilyName);
                info->style_flags = face->style_flags;
                info->charset = cs;
                info->raster_height = raster_height;
                info->raster_internal_leading = raster_internal_leading;
                registered_fonts.push_back(info);
            }
        } else {
            FontInfo* info = new FontInfo();
            _StringCchAnsiFromWide(CP_ACP, info->ansi_path, _countof(info->ansi_path), path);
            StringCchCopyW(info->wide_path, _countof(info->wide_path), path);
            info->face_index = iStart;
            info->family_name = get_family_name(face, TT_NAME_ID_FONT_FAMILY, true, szFamilyName);
            info->english_name = get_family_name(face, TT_NAME_ID_FONT_FAMILY, false, szFamilyName);
            info->style_flags = face->style_flags;
            info->charset = cs;
            info->raster_height = raster_height;
            info->raster_internal_leading = raster_internal_leading;
            registered_fonts.push_back(info);
        }
    }

    FT_Done_Face(face);

    if (face_index == -1)
    {
        for (FT_Long iFace = 1; iFace < num_faces; ++iFace)
        {
            if (!load_font(path, iFace))
                return false;
        }
    }

    return true;
}

void free_fonts(void)
{
    for (auto* info : registered_fonts)
    {
        delete info;
    }
    registered_fonts.clear();
}

// Return the list of available sizes for a raster font as a string like "8,10,12"
static std::wstring get_raster_sizes(FT_Face face)
{
    std::wstring result;
    for (int i = 0; i < face->num_fixed_sizes; ++i)
    {
        if (!result.empty())
            result += L",";
        WCHAR buf[16];
        // fixed_sizes[i].height is in pixels
        StringCchPrintfW(buf, _countof(buf), L"%d", static_cast<int>(face->available_sizes[i].height));
        result += buf;
    }
    return result;
}

// Generate a registry value name by grouping FontInfos from the same file.
// TrueType / OpenType: "MS Gothic & MS UI Gothic & MS PGothic (TrueType)"
// Raster:              "MS Sans Serif 8,10,12,14,18,24"
static std::wstring make_registry_value_name(
    const std::vector<FontInfo*>& group, FT_Face first_face)
{
    bool raster = is_raster_font(group[0]->wide_path);
    if (raster)
    {
        // Raster fonts include a size list (Windows-compatible)
        std::wstring sizes = get_raster_sizes(first_face);
        std::wstring name = group[0]->family_name;
        if (!sizes.empty())
            name += L" " + sizes;
        return name;
    }

    // Outline fonts: concatenate all family_names in the group with " & "
    // (duplicates are removed)
    std::vector<std::wstring> names;
    for (std::vector<FontInfo*>::const_iterator it = group.begin();
         it != group.end(); ++it)
    {
        const FontInfo* info = *it;
        bool found = false;
        for (std::vector<std::wstring>::const_iterator ni = names.begin(); ni != names.end(); ++ni)
        {
            if (lstrcmpiW(ni->c_str(), info->family_name.c_str()) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            names.push_back(info->family_name);
    }

    std::wstring value_name;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i > 0) value_name += L" & ";
        value_name += names[i];
    }

    value_name += L" (TrueType)";
    return value_name;
}

void write_fonts_to_registry(HKEY hKey)
{
    // Delete all existing values
    WCHAR szValue[MAX_PATH];
    for (;;) {
        DWORD cchValue = _countof(szValue);
        if (RegEnumValueW(hKey, 0, szValue, &cchValue, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        RegDeleteValueW(hKey, szValue);
    }

    // Group by path
    typedef std::pair<std::wstring, std::vector<FontInfo*> > FontGroup;
    std::vector<FontGroup> groups;

    for (std::vector<FontInfo*>::iterator it = registered_fonts.begin();
         it != registered_fonts.end(); ++it)
    {
        FontInfo* info = *it;
        bool found = false;
        for (std::vector<FontGroup>::iterator gi = groups.begin(); gi != groups.end(); ++gi)
        {
            if (lstrcmpiW(gi->first.c_str(), info->wide_path) == 0)
            {
                gi->second.push_back(info);
                found = true;
                break;
            }
        }
        if (!found)
        {
            groups.push_back(FontGroup(info->wide_path, std::vector<FontInfo*>(1, info)));
        }
    }

    for (std::vector<FontGroup>::iterator gi = groups.begin(); gi != groups.end(); ++gi)
    {
        const std::wstring& path = gi->first;
        const std::vector<FontInfo*>& group = gi->second;

        char ansi_path[MAX_PATH];
        _StringCchAnsiFromWide(CP_ACP, ansi_path, _countof(ansi_path), path.c_str());

        // Temporarily open the face to generate the value name
        FT_Face first_face;
        if (FT_New_Face(library, ansi_path, group[0]->face_index, &first_face) != 0)
            continue;

        std::wstring value_name = make_registry_value_name(group, first_face);
        FT_Done_Face(first_face);

        // Value data: filename only (matching the Windows registry format)
        WCHAR filename[MAX_PATH];
        if (path.find(fonts_dir) == 0)
            lstrcpynW(filename, PathFindFileNameW(path.c_str()), _countof(filename));
        else
            lstrcpynW(filename, path.c_str(), _countof(filename));

        DWORD cbValue = (lstrlenW(filename) + 1) * sizeof(WCHAR);
        RegSetValueExW(hKey, value_name.c_str(), 0, REG_SZ, (PBYTE)filename, cbValue);
    }
}

void read_fonts_from_registry(HKEY hKey)
{
    DWORD index = 0;
    WCHAR value_name[512];
    WCHAR value_data[MAX_PATH];
    DWORD name_size, data_size, type;

    for (;;)
    {
        name_size = sizeof(value_name);
        data_size = sizeof(value_data);
        LSTATUS status = RegEnumValueW(hKey, index++, value_name, &name_size,
                                       NULL, &type,
                                       reinterpret_cast<PBYTE>(value_data),
                                       &data_size);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS || type != REG_SZ)
            continue;

        // Convert to full path
        WCHAR full_path[MAX_PATH];
        if (PathIsRelativeW(value_data))
        {
            lstrcpynW(full_path, fonts_dir, _countof(full_path));
            PathAppendW(full_path, value_data);
        }
        else
        {
            lstrcpynW(full_path, value_data, _countof(full_path));
        }

        // Skip if the file does not exist
        if (!PathFileExistsW(full_path))
            continue;

        // Load all faces with FreeType (same logic as load_font with -1)
        load_font(full_path, -1);
    }
}

void load_from_fonts_folder()
{
    WCHAR path[MAX_PATH];
    lstrcpynW(path, fonts_dir, MAX_PATH);
    PathAppendW(path, L"*.*");

    WIN32_FIND_DATAW find;
    HANDLE hFind = FindFirstFileW(path, &find);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (lstrcmpW(find.cFileName, L".") == 0 || lstrcmpW(find.cFileName, L"..") == 0)
                continue;

            if (is_supported_font(find.cFileName)) {
                lstrcpynW(path, fonts_dir, _countof(path));
                PathAppendW(path, find.cFileName);
                load_font(path, -1);
            }
        } while (FindNextFileW(hFind, &find));
        FindClose(hFind);
    }
}

BOOL InitFontSupport(VOID)
{
    SHGetSpecialFolderPathW(NULL, fonts_dir, CSIDL_FONTS, FALSE);

    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        wprintf(L"FT_Init_FreeType: %d\n", error);
        return FALSE;
    }

    FT_Library_SetLcdFilter(library, FT_LCD_FILTER_DEFAULT);

    error = FTC_Manager_New(library, 0, 0, 0, requester, NULL, &cache_manager);
    if (error) {
        wprintf(L"FTC_Manager_New: %d\n", error);
        return FALSE;
    }

    HKEY hKey;
    error = RegCreateKeyExW(HKEY_CURRENT_USER, reg_key, 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &hKey, NULL);
    if (!error) {
        error = RegEnumValueW(hKey, 0, NULL, NULL, NULL, NULL, NULL, NULL);
        if (error) {
            load_from_fonts_folder();
            //write_fonts_to_registry(hKey);
        } else {
            read_fonts_from_registry(hKey);
        }
        RegCloseKey(hKey);
    }

    return TRUE;
}

VOID FreeFontSupport(VOID)
{
    free_fonts();
    if (cache_manager)
        FTC_Manager_Done(cache_manager);
    FT_Done_FreeType(library);
    DeleteObject(hbmMask_cache);
}

FontInfo* find_font_by_logfont(const LOGFONTW *plf)
{
    PCWSTR font_name = plf->lfFaceName;
    FT_Byte preferred_charset = plf->lfCharSet;
    int preferred_height = plf->lfHeight;

    FT_Long style_flags = 0;
    if (plf->lfWeight >= FW_BOLD)
        style_flags |= FT_STYLE_FLAG_BOLD;
    if (plf->lfItalic)
        style_flags |= FT_STYLE_FLAG_ITALIC;

    FontInfo* best = NULL;
    int total_penalty,best_penalty = INT_MAX;

    for (auto* font_info : registered_fonts)
    {
        if (lstrcmpiW(font_info->family_name.c_str(), font_name) != 0 &&
            lstrcmpiW(font_info->english_name.c_str(), font_name) != 0)
        {
            continue;
        }

        if (!is_raster_font(font_info->wide_path))
        {
            if (font_info->style_flags == style_flags)
                return font_info;
            continue;
        }

        int charset_penalty = 0, size_penalty = 0;
        if (preferred_charset != DEFAULT_CHARSET && font_info->charset != preferred_charset)
            charset_penalty += 10000;

        if (is_raster_font(font_info->wide_path))
        {
            int size;
            if (preferred_height < 0)
                size = labs(preferred_height) + font_info->raster_internal_leading;
            else if (preferred_height > 0)
                size = preferred_height;
            else
                size = 12;
            size_penalty = abs(font_info->raster_height - size);
        }

        total_penalty = charset_penalty + size_penalty;

        if (total_penalty < best_penalty)
        {
            best_penalty = total_penalty;
            best = font_info;
        }
    }

    if (best)
        return best;

    // If style_flags do not match, search again by name only
    for (std::vector<FontInfo*>::iterator it = registered_fonts.begin();
         it != registered_fonts.end(); ++it)
    {
        FontInfo* font_info = *it;
        if (lstrcmpiW(font_info->family_name.c_str(), font_name) == 0)
            return font_info;
    }

    return NULL;
}

void draw_glyph(HDC hdc, FT_Bitmap* bitmap, int left, int top,
                COLORREF fg_color, COLORREF bg_color)
{
    int w = (int)bitmap->width;
    int h = (int)bitmap->rows;
    if (w <= 0 || h <= 0)
        return;

    int bkMode = GetBkMode(hdc);

    // Create a working DC and bitmap
    HDC hdcSrc = CreateCompatibleDC(hdc);
    HBITMAP hbmSrc = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ hbmSrcOld = SelectObject(hdcSrc, hbmSrc);

    if (bkMode == OPAQUE) {
        HBRUSH hbrBg = CreateSolidBrush(bg_color);
        RECT rc = { left, top, left + w, top + h };
        FillRect(hdc, &rc, hbrBg);
        DeleteObject(hbrBg);
    }

    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
    {
        // --- Monochrome (1bpp) processing ---

        HBRUSH hbrFg = CreateSolidBrush(fg_color);
        RECT rcSrc = { 0, 0, w, h };
        FillRect(hdcSrc, &rcSrc, hbrFg);
        DeleteObject(hbrFg);

        if (w > hbmMask_cache_w || h > hbmMask_cache_h)
        {
            if (hbmMask_cache) { DeleteObject(hbmMask_cache); hbmMask_cache = NULL; }
            hbmMask_cache = CreateBitmap(w, h, 1, 1, NULL);
            hbmMask_cache_w = w;
            hbmMask_cache_h = h;
        }

        int src_pitch = (bitmap->pitch < 0) ? -bitmap->pitch : bitmap->pitch;
        int row_bytes = (w + 7) / 8;
        int dib_stride = (row_bytes + 3) & ~3; // DWORD align

        std::vector<BYTE> packed(dib_stride * h, 0);
        for (int r = 0; r < h; ++r)
            memcpy(&packed[r * dib_stride],
                   bitmap->buffer + r * src_pitch,
                   row_bytes);

        // Pass as top-down DIB by setting biHeight negative
        struct { BITMAPINFOHEADER bih; RGBQUAD colors[2]; } bmiMono2 = {};
        bmiMono2.bih.biSize        = sizeof(BITMAPINFOHEADER);
        bmiMono2.bih.biWidth       = w;
        bmiMono2.bih.biHeight      = -h; // top-down
        bmiMono2.bih.biPlanes      = 1;
        bmiMono2.bih.biBitCount    = 1;
        bmiMono2.bih.biCompression = BI_RGB;
        bmiMono2.colors[0] = { 0,   0,   0,   0 }; // index 0 = black
        bmiMono2.colors[1] = { 255, 255, 255, 0 }; // index 1 = white

        SetDIBits(NULL, hbmMask_cache, 0, h, packed.data(),
                  reinterpret_cast<BITMAPINFO*>(&bmiMono2), DIB_RGB_COLORS);

        MaskBlt(hdc, left, top, w, h,
                hdcSrc, 0, 0,
                hbmMask_cache, 0, 0,
                MAKEROP4(SRCCOPY, 0x00AA0029 /* DST */));

        // Do not DeleteObject hbmMask_cache as it is reused
    }
    else if (bitmap->pixel_mode == FT_PIXEL_MODE_LCD)
    {
        int logical_w = w / 3;

        HDC hdcWork = CreateCompatibleDC(hdc);
        HBITMAP hbmWork = CreateCompatibleBitmap(hdc, logical_w, h);
        HGDIOBJ hbmWorkOld = SelectObject(hdcWork, hbmWork);

        BitBlt(hdcWork, 0, 0, logical_w, h, hdc, left, top, SRCCOPY);

        std::vector<DWORD> pixels(logical_w * h);
        BITMAPINFO bmi = { 0 };
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = logical_w;
        bmi.bmiHeader.biHeight      = -h; // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        GetDIBits(hdcWork, hbmWork, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);

        int src_pitch = (bitmap->pitch < 0) ? -bitmap->pitch : bitmap->pitch;

        for (int row = 0; row < h; ++row)
        {
            for (int col = 0; col < logical_w; ++col)
            {
                unsigned char R_sub = bitmap->buffer[row * src_pitch + col * 3 + 0];
                unsigned char G_sub = bitmap->buffer[row * src_pitch + col * 3 + 1];
                unsigned char B_sub = bitmap->buffer[row * src_pitch + col * 3 + 2];

                if (R_sub == 0 && G_sub == 0 && B_sub == 0) continue;

                COLORREF target_bg;
                if (bkMode == TRANSPARENT) {
                    DWORD pixel = pixels[row * logical_w + col];
                    target_bg = RGB(GetBValue(pixel), GetGValue(pixel), GetRValue(pixel));
                } else {
                    target_bg = bg_color;
                }

                int r = (GetRValue(fg_color) * R_sub + GetRValue(target_bg) * (255 - R_sub)) / 255;
                int g = (GetGValue(fg_color) * G_sub + GetGValue(target_bg) * (255 - G_sub)) / 255;
                int b = (GetBValue(fg_color) * B_sub + GetBValue(target_bg) * (255 - B_sub)) / 255;

                pixels[row * logical_w + col] = ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
            }
        }

        SetDIBits(hdcWork, hbmWork, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);
        BitBlt(hdc, left, top, logical_w, h, hdcWork, 0, 0, SRCCOPY);

        SelectObject(hdcWork, hbmWorkOld);
        DeleteObject(hbmWork);
        DeleteDC(hdcWork);
    }
    else
    {
        // --- Grayscale (8bpp) processing ---

        BitBlt(hdcSrc, 0, 0, w, h, hdc, left, top, SRCCOPY);

        std::vector<DWORD> pixels(w * h);
        BITMAPINFO bmi = { 0 };
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h; // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        GetDIBits(hdcSrc, hbmSrc, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);

        int src_pitch = (bitmap->pitch < 0) ? -bitmap->pitch : bitmap->pitch;

        for (int row = 0; row < h; ++row)
        {
            for (int col = 0; col < w; ++col)
            {
                unsigned char alpha = bitmap->buffer[row * src_pitch + col];
                if (alpha == 0) continue;

                COLORREF target_bg;
                if (bkMode == TRANSPARENT) {
                    DWORD pixel = pixels[row * w + col];
                    target_bg = RGB(GetBValue(pixel), GetGValue(pixel), GetRValue(pixel));
                } else {
                    target_bg = bg_color;
                }

                int r = (GetRValue(fg_color) * alpha + GetRValue(target_bg) * (255 - alpha)) / 255;
                int g = (GetGValue(fg_color) * alpha + GetGValue(target_bg) * (255 - alpha)) / 255;
                int b = (GetBValue(fg_color) * alpha + GetBValue(target_bg) * (255 - alpha)) / 255;
                pixels[row * w + col] = ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
            }
        }

        SetDIBits(hdcSrc, hbmSrc, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);
        BitBlt(hdc, left, top, w, h, hdcSrc, 0, 0, SRCCOPY);
    }

    SelectObject(hdcSrc, hbmSrcOld);
    DeleteObject(hbmSrc);
    DeleteDC(hdcSrc);
}

BOOL EmulatedExtTextOutW(
    HDC hdc,
    INT X,
    INT Y,
    UINT fuOptions,
    CONST RECT *lprc,
    const WCHAR* lpString,
    INT Count,
    CONST INT *lpDx)
{
    HFONT hFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);

    LOGFONTW lf;
    GetObjectW(hFont, sizeof(lf), &lf);
    FontInfo* font_info = find_font_by_logfont(&lf);
    if (!font_info) {
        wprintf(L"'%S': not found\n", lf.lfFaceName);
        return FALSE;
    }
    LONG lfHeight = lf.lfHeight;

    wprintf(L"Using font: %S, %ld\n", font_info->wide_path, lfHeight);

    POINT Start, CurPos;
    LONGLONG RealXStart64, RealYStart64;

    if (lprc && !(fuOptions & (ETO_CLIPPED|ETO_OPAQUE)))
    {
        lprc = NULL; // No flags, no rectangle.
    }
    else if (!lprc) // No rectangle, force clear flags if set and continue.
    {
        fuOptions &= ~(ETO_CLIPPED|ETO_OPAQUE);
    }

    if (Count > 0xFFFF || (Count > 0 && lpString == NULL))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Start.x = X;
    Start.y = Y;

    LPtoDP(hdc, &Start, 1);

    RealXStart64 = ((LONGLONG)Start.x) << 6;
    RealYStart64 = ((LONGLONG)Start.y) << 6;

    RECT MaskRect;
    MaskRect.left = 0;
    MaskRect.top = 0;

    XFORM xform;
    GetWorldTransform(hdc, &xform);

    if (lprc && (fuOptions & (ETO_CLIPPED | ETO_OPAQUE)))
    {
        LPtoDP(hdc, (POINT*)lprc, 2);
    }

    COLORREF fg_color = GetTextColor(hdc);
    COLORREF bg_color = GetBkColor(hdc);

    if (lprc && (fuOptions & ETO_OPAQUE))
    {
        HBRUSH hbr = CreateSolidBrush(bg_color);
        FillRect(hdc, lprc, hbr);
        DeleteObject(hbr);
        fuOptions &= ~ETO_OPAQUE;
    }
    else
    {
        if (GetBkMode(hdc) == OPAQUE)
        {
            fuOptions |= ETO_OPAQUE;
        }
    }

    bool is_raster = is_raster_font(font_info->wide_path);
    FT_Face face = NULL;
    bool face_needs_done = false;

    FT_WinFNT_HeaderRec WinFNT;
    bool has_fnt_header = false;
    int pixel_ascent, baseline_y;

    if (is_raster)
    {
        // Raster fonts are opened directly without going through the cache
        if (FT_New_Face(library, font_info->ansi_path, font_info->face_index, &face) != 0)
            return FALSE;
        face_needs_done = true;

        if (face->num_fixed_sizes > 0) {
            int target_cell;
            if (lfHeight < 0)
                target_cell = labs(lfHeight) + font_info->raster_internal_leading;
            else
                target_cell = abs(lfHeight);

            int best_idx = 0;
            int best_diff = abs(face->available_sizes[0].height - target_cell);
            for (int si = 1; si < face->num_fixed_sizes; ++si) {
                int diff = abs(face->available_sizes[si].height - target_cell);
                if (diff < best_diff) { best_diff = diff; best_idx = si; }
            }
            FT_Select_Size(face, best_idx);
        }

        // Added immediately after FT_Select_Size
        bool has_fnt_header = (FT_Get_WinFNT_Header(face, &WinFNT) == 0);
        wprintf(L"first_char=0x%02X, last_char=0x%02X, default_char=0x%02X\n",
            WinFNT.first_char, WinFNT.last_char, WinFNT.default_char);

        if (has_fnt_header) {
            pixel_ascent = WinFNT.ascent;
        } else {
            pixel_ascent = (face->size->metrics.ascender + 32) >> 6;
        }
        baseline_y   = Y + pixel_ascent;
    }
    else
    {
        // Outline fonts: compute ppem with VDMX then Wine-compatible fallback.
        // Step 1 - open a temporary face to read font tables (calc_ppem_for_height
        //          and load_VDMX need the raw FT_Face, not the cached FT_Size).
        FT_Face tmp_face = NULL;
        if (FT_New_Face(library, font_info->ansi_path, font_info->face_index, &tmp_face) != 0)
            return FALSE;

        // Step 2 - try VDMX first (exact Windows metrics for common sizes)
        VdmxEntry vdmx = {};
        bool have_vdmx = load_VDMX(tmp_face, lfHeight, &vdmx);

        // Step 3 - compute ppem
        int ppem;
        if (have_vdmx)
            ppem = vdmx.ppem;
        else
            ppem = calc_ppem_for_height(tmp_face, lfHeight);

        FT_Done_Face(tmp_face);

        // Step 4 - look up (or create) the sized face in the cache
        FTC_ScalerRec scaler;
        scaler.face_id = static_cast<FTC_FaceID>(font_info);
        scaler.width   = 0;
        scaler.height  = ppem;
        scaler.pixel   = 1;
        scaler.x_res   = 96;
        scaler.y_res   = 96;

        FT_Size ft_size;
        if (FTC_Manager_LookupSize(cache_manager, &scaler, &ft_size) != 0)
            return FALSE;
        face = ft_size->face;

        // Step 5 - determine pixel ascent (= distance from top of cell to baseline)
        if (have_vdmx)
        {
            // VDMX gives us exact integer pixel values - use them directly.
            pixel_ascent = vdmx.yMax;
        }
        else
        {
            // No VDMX: scale usWinAscent with the em_scale we just computed.
            // em_scale is a 16.16 fixed-point value: ppem / units_per_EM.
            TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
            if (os2 && (os2->usWinAscent != 0 || os2->usWinDescent != 0))
            {
                FT_Fixed em_scale = FT_MulDiv(
                    (FT_Long)ppem, 1 << 16,
                    (FT_Long)face->units_per_EM);
                pixel_ascent = (int)FT_MulFix((FT_Long)os2->usWinAscent, em_scale);
            }
            else
            {
                pixel_ascent = (face->size->metrics.ascender + 32) >> 6;
            }
        }

        baseline_y = Y + pixel_ascent;
    }

    FT_Int32 load_flags;
    if (is_raster)
    {
        // Raster fonts always retrieve a monochrome bitmap.
        // FT_LOAD_NO_SCALE instructs FreeType to use the fixed size as-is.
        load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_NO_HINTING;
    }
    else
    {
        load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_LCD | FT_LOAD_FORCE_AUTOHINT;
    }

    FT_Pos current_pen_x = (FT_Pos)X << 6;
    FT_Pos current_pen_y = (FT_Pos)baseline_y << 6;
    int lpDx_accumulated = 0;
    FT_UInt previous_glyph = 0; // Holds the previous glyph index
    bool use_kerning = false;

    const WCHAR* pch = lpString;
    for (INT i = 0; i < Count; ++i)
    {
        unsigned long codepoint = 0;
        WCHAR w1 = pch[i];
        if (IS_HIGH_SURROGATE(w1) && i + 1 < Count) {
            WCHAR w2 = pch[i + 1];
            if (IS_LOW_SURROGATE(w2)) {
                codepoint = MAKE_SURROGATE_PAIR(w1, w2);
                ++i;
            }
        } else {
            codepoint = w1;
        }

        FT_UInt glyph_index;
        if (is_raster)
        {
            // Convert Unicode codepoint to the FON codepage
            WCHAR wc = static_cast<WCHAR>(codepoint);
            char mb[4] = {};
            UINT codepage = 1252;
            CHARSETINFO csi;
            if (TranslateCharsetInfo((DWORD*)(DWORD_PTR)font_info->charset,
                                     &csi, TCI_SRCCHARSET))
                codepage = csi.ciACP;

            int mblen = WideCharToMultiByte(codepage, 0, &wc, 1,
                                            mb, sizeof(mb), NULL, NULL);
            if (mblen != 1)
                continue;

            unsigned char byte_val = (unsigned char)mb[0];

            if (byte_val < WinFNT.first_char || byte_val > WinFNT.last_char)
            {
                // Out of range: use default_char
                glyph_index = WinFNT.default_char - WinFNT.first_char;
            }
            else
            {
                glyph_index = byte_val - WinFNT.first_char + 1;
            }

            wprintf(L"glyph_index=%u, byte_val=0x%02X, first_char=0x%02X, calc=%u\n",
                glyph_index, byte_val, WinFNT.first_char,
                byte_val - WinFNT.first_char);
        }
        else
        {
            glyph_index = FT_Get_Char_Index(face, codepoint);
        }

        if (use_kerning && previous_glyph != 0 && glyph_index != 0) {
            FT_Vector delta;
            FT_Get_Kerning(face, previous_glyph, glyph_index, FT_KERNING_DEFAULT, &delta);
            current_pen_x += delta.x;
        }

        if (FT_Load_Glyph(face, glyph_index, load_flags) != 0)
            continue;

        // Verify cmap
        wprintf(L"num_charmaps=%d\n", face->num_charmaps);
        for (int ci = 0; ci < face->num_charmaps; ++ci)
        {
            wprintf(L"  charmap[%d]: platform=%d, encoding=%d, encoding_id=%d\n",
                ci,
                face->charmaps[ci]->platform_id,
                face->charmaps[ci]->encoding,
                face->charmaps[ci]->encoding_id);
        }
        wprintf(L"active charmap: platform=%d, encoding=%d\n",
            face->charmap ? face->charmap->platform_id : -1,
            face->charmap ? face->charmap->encoding    : -1);

        FT_GlyphSlot slot = face->glyph;

        int draw_x = (current_pen_x >> 6) + slot->bitmap_left;
        int draw_y = baseline_y - slot->bitmap_top;

        draw_glyph(hdc, &slot->bitmap, draw_x, draw_y,
                   fg_color, bg_color);

        wprintf(L"glyph U+%04lX: bitmap=%dx%d, advance.x=%ld (>>6=%ld), bitmap_left=%d, bitmap_top=%d\n",
            codepoint,
            slot->bitmap.width, slot->bitmap.rows,
            slot->advance.x, slot->advance.x >> 6,
            slot->bitmap_left, slot->bitmap_top);

        if (lpDx) {
            lpDx_accumulated += lpDx[i];
            current_pen_x = ((FT_Pos)X << 6) + ((FT_Pos)lpDx_accumulated << 6);
        } else {
            current_pen_x += (face->glyph->advance.x + 63) & ~63;
        }
        previous_glyph = glyph_index;
    }

    if (face_needs_done)
        FT_Done_Face(face);

    return TRUE;
}

HBITMAP TestFreeType(PCWSTR font_name, int font_size, XFORM& xform, HFONT hFont)
{
    HDC hScreenDC = GetDC(NULL);
    HBITMAP hbm = CreateCompatibleBitmap(hScreenDC, WIDTH, HEIGHT);
    ReleaseDC(NULL, hScreenDC);

    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ hbmOld = SelectObject(hdc, hbm);

    RECT rc = { 0, 0, WIDTH, HEIGHT };
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

    SetTextColor(hdc, color1);
    SetBkMode(hdc, TRANSPARENT);
    SetBkColor(hdc, color2);

    SetGraphicsMode(hdc, GM_ADVANCED);
    SetWorldTransform(hdc, &xform);

    HGDIOBJ hFontOld = SelectObject(hdc, hFont);
    EmulatedExtTextOutW(hdc, WIDTH / 2, HEIGHT / 2, 0, &rc, text, lstrlenW(text), NULL);
    SelectObject(hdc, hFontOld);

    SelectObject(hdc, hbmOld);
    DeleteDC(hdc);

    return hbm;
}

HBITMAP TestGdi(PCWSTR font_name, int font_size, XFORM& xform, HFONT hFont)
{
    HDC hScreenDC = GetDC(NULL);
    HBITMAP hbm = CreateCompatibleBitmap(hScreenDC, WIDTH, HEIGHT);
    ReleaseDC(NULL, hScreenDC);

    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ hbmOld = SelectObject(hdc, hbm);

    RECT rc = { 0, 0, WIDTH, HEIGHT };
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

    SetTextColor(hdc, color1);
    SetBkMode(hdc, TRANSPARENT);
    SetBkColor(hdc, color2);

    SetGraphicsMode(hdc, GM_ADVANCED);
    SetWorldTransform(hdc, &xform);

    HGDIOBJ hFontOld = SelectObject(hdc, hFont);
    ExtTextOutW(hdc, WIDTH / 2, HEIGHT / 2, 0, &rc, text, lstrlenW(text), NULL);
    SelectObject(hdc, hFontOld);

    SelectObject(hdc, hbmOld);
    DeleteDC(hdc);

    return hbm;
}

bool TestEntry(PCWSTR font_name, int font_size, XFORM& xform)
{
    wprintf(L"%ls, %d: Testing\n", font_name, font_size);

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = font_size;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyW(lf.lfFaceName, font_name);

    HFONT hFont = CreateFontIndirectW(&lf);

    HBITMAP hbm1 = TestFreeType(font_name, font_size, xform, hFont);
    HBITMAP hbm2 = TestGdi(font_name, font_size, xform, hFont);
    if (!hbm1)
        wprintf(L"!hbm1\n");
    if (!hbm2)
        wprintf(L"!hbm2\n");

    DeleteObject(hFont);

    BOOL ret = nearly_equal_bitmap(hbm1, hbm2, color1, color2);
    if (ret)
    {
        wprintf(L"%ls, %d: Success!\n", font_name, font_size);
    }
    else
    {
        wprintf(L"%ls, %d: FAILED\n", font_name, font_size);
    }

    SaveBitmapToFile("a.bmp", hbm1);
    SaveBitmapToFile("b.bmp", hbm2);
    DeleteObject(hbm2);
    DeleteObject(hbm1);
    return ret;
}

#include <io.h>
#include <fcntl.h>
#include <locale.h>

int wmain(int argc, wchar_t** wargv)
{
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U16TEXT);
    setlocale(LC_ALL, "");

    PCWSTR font_name = FONT_NAME;
    int font_size = FONT_SIZE;

    XFORM xform;
    xform.eM11 = 1;
    xform.eM12 = 0;
    xform.eM21 = 0;
    xform.eM22 = 1;
    xform.eDx = 0;
    xform.eDy = 0;

    if (argc >= 2) font_name = wargv[1];
    if (argc >= 3) font_size = _wtoi(wargv[2]);
    if (argc >= 4) xform.eM11 = _wtoi(wargv[3]);
    if (argc >= 5) xform.eM12 = _wtoi(wargv[4]);
    if (argc >= 6) xform.eM21 = _wtoi(wargv[5]);
    if (argc >= 7) xform.eM22 = _wtoi(wargv[6]);

    if (!InitFontSupport()) {
        FreeFontSupport();
        return -1;
    }

    bool ret = TestEntry(font_name, font_size, xform);

    FreeFontSupport();
    return ret ? 0 : 1;
}

int main(void)
{
    int argc;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int ret = wmain(argc, wargv);
    LocalFree(wargv);
    return ret;
}
