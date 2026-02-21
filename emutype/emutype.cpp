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

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CACHE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include FT_WINFONTS_H

#include "SaveBitmapToFile.h"
#include "nearly_equal_bitmap.h"

#define MAKE_SURROGATE_PAIR(w1, w2) \
    (0x10000 + (((DWORD)(w1) - HIGH_SURROGATE_START) << 10) + ((DWORD)(w2) - LOW_SURROGATE_START));

#define _TMPF_VARIABLE_PITCH TMPF_FIXED_PITCH // TMPF_FIXED_PITCH is a brain-dead API

const int WIDTH  = 300;
const int HEIGHT = 100;
const COLORREF BG = RGB(255, 255, 0);
const COLORREF FG = RGB(0,   0,   0);
const char* FONT_NAME = "Tahoma";
const LONG FONT_SIZE = 30;
const WCHAR* text = L"FreeType Draw";
const COLORREF color1 = RGB(0, 0, 0);
const COLORREF color2 = RGB(255, 255, 0);

// Using a different registry key for this test.
const char* reg_key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontsEmulated";

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
    std::string path; // ANSI
    FT_Long face_index;
    std::string family_name; // UTF-8
    std::string english_name; // UTF-8
    FT_Long style_flags; // See FT_Face.style_flags
    FT_Long em_ascender;
    FT_Long em_descender;
    FT_Long em_units;
    BYTE charset;
    INT raster_height;
};
std::vector<FontInfo*> registered_fonts;

char fonts_dir[MAX_PATH];
FT_Library library;
FTC_Manager cache_manager;

HBITMAP hbmMask_cache = NULL;
int hbmMask_cache_w = 0, hbmMask_cache_h = 0;

// Helper to determine font type
static bool is_raster_font(const std::string& path)
{
    LPCSTR ext = PathFindExtensionA(path.c_str());
    return lstrcmpiA(ext, ".fon") == 0 || lstrcmpiA(ext, ".fnt") == 0;
}

bool is_supported_font(const char *filename) {
    LPCSTR pchDotExt = PathFindExtensionA(filename);
    return lstrcmpiA(pchDotExt, ".ttf") == 0 ||
           lstrcmpiA(pchDotExt, ".ttc") == 0 ||
           lstrcmpiA(pchDotExt, ".otf") == 0 ||
           lstrcmpiA(pchDotExt, ".otc") == 0 ||
           lstrcmpiA(pchDotExt, ".fon") == 0 ||
           lstrcmpiA(pchDotExt, ".fnt") == 0;
}

int calc_pixel_size_from_cell_height(FontInfo* font_info, int cell_height_px)
{
    FT_Long em_ascender  = font_info->em_ascender;
    FT_Long em_descender = font_info->em_descender;
    FT_Long em_units     = font_info->em_units;
    FT_Long em_cell = em_ascender - em_descender;
    int pixel_size = static_cast<int>(static_cast<double>(cell_height_px) * em_units / em_cell);
    return pixel_size;
}

FT_Error
requester(FTC_FaceID  face_id,
          FT_Library  lib,
          FT_Pointer  req_data,
          FT_Face*    aface)
{
    FontInfo* info = static_cast<FontInfo*>(face_id);
    return FT_New_Face(lib, info->path.c_str(), info->face_index, aface);
}

static std::string get_family_name(FT_Face face, bool localized)
{
    if (!FT_IS_SFNT(face))
        return face->family_name ? face->family_name : "";

    FT_UInt count = FT_Get_Sfnt_Name_Count(face);

    int best_priority = -1;
    std::string best_name;

    for (FT_UInt i = 0; i < count; ++i)
    {
        FT_SfntName sname;
        if (FT_Get_Sfnt_Name(face, i, &sname) != 0)
            continue;
        if (sname.name_id != TT_NAME_ID_FONT_FAMILY)
            continue;

        int priority = -1;
        std::string name;

        if (sname.platform_id == TT_PLATFORM_MICROSOFT &&
            sname.encoding_id == TT_MS_ID_UNICODE_CS)
        {
            int wlen = static_cast<int>(sname.string_len) / 2;
            std::vector<wchar_t> wbuf(wlen + 1, 0);
            for (int j = 0; j < wlen; ++j)
            {
                wbuf[j] = static_cast<wchar_t>(
                    (sname.string[j * 2] << 8) | sname.string[j * 2 + 1]);
            }
            int mblen = WideCharToMultiByte(CP_UTF8, 0,
                                            &wbuf[0], wlen,
                                            NULL, 0, NULL, NULL);
            if (mblen > 0)
            {
                std::string mbstr(mblen, '\0');
                WideCharToMultiByte(CP_UTF8, 0,
                                    &wbuf[0], wlen,
                                    &mbstr[0], mblen, NULL, NULL);
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
            // Mac Roman: usable as-is within the ASCII range
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

    // Fallback
    return face->family_name ? face->family_name : "";
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

    // 1. Prepare each string
    std::string family_utf8 = get_family_name(face, true);
    std::string style_utf8  = get_style_name(face, true);
    std::string face_utf8   = family_utf8 + (style_utf8.empty() ? "" : " " + style_utf8);
    std::wstring wFamily = utf8_to_wide(family_utf8);
    std::wstring wFace   = utf8_to_wide(face_utf8);
    std::wstring wStyle  = utf8_to_wide(style_utf8);
    std::wstring wFull   = wFace; 

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

bool load_font(const char *path, int face_index)
{
    int iStart = (face_index == -1) ? 0 : face_index;
    FT_Face face;
    FT_Error error = FT_New_Face(library, path, iStart, &face);
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
    INT raster_height = 0;
    if (FT_Get_WinFNT_Header(face, &WinFNT) == 0)
    {
        charsets.push_back(WinFNT.charset);
        raster_height = WinFNT.pixel_height - WinFNT.internal_leading;
    }

    if (charsets.empty())
        charsets.push_back(DEFAULT_CHARSET);

    for (BYTE cs : charsets) {
        FontInfo* info = new FontInfo();
        info->path = path;
        info->face_index = iStart;
        info->family_name = get_family_name(face, true);
        info->english_name = get_family_name(face, false);
        info->style_flags = face->style_flags;
        info->em_ascender  = face->ascender;
        info->em_descender = face->descender;
        info->em_units = face->units_per_EM;
        info->charset = cs;
        info->raster_height = raster_height;
        registered_fonts.push_back(info);
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
static std::string get_raster_sizes(FT_Face face)
{
    std::string result;
    for (int i = 0; i < face->num_fixed_sizes; ++i)
    {
        if (!result.empty())
            result += ",";
        char buf[16];
        // fixed_sizes[i].height is in pixels
        _snprintf(buf, sizeof(buf), "%d", static_cast<int>(face->available_sizes[i].height));
        result += buf;
    }
    return result;
}

// Generate a registry value name by grouping FontInfos from the same file.
// TrueType / OpenType: "MS Gothic & MS UI Gothic & MS PGothic (TrueType)"
// Raster:              "MS Sans Serif 8,10,12,14,18,24"
static std::string make_registry_value_name(
    const std::vector<FontInfo*>& group, FT_Face first_face)
{
    bool raster = is_raster_font(group[0]->path);
    if (raster)
    {
        // Raster fonts include a size list (Windows-compatible)
        std::string sizes = get_raster_sizes(first_face);
        std::string name = group[0]->family_name;
        if (!sizes.empty())
            name += " " + sizes;
        return name;
    }

    // Outline fonts: concatenate all family_names in the group with " & "
    // (duplicates are removed)
    std::vector<std::string> names;
    for (std::vector<FontInfo*>::const_iterator it = group.begin();
         it != group.end(); ++it)
    {
        const FontInfo* info = *it;
        bool found = false;
        for (std::vector<std::string>::const_iterator ni = names.begin();
             ni != names.end(); ++ni)
        {
            if (lstrcmpiA(ni->c_str(), info->family_name.c_str()) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            names.push_back(info->family_name);
    }

    std::string value_name;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i > 0) value_name += " & ";
        value_name += names[i];
    }

    value_name += " (TrueType)";
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
    typedef std::pair<std::string, std::vector<FontInfo*> > FontGroup;
    std::vector<FontGroup> groups;

    for (std::vector<FontInfo*>::iterator it = registered_fonts.begin();
         it != registered_fonts.end(); ++it)
    {
        FontInfo* info = *it;
        bool found = false;
        for (std::vector<FontGroup>::iterator gi = groups.begin();
             gi != groups.end(); ++gi)
        {
            if (lstrcmpiA(gi->first.c_str(), info->path.c_str()) == 0)
            {
                gi->second.push_back(info);
                found = true;
                break;
            }
        }
        if (!found)
        {
            groups.push_back(FontGroup(info->path, std::vector<FontInfo*>(1, info)));
        }
    }

    for (std::vector<FontGroup>::iterator gi = groups.begin();
         gi != groups.end(); ++gi)
    {
        const std::string& path = gi->first;
        const std::vector<FontInfo*>& group = gi->second;

        // Temporarily open the face to generate the value name
        FT_Face first_face;
        if (FT_New_Face(library, path.c_str(), group[0]->face_index, &first_face) != 0)
            continue;

        std::string value_name = make_registry_value_name(group, first_face);
        FT_Done_Face(first_face);

        // Value data: filename only (matching the Windows registry format)
        char filename[MAX_PATH];
        if (path.find(fonts_dir) == 0)
            lstrcpynA(filename, PathFindFileNameA(path.c_str()), MAX_PATH);
        else
            lstrcpynA(filename, path.c_str(), MAX_PATH);

        // Convert value_name (UTF-8) to UTF-16
        int wname_len = MultiByteToWideChar(CP_UTF8, 0,
                                            value_name.c_str(), -1,
                                            NULL, 0);
        std::vector<WCHAR> wname(wname_len);
        MultiByteToWideChar(CP_UTF8, 0,
                            value_name.c_str(), -1,
                            &wname[0], wname_len);

        // Convert filename (system default ACP) to UTF-16
        int wdata_len = MultiByteToWideChar(CP_ACP, 0,
                                            filename, -1,
                                            NULL, 0);
        std::vector<WCHAR> wdata(wdata_len);
        MultiByteToWideChar(CP_ACP, 0,
                            filename, -1,
                            &wdata[0], wdata_len);

        RegSetValueExW(hKey, &wname[0], 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(&wdata[0]),
                       static_cast<DWORD>(wdata_len * sizeof(WCHAR)));
    }
}

void read_fonts_from_registry(HKEY hKey)
{
    DWORD index = 0;
    char value_name[512];
    char value_data[MAX_PATH];
    DWORD name_size, data_size, type;

    for (;;)
    {
        name_size = sizeof(value_name);
        data_size = sizeof(value_data);
        LSTATUS status = RegEnumValueA(hKey, index++, value_name, &name_size,
                                       NULL, &type,
                                       reinterpret_cast<LPBYTE>(value_data),
                                       &data_size);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS || type != REG_SZ)
            continue;

        // Convert to full path
        char full_path[MAX_PATH];
        if (PathIsRelativeA(value_data))
        {
            lstrcpynA(full_path, fonts_dir, MAX_PATH);
            PathAppendA(full_path, value_data);
        }
        else
        {
            lstrcpynA(full_path, value_data, MAX_PATH);
        }

        // Skip if the file does not exist
        if (!PathFileExistsA(full_path))
            continue;

        // Load all faces with FreeType (same logic as load_font with -1)
        load_font(full_path, -1);
    }
}

void load_from_fonts_folder()
{
    char path[MAX_PATH];
    lstrcpynA(path, fonts_dir, MAX_PATH);
    PathAppendA(path, "*.*");

    WIN32_FIND_DATAA find;
    HANDLE hFind = FindFirstFileA(path, &find);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (lstrcmpA(find.cFileName, ".") == 0 || lstrcmpA(find.cFileName, "..") == 0)
                continue;

            if (is_supported_font(find.cFileName)) {
                lstrcpynA(path, fonts_dir, MAX_PATH);
                PathAppendA(path, find.cFileName);
                load_font(path, -1);
            }
        } while (FindNextFileA(hFind, &find));
        FindClose(hFind);
    }
}

BOOL InitFontSupport(VOID)
{
    SHGetSpecialFolderPathA(NULL, fonts_dir, CSIDL_FONTS, FALSE);

    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        printf("FT_Init_FreeType: %d\n", error);
        return FALSE;
    }

    error = FTC_Manager_New(library, 0, 0, 0, requester, NULL, &cache_manager);
    if (error) {
        printf("FTC_Manager_New: %d\n", error);
        return FALSE;
    }

    HKEY hKey;
    error = RegCreateKeyExA(HKEY_CURRENT_USER, reg_key, 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &hKey, NULL);
    if (!error) {
        error = RegEnumValueA(hKey, 0, NULL, NULL, NULL, NULL, NULL, NULL);
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

FontInfo* find_font_by_logfont(const LOGFONTA *plf)
{
    const char* font_name = plf->lfFaceName;
    FT_Byte preferred_charset = plf->lfCharSet;
    int preferred_height = plf->lfHeight;

    FT_Long style_flags = 0;
    if (plf->lfWeight > FW_NORMAL)
        style_flags |= FT_STYLE_FLAG_BOLD;
    if (plf->lfItalic)
        style_flags |= FT_STYLE_FLAG_ITALIC;

    FontInfo* best = NULL;
    int total_penalty,best_penalty = INT_MAX;

    for (auto* info : registered_fonts)
    {
        if (lstrcmpiA(info->family_name.c_str(), font_name) != 0)
            continue;

        if (!is_raster_font(info->path))
        {
            if (info->style_flags == style_flags)
                return info;
            continue;
        }

        int charset_penalty = 0;
        if (preferred_charset != DEFAULT_CHARSET && info->charset != preferred_charset)
            charset_penalty += 10000;

        if (is_raster_font(info->path))
        {
            int size_penalty = abs(info->raster_height - preferred_height);
            total_penalty = charset_penalty + size_penalty;
        }
        else
        {
            total_penalty   = charset_penalty;
        }

        if (total_penalty < best_penalty)
        {
            best_penalty = total_penalty;
            best = info;
        }
    }

    if (best) return best;

    // If style_flags do not match, search again by name only
    for (std::vector<FontInfo*>::iterator it = registered_fonts.begin();
         it != registered_fonts.end(); ++it)
    {
        FontInfo* info = *it;
        if (lstrcmpiA(info->family_name.c_str(), font_name) == 0)
            return info;
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

    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
    {
        // --- Monochrome (1bpp) processing ---
        // (omitted: existing MaskBlt-based implementation is fine)
        // However, when OPAQUE, background fill processing is required.
        if (bkMode == OPAQUE) {
            HBRUSH hbrBg = CreateSolidBrush(bg_color);
            RECT rc = {0, 0, w, h};
            FillRect(hdc, &rc, hbrBg);
            DeleteObject(hbrBg);
        }

        if (w > hbmMask_cache_w || h > hbmMask_cache_h)
        {
            if (hbmMask_cache) { DeleteObject(hbmMask_cache); hbmMask_cache = NULL; }
            hbmMask_cache = CreateBitmap(w, h, 1, 1, NULL);
            hbmMask_cache_w = w;
            hbmMask_cache_h = h;
        }

        int src_pitch = (bitmap->pitch < 0) ? -bitmap->pitch : bitmap->pitch;
        int row_bytes = (w + 7) / 8; // DIB 1bpp row bytes are DWORD-aligned
        int dib_stride = (row_bytes + 3) & ~3; // DWORD align

        std::vector<BYTE> packed(dib_stride * h, 0);
        for (int r = 0; r < h; ++r)
            memcpy(&packed[r * dib_stride],
                   bitmap->buffer + r * src_pitch,
                   row_bytes);

        // Pass as top-down DIB by setting biHeight negative
        BITMAPINFO bmiMono = {};
        bmiMono.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmiMono.bmiHeader.biWidth       = w;
        bmiMono.bmiHeader.biHeight      = -h; // top-down
        bmiMono.bmiHeader.biPlanes      = 1;
        bmiMono.bmiHeader.biBitCount    = 1;
        bmiMono.bmiHeader.biCompression = BI_RGB;
        // 1bpp DIB color table: index 0 = black (background), index 1 = white (foreground)
        bmiMono.bmiColors[0] = { 0,   0,   0, 0 };
        // bmiColors[1] is not included in the default BITMAPINFO structure, so allocate separately
        struct { BITMAPINFOHEADER bih; RGBQUAD colors[2]; } bmiMono2 = {};
        bmiMono2.bih = bmiMono.bmiHeader;
        bmiMono2.colors[0] = { 0,   0,   0, 0 }; // black
        bmiMono2.colors[1] = { 255, 255, 255, 0 }; // white

        SetDIBits(NULL, hbmMask_cache, 0, h, packed.data(),
                  reinterpret_cast<BITMAPINFO*>(&bmiMono2), DIB_RGB_COLORS);

        // MaskBlt: mask=1 -> transfer foreground color from hdcSrc; mask=0 -> keep destination
        MaskBlt(hdc, left, top, w, h,
                hdcSrc, 0, 0,
                hbmMask_cache, 0, 0,
                MAKEROP4(SRCCOPY, 0x00AA0029 /* DST */));

        // Do not DeleteObject hbmMask_cache as it is reused
    }
    else
    {
        // --- Grayscale (8bpp) processing ---
        
        // 1. Get current pixels at the destination (for TRANSPARENT mode)
        // Copy the background from hdc to hdcSrc via BitBlt
        BitBlt(hdcSrc, 0, 0, w, h, hdc, left, top, SRCCOPY);

        std::vector<DWORD> pixels(w * h);
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        // Retrieve current background pixels into array
        GetDIBits(hdcSrc, hbmSrc, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);

        int src_pitch = (bitmap->pitch < 0) ? -bitmap->pitch : bitmap->pitch;
        
        for (int row = 0; row < h; ++row)
        {
            for (int col = 0; col < w; ++col)
            {
                unsigned char alpha = bitmap->buffer[row * src_pitch + col];
                if (alpha == 0) continue;

                // 2. Determine the background color to blend against
                COLORREF target_bg;
                if (bkMode == TRANSPARENT) {
                    // Transparent background: use current pixel at the destination (BGR)
                    DWORD pixel = pixels[row * w + col];
                    target_bg = RGB(GetRValue(pixel), GetGValue(pixel), GetBValue(pixel));
                } else {
                    // Opaque background: use GetBkColor
                    target_bg = bg_color;
                }

                // 3. Alpha blend calculation
                int r = (GetRValue(fg_color) * alpha + GetRValue(target_bg) * (255 - alpha)) / 255;
                int g = (GetGValue(fg_color) * alpha + GetGValue(target_bg) * (255 - alpha)) / 255;
                int b = (GetBValue(fg_color) * alpha + GetBValue(target_bg) * (255 - alpha)) / 255;
                
                pixels[row * w + col] = RGB(r, g, b);
            }
        }

        // 4. Write back the composited pixels
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

    LOGFONTA lf;
    GetObjectA(hFont, sizeof(lf), &lf);
    FontInfo* font_info = find_font_by_logfont(&lf);
    if (!font_info) {
        printf("'%s': not found\n", lf.lfFaceName);
        return FALSE;
    }
    LONG lfHeight = lf.lfHeight;

    printf("Using font: %s, %ld\n", font_info->path.c_str(), lfHeight);

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

    bool is_raster = is_raster_font(font_info->path);
    FT_Face face = NULL;
    bool face_needs_done = false;

    FT_WinFNT_HeaderRec WinFNT;
    bool has_fnt_header = false;
    int pixel_ascent;

    if (is_raster)
    {
        // Raster fonts are opened directly without going through the cache
        if (FT_New_Face(library, font_info->path.c_str(),
                        font_info->face_index, &face) != 0)
            return FALSE;
        face_needs_done = true;

        // Select the fixed size closest to the requested size
        int target_h;
        if (lfHeight < 0)
            target_h = font_info->raster_height;
        else if (lfHeight > 0)
            target_h = lfHeight;
        else
            target_h = 12;

        int best_idx = 0, best_diff = INT_MAX;
        for (int i = 0; i < face->num_fixed_sizes; ++i)
        {
            int diff = abs(face->available_sizes[i].height - target_h);
            if (diff < best_diff) { best_diff = diff; best_idx = i; }
        }
        FT_Select_Size(face, best_idx);

        // Added immediately after FT_Select_Size
        bool has_fnt_header = (FT_Get_WinFNT_Header(face, &WinFNT) == 0);
        printf("first_char=0x%02X, last_char=0x%02X, default_char=0x%02X\n",
            WinFNT.first_char, WinFNT.last_char, WinFNT.default_char);

        pixel_ascent = face->size->metrics.ascender >> 6;
    }
    else
    {
        // Outline fonts go through the cache as before
        FTC_ScalerRec scaler;
        scaler.face_id = static_cast<FTC_FaceID>(font_info);
        scaler.width   = 0;

        if (lfHeight > 0) {
            scaler.height = calc_pixel_size_from_cell_height(font_info, lfHeight);
        } else {
            scaler.height = labs(lfHeight);
        }

        scaler.pixel = 1;
        scaler.x_res = 96;
        scaler.y_res = 96;

        FT_Size ft_size;
        if (FTC_Manager_LookupSize(cache_manager, &scaler, &ft_size) != 0)
            return FALSE;
        face = ft_size->face;

        pixel_ascent = face->size->metrics.ascender >> 6;
    }

    int baseline_y   = Y + pixel_ascent;

    FT_Int32 load_flags;
    if (is_raster)
    {
        // Raster fonts always retrieve a monochrome bitmap.
        // FT_LOAD_NO_SCALE instructs FreeType to use the fixed size as-is.
        load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_NO_HINTING;
    }
    else
    {
        load_flags = FT_LOAD_RENDER;
    }

    FT_Pos current_pen_x = (FT_Pos)X << 6;
    FT_Pos current_pen_y = (FT_Pos)baseline_y << 6;
    FT_UInt previous_glyph = 0; // Holds the previous glyph index
    bool has_kerning = FT_HAS_KERNING(face); // Whether the font has kerning information

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

            printf("glyph_index=%u, byte_val=0x%02X, first_char=0x%02X, calc=%u\n",
                glyph_index, byte_val, WinFNT.first_char,
                byte_val - WinFNT.first_char);
        }
        else
        {
            glyph_index = FT_Get_Char_Index(face, codepoint);
        }

        if (has_kerning && previous_glyph != 0 && glyph_index != 0) {
            FT_Vector delta;
            FT_Get_Kerning(face, previous_glyph, glyph_index, FT_KERNING_DEFAULT, &delta);
            current_pen_x += delta.x;
        }

        if (FT_Load_Glyph(face, glyph_index, load_flags) != 0)
            continue;

        // Verify cmap
        printf("num_charmaps=%d\n", face->num_charmaps);
        for (int ci = 0; ci < face->num_charmaps; ++ci)
        {
            printf("  charmap[%d]: platform=%d, encoding=%d, encoding_id=%d\n",
                ci,
                face->charmaps[ci]->platform_id,
                face->charmaps[ci]->encoding,
                face->charmaps[ci]->encoding_id);
        }
        printf("active charmap: platform=%d, encoding=%d\n",
            face->charmap ? face->charmap->platform_id : -1,
            face->charmap ? face->charmap->encoding    : -1);

        FT_GlyphSlot slot = face->glyph;

        int draw_x = (current_pen_x >> 6) + slot->bitmap_left;
        int draw_y = (current_pen_y >> 6) - slot->bitmap_top;

        draw_glyph(hdc, &slot->bitmap, draw_x, draw_y,
                   fg_color, bg_color);

        printf("glyph U+%04lX: bitmap=%dx%d, advance.x=%ld (>>6=%ld), bitmap_left=%d, bitmap_top=%d\n",
            codepoint,
            slot->bitmap.width, slot->bitmap.rows,
            slot->advance.x, slot->advance.x >> 6,
            slot->bitmap_left, slot->bitmap_top);

        current_pen_x += slot->advance.x;
        previous_glyph = glyph_index;
    }

    if (face_needs_done)
        FT_Done_Face(face);

    return TRUE;
}

HBITMAP TestFreeType(const char* font_name, int font_size, XFORM& xform, HFONT hFont)
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

HBITMAP TestGdi(const char* font_name, int font_size, XFORM& xform, HFONT hFont)
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

bool TestEntry(const char* font_name, int font_size, XFORM& xform)
{
    LOGFONTA lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = font_size;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyA(lf.lfFaceName, font_name);

    HFONT hFont = CreateFontIndirectA(&lf);

    HBITMAP hbm1 = TestFreeType(font_name, font_size, xform, hFont);
    HBITMAP hbm2 = TestGdi(font_name, font_size, xform, hFont);
    if (!hbm1)
        printf("!hbm1\n");
    if (!hbm2)
        printf("!hbm2\n");

    DeleteObject(hFont);

    BOOL ret = nearly_equal_bitmap(hbm1, hbm2, color1, color2);
    if (ret)
    {
        printf("%s, %d: Success!\n", font_name, font_size);
    }
    else
    {
        printf("%s, %d: FAILED\n", font_name, font_size);
    }
    SaveBitmapToFile("a.bmp", hbm1);
    SaveBitmapToFile("b.bmp", hbm2);
    DeleteObject(hbm2);
    DeleteObject(hbm1);
    return ret;
}

int main(int argc, char** argv)
{
    const char* font_name = FONT_NAME;
    int font_size = FONT_SIZE;

    XFORM xform;
    xform.eM11 = 1;
    xform.eM12 = 0;
    xform.eM21 = 0;
    xform.eM22 = 1;
    xform.eDx = 0;
    xform.eDy = 0;

    if (argc >= 2) font_name = argv[1];
    if (argc >= 3) font_size = atoi(argv[2]);
    if (argc >= 4) xform.eM11 = atoi(argv[3]);
    if (argc >= 5) xform.eM12 = atoi(argv[4]);
    if (argc >= 6) xform.eM21 = atoi(argv[5]);
    if (argc >= 7) xform.eM22 = atoi(argv[6]);

    if (!InitFontSupport()) {
        FreeFontSupport();
        return -1;
    }

    bool ret = TestEntry(font_name, font_size, xform);

    FreeFontSupport();
    return ret ? 0 : 1;
}