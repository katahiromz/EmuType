// Windowsのフォントエンジンをエミュレートしたい
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

// 今回は別のレジストリキーでテストする。
const char* reg_key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontsEmulated";

struct FontInfo {
    std::string path; // ANSI
    FT_Long face_index;
    std::string family_name; // UTF-8
    std::string english_name; // UTF-8
    FT_Long style_flags; // See FT_Face.style_flags
    FT_Long em_ascender;
    FT_Long em_descender;
    FT_Long em_units;
    FT_Byte charset;
    INT raster_height;
    POUTLINETEXTMETRICW potm;
};
std::vector<FontInfo*> registered_fonts;

char fonts_dir[MAX_PATH];
FT_Library library;
FTC_Manager cache_manager;

const int WIDTH  = 300;
const int HEIGHT = 300;
const COLORREF BG = RGB(255, 255, 255); // 白背景
const COLORREF FG = RGB(0,   0,   0);   // 黒文字
const LONG FONT_SIZE = 24;

// フォントの種類を判定するヘルパー
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

            if (sname.language_id == TT_MS_LANGID_JAPANESE_JAPAN && localized)
                priority = 3;
            else if (sname.language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES)
                priority = 2;
            else
                priority = 1;
        }
        else if (sname.platform_id == TT_PLATFORM_MACINTOSH &&
                 sname.encoding_id == TT_MAC_ID_ROMAN)
        {
            // Mac Roman: ASCII範囲であればそのまま使える
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

    // フォールバック
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

        // TT_NAME_ID_FONT_SUBFAMILY は "Regular" や "Bold" などのスタイル名を指す
        if (sname.name_id != TT_NAME_ID_FONT_SUBFAMILY)
            continue;

        int priority = -1;
        std::string name;

        if (sname.platform_id == TT_PLATFORM_MICROSOFT &&
            sname.encoding_id == TT_MS_ID_UNICODE_CS)
        {
            // UTF-16BE から UTF-8 への変換
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

            if (sname.language_id == TT_MS_LANGID_JAPANESE_JAPAN && localized)
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

    // フォールバック: FreeTypeがデフォルトで保持しているスタイル名を使用
    return face->style_name ? face->style_name : "";
}

bool fill_raster_text_metrics(FT_Face face, FT_WinFNT_HeaderRec *pWinFNT, FontInfo* info) {
    info->potm = (OUTLINETEXTMETRICW*)calloc(1, sizeof(OUTLINETEXTMETRICW));
    if (!info->potm)
        return FALSE;
    info->potm->otmSize = sizeof(TEXTMETRICW);
    TEXTMETRICW* tm = &info->potm->otmTextMetrics;
    tm->tmHeight           = pWinFNT->pixel_height;
    tm->tmAscent           = pWinFNT->ascent;
    tm->tmDescent          = tm->tmHeight - tm->tmAscent;
    tm->tmInternalLeading  = pWinFNT->internal_leading;
    tm->tmExternalLeading  = pWinFNT->external_leading;
    tm->tmAveCharWidth     = pWinFNT->avg_width;
    tm->tmMaxCharWidth     = pWinFNT->max_width;
    tm->tmWeight           = pWinFNT->weight;
    tm->tmItalic           = pWinFNT->italic;
    tm->tmUnderlined       = pWinFNT->underline;
    tm->tmStruckOut        = pWinFNT->strike_out;
    tm->tmFirstChar        = pWinFNT->first_char;
    tm->tmLastChar         = pWinFNT->last_char;
    tm->tmDefaultChar      = pWinFNT->default_char;
    tm->tmBreakChar        = pWinFNT->break_char;
    tm->tmCharSet          = pWinFNT->charset;
    tm->tmPitchAndFamily   = pWinFNT->pitch_and_family;
    return TRUE;
}

// UTF-8文字列をstd::wstring(UTF-16)に変換する
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
    pStrBase += bsize; // 書き込み位置を次に進める
}

bool fill_outline_text_metrics(FT_Face face, FontInfo* info) {
    if (!FT_IS_SFNT(face))
        return false;

    // 1. 各種文字列の取得と UTF-16 変換
    std::string family_utf8 = get_family_name(face, true);
    std::string style_utf8  = get_style_name(face, true);
    std::string face_utf8   = family_utf8 + (style_utf8.empty() ? "" : " " + style_utf8);
    std::string full_utf8   = face_utf8; // 本来はもっと複雑な構成が可能

    std::wstring wFamily = utf8_to_wide(family_utf8);
    std::wstring wFace   = utf8_to_wide(face_utf8);
    std::wstring wStyle  = utf8_to_wide(style_utf8);
    std::wstring wFull   = utf8_to_wide(full_utf8);

    size_t strings_size = (wFamily.length() + 1 + wFace.length() + 1 +
                           wStyle.length() + 1 + wFull.length() + 1) * sizeof(WCHAR);
    size_t total_size = sizeof(OUTLINETEXTMETRICW) + strings_size;

    // 3. メモリ確保
    info->potm = (OUTLINETEXTMETRICW*)calloc(1, total_size);
    if (!info->potm) return false;

    POUTLINETEXTMETRICW potm = info->potm;
    potm->otmSize = (UINT)total_size;

    // --- メトリクス情報のフィル (前回同様) ---
    potm->otmTextMetrics.tmAscent  = (LONG)face->ascender;
    potm->otmTextMetrics.tmDescent = (LONG)-face->descender;
    potm->otmTextMetrics.tmHeight  = potm->otmTextMetrics.tmAscent + potm->otmTextMetrics.tmDescent;
    potm->otmTextMetrics.tmInternalLeading = (LONG)(potm->otmTextMetrics.tmHeight - face->units_per_EM);
    potm->otmEMSquare = (UINT)face->units_per_EM;
    
    TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2) {
        memcpy(&potm->otmPanoseNumber, &os2->panose, 10);
        potm->otmfsSelection = os2->fsSelection;
        potm->otmfsType = os2->fsType;
    }

    BYTE* pStrBase = (BYTE*)potm + sizeof(OUTLINETEXTMETRICW);
    BYTE* pBaseAddr = (BYTE*)potm;

    copy_str_to_otm(wFamily, potm->otmpFamilyName, pStrBase, pBaseAddr);
    copy_str_to_otm(wFace,   potm->otmpFaceName,   pStrBase, pBaseAddr);
    copy_str_to_otm(wStyle,  potm->otmpStyleName,  pStrBase, pBaseAddr);
    copy_str_to_otm(wFull,   potm->otmpFullName,   pStrBase, pBaseAddr);

    return true;
}

bool load_font(const char *path, int index)
{
    int iStart = (index == -1) ? 0 : index;
    FT_Face face;
    FT_Error error = FT_New_Face(library, path, iStart, &face);
    if (error)
        return false;

    FT_Long num_faces = face->num_faces;

    FontInfo* info = new FontInfo();
    info->path = path;
    info->face_index = iStart;
    info->family_name = get_family_name(face, true);
    info->english_name = get_family_name(face, false);
    info->style_flags = face->style_flags;
    info->em_ascender  = face->ascender;
    info->em_descender = face->descender;
    info->em_units     = face->units_per_EM;
    info->charset = 0xFF; // 不明
    info->raster_height = 0;
    info->potm = NULL;

    if (FT_IS_SFNT(face))
    {
        fill_outline_text_metrics(face, info);
    }
    else
    {
        FT_WinFNT_HeaderRec fnt_header;
        if (FT_Get_WinFNT_Header(face, &fnt_header) == 0)
        {
            info->charset = fnt_header.charset;
            info->raster_height = fnt_header.pixel_height;
            fill_raster_text_metrics(face, &fnt_header, info);
        }
        else
        {
            return false;
        }
    }

    registered_fonts.push_back(info);

    FT_Done_Face(face);

    if (index == -1)
    {
        for (FT_Long iFace = 1; iFace < num_faces; ++iFace)
        {
            if (!load_font(path, static_cast<int>(iFace)))
                return false;
        }
    }

    return true;
}

void free_fonts(void)
{
    for (auto* info : registered_fonts)
    {
        if (info->potm)
            free(info->potm);
        delete info;
    }
    registered_fonts.clear();
}

// ラスタフォントの利用可能サイズ一覧を "8,10,12" 形式で返す
static std::string get_raster_sizes(FT_Face face)
{
    std::string result;
    for (int i = 0; i < face->num_fixed_sizes; ++i)
    {
        if (!result.empty())
            result += ",";
        char buf[16];
        // fixed_sizes[i].height はピクセル単位
        _snprintf(buf, sizeof(buf), "%d", static_cast<int>(face->available_sizes[i].height));
        result += buf;
    }
    return result;
}

// 同一ファイルのFontInfoをまとめて値名を生成する
// TrueType / OpenType: "MS Gothic & MS UI Gothic & MS PGothic (TrueType)"
// ラスタ:   "MS Sans Serif 8,10,12,14,18,24"
static std::string make_registry_value_name(
    const std::vector<FontInfo*>& group, FT_Face first_face)
{
    bool raster = is_raster_font(group[0]->path);
    if (raster)
    {
        // ラスタフォントはサイズ列を付ける（Windows互換）
        std::string sizes = get_raster_sizes(first_face);
        std::string name = group[0]->family_name;
        if (!sizes.empty())
            name += " " + sizes;
        return name;
    }

    // アウトラインフォント: グループ内の全family_nameを & で連結
    // （重複は除く）
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
    // 既存の値をすべて削除
    WCHAR szValue[MAX_PATH];
    for (;;) {
        DWORD cchValue = _countof(szValue);
        if (RegEnumValueW(hKey, 0, szValue, &cchValue, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        RegDeleteValueW(hKey, szValue);
    }

    // パスでグループ化
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

        // 値名生成のためにフェイスを一時オープン
        FT_Face first_face;
        if (FT_New_Face(library, path.c_str(), group[0]->face_index, &first_face) != 0)
            continue;

        std::string value_name = make_registry_value_name(group, first_face);
        FT_Done_Face(first_face);

        // 値データ: ファイル名のみ（Windowsレジストリに合わせる）
        char filename[MAX_PATH];
        if (path.find(fonts_dir) == 0)
            lstrcpynA(filename, PathFindFileNameA(path.c_str()), MAX_PATH);
        else
            lstrcpynA(filename, path.c_str(), MAX_PATH);

        // value_name (UTF-8) → UTF-16 変換
        int wname_len = MultiByteToWideChar(CP_UTF8, 0,
                                            value_name.c_str(), -1,
                                            NULL, 0);
        std::vector<WCHAR> wname(wname_len);
        MultiByteToWideChar(CP_UTF8, 0,
                            value_name.c_str(), -1,
                            &wname[0], wname_len);

        // filename (システムデフォルトACP) → UTF-16 変換
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

        // フルパスに変換
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

        // ファイルが存在しなければスキップ
        if (!PathFileExistsA(full_path))
            continue;

        // FreeTypeで全フェイスを読み込む（load_fontの-1と同じロジック）
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
            write_fonts_to_registry(hKey);
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
}

FontInfo* find_font_by_name(const char* font_name, FT_Long style_flags = 0, int preferred_height = 0, FT_Byte preferred_charset = ANSI_CHARSET)
{
    FontInfo* best = NULL;
    int best_penalty = INT_MAX;

    for (auto* info : registered_fonts)
    {
        if (lstrcmpiA(info->family_name.c_str(), font_name) != 0)
            continue;

        if (!is_raster_font(info->path))
        {
            // アウトラインフォントはstyle_flagsだけ見ればよい
            if (info->style_flags == style_flags)
                return info;
            continue;
        }

        if (!is_raster_font(info->path))
        {
            // アウトラインフォントはそのまま返す
            return info;
        }

        int charset_penalty = (info->charset != preferred_charset) ? 10000 : 0;
        int size_penalty    = abs(info->raster_height - preferred_height);
        int total_penalty   = charset_penalty + size_penalty;

        if (total_penalty < best_penalty)
        {
            best_penalty = total_penalty;
            best = info;
        }
    }

    if (best) return best;

    // style_flagsが一致しなければ名前だけで再検索
    for (std::vector<FontInfo*>::iterator it = registered_fonts.begin();
         it != registered_fonts.end(); ++it)
    {
        FontInfo* info = *it;
        if (lstrcmpiA(info->family_name.c_str(), font_name) == 0)
            return info;
    }
    return NULL;
}

// FreeTypeのグレースケールビットマップ（8bpp）をHDCに描画する。
// (pen_x, pen_y) はベースライン上のペン位置（ピクセル単位）。
// fg_color: 文字色（COLORREF）、bg_color: 背景色（COLORREF）。
void draw_glyph(HDC hDC, FT_Bitmap* bitmap, int left, int top,
                COLORREF fg_color, COLORREF bg_color)
{
    for (unsigned int row = 0; row < bitmap->rows; ++row)
    {
        for (unsigned int col = 0; col < bitmap->width; ++col)
        {
            unsigned char alpha;
            if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
            {
                // packed 1bpp: 1バイトに8ピクセル、MSBが左
                unsigned char byte = bitmap->buffer[row * bitmap->pitch + col / 8];
                alpha = (byte & (0x80 >> (col % 8))) ? 255 : 0;
            }
            else
            {
                // 8bpp グレースケール
                alpha = bitmap->buffer[row * bitmap->pitch + col];
            }

            if (alpha == 0)
                continue;

            int r = (GetRValue(fg_color) * alpha + GetRValue(bg_color) * (255 - alpha)) / 255;
            int g = (GetGValue(fg_color) * alpha + GetGValue(bg_color) * (255 - alpha)) / 255;
            int b = (GetBValue(fg_color) * alpha + GetBValue(bg_color) * (255 - alpha)) / 255;
            SetPixel(hDC, left + col, top + row, RGB(r, g, b));
        }
    }
}

int calc_pixel_size_from_cell_height(FontInfo* font_info, int cell_height_px)
{
    FT_Long em_ascender  = font_info->em_ascender;
    FT_Long em_descender = font_info->em_descender;
    FT_Long em_units     = font_info->em_units;
    FT_Long em_cell = em_ascender - em_descender;
    int pixel_size = static_cast<int>(
        static_cast<double>(cell_height_px) * em_units / em_cell + 0.5);
    return pixel_size;
}

void draw_text_wide(HDC hDC, int x, int y, const WCHAR* wide_text, INT cch,
                    FontInfo* font_info, LONG lfHeight)
{
    COLORREF fg_color = GetTextColor(hDC);
    COLORREF bg_color = GetBkColor(hDC);

    bool is_raster = is_raster_font(font_info->path);
    FT_Face face = NULL;
    bool face_needs_done = false;

    FT_WinFNT_HeaderRec fnt_header;
    bool has_fnt_header = false;

    if (is_raster)
    {
        // ラスタフォントはキャッシュを経由せず直接オープン
        if (FT_New_Face(library, font_info->path.c_str(),
                        font_info->face_index, &face) != 0)
            return;
        face_needs_done = true;

        // 要求サイズに最も近い固定サイズを選択
        int target_h = (lfHeight < 0) ? -lfHeight : (lfHeight > 0 ? lfHeight : 12);
        int best_idx = 0, best_diff = INT_MAX;
        for (int i = 0; i < face->num_fixed_sizes; ++i)
        {
            int diff = abs(face->available_sizes[i].height - target_h);
            if (diff < best_diff) { best_diff = diff; best_idx = i; }
        }
        FT_Select_Size(face, best_idx);

        printf("Selected size: %d px (requested %d)\n",
            face->available_sizes[best_idx].height, target_h);
        printf("metrics: ascender=%ld, descender=%ld, height=%ld\n",
            face->size->metrics.ascender >> 6,
            face->size->metrics.descender >> 6,
            face->size->metrics.height >> 6);

        // FT_Select_Size の直後に追加
        fnt_header;
        bool has_fnt_header = (FT_Get_WinFNT_Header(face, &fnt_header) == 0);
        printf("first_char=0x%02X, last_char=0x%02X, default_char=0x%02X\n",
            fnt_header.first_char, fnt_header.last_char, fnt_header.default_char);
    }
    else
    {
        // アウトラインフォントは従来通りキャッシュ経由
        FTC_ScalerRec scaler;
        scaler.face_id = static_cast<FTC_FaceID>(font_info);
        scaler.width   = 0;
        if (lfHeight < 0) {
            scaler.height = static_cast<FT_UInt>(-lfHeight);
        } else if (lfHeight > 0) {
            FT_Long em_cell = font_info->em_ascender - font_info->em_descender;
            scaler.height = static_cast<FT_UInt>(
                static_cast<double>(lfHeight) * font_info->em_units / em_cell + 0.5);
        } else {
            scaler.height = 12;
        }
        scaler.pixel = 1;
        scaler.x_res = 0;
        scaler.y_res = 0;

        FT_Size ft_size;
        if (FTC_Manager_LookupSize(cache_manager, &scaler, &ft_size) != 0)
            return;
        face = ft_size->face;
        // face_needs_done = false のまま（キャッシュ管理）
    }

    int pixel_ascent = face->size->metrics.ascender >> 6;
    int baseline_y   = y + pixel_ascent;
    int pen_x        = x;

    FT_Int32 load_flags;
    if (is_raster)
    {
        // ラスタフォントは必ずモノクロビットマップで取得する
        // FT_LOAD_NO_SCALEは固定サイズそのものを使う指示
        load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_NO_HINTING;
    }
    else
    {
        load_flags = FT_LOAD_RENDER;
    }

    const WCHAR* pch = wide_text;
    for (INT i = 0; i < cch; ++i)
    {
        unsigned long codepoint = 0;
        WCHAR w = pch[i];
        if (IS_HIGH_SURROGATE(w) && i + 1 < cch) {
            WCHAR w2 = pch[i + 1];
            if (IS_LOW_SURROGATE(w2)) {
                codepoint = 0x10000UL
                    + (static_cast<unsigned long>(w  - 0xD800) << 10)
                    +  static_cast<unsigned long>(w2 - 0xDC00);
                ++i;
            }
        } else {
            codepoint = static_cast<unsigned long>(w);
        }

        FT_UInt glyph_index;
        if (is_raster)
        {
            // UnicodeコードポイントをFONのコードページに変換
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

            if (byte_val < fnt_header.first_char || byte_val > fnt_header.last_char)
            {
                // 範囲外は default_char を使う
                glyph_index = fnt_header.default_char - fnt_header.first_char;
            }
            else
            {
                glyph_index = byte_val - fnt_header.first_char + 1;
            }

            printf("glyph_index=%u, byte_val=0x%02X, first_char=0x%02X, calc=%u\n",
                glyph_index, byte_val, fnt_header.first_char,
                byte_val - fnt_header.first_char);
        }
        else
        {
            glyph_index = FT_Get_Char_Index(face, codepoint);
        }

        if (FT_Load_Glyph(face, glyph_index, load_flags) != 0)
            continue;

        // cmapの確認
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
        draw_glyph(hDC, &slot->bitmap,
                   pen_x + slot->bitmap_left,
                   baseline_y - slot->bitmap_top,
                   fg_color, bg_color);

        printf("glyph U+%04lX: bitmap=%dx%d, advance.x=%ld (>>6=%ld), bitmap_left=%d, bitmap_top=%d\n",
            codepoint,
            slot->bitmap.width, slot->bitmap.rows,
            slot->advance.x, slot->advance.x >> 6,
            slot->bitmap_left, slot->bitmap_top);

        if (is_raster)
            pen_x += (slot->advance.x + 32) >> 6;  // これも試す
        else
            pen_x += slot->advance.x >> 6;
    }

    if (face_needs_done)
        FT_Done_Face(face);
}

void AppMain(void)
{
    const char* font_name = "MS Serif";
    FontInfo* font_info = find_font_by_name(font_name, 0, abs(FONT_SIZE));
    if (!font_info) {
        printf("'%s': not found\n", font_name);
        return;
    }
    printf("Using font: %s\n", font_info->path.c_str());

    HDC hScreenDC = GetDC(NULL);
    HBITMAP hbm = CreateCompatibleBitmap(hScreenDC, WIDTH, HEIGHT);
    ReleaseDC(NULL, hScreenDC);

    HDC hDC = CreateCompatibleDC(NULL);
    HGDIOBJ hbmOld = SelectObject(hDC, hbm);

    // 背景を白で塗りつぶす
    RECT rc = { 0, 0, WIDTH, HEIGHT };
    HBRUSH hBrush = CreateSolidBrush(BG);
    FillRect(hDC, &rc, hBrush);
    DeleteObject(hBrush);

    const WCHAR* lines[] = {
        L"FreeType Draw",
    };
    int num_lines = static_cast<int>(sizeof(lines) / sizeof(lines[0]));
    int line_height = abs(FONT_SIZE) + 8;
    int start_y = abs(FONT_SIZE) + 10;

    for (int i = 0; i < num_lines; ++i)
    {
        SetBkMode(hDC, OPAQUE);
        SetBkColor(hDC, BG);
        SetTextColor(hDC, FG);
        draw_text_wide(hDC, 10, start_y + i * line_height,
            lines[i], lstrlenW(lines[i]), font_info, FONT_SIZE);

        LOGFONTA lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = FONT_SIZE;
        lstrcpyA(lf.lfFaceName, font_name);
        lf.lfCharSet = DEFAULT_CHARSET;
        HFONT hFont = CreateFontIndirectA(&lf);
        HGDIOBJ hFontOld = SelectObject(hDC, hFont);
        TextOutW(hDC, 10, start_y + i * line_height + 100,
                 lines[i], lstrlenW(lines[i]));
        SelectObject(hDC, hFontOld);
        DeleteObject(hFont);
    }

    SelectObject(hDC, hbmOld);
    DeleteDC(hDC);

    SaveBitmapToFileA("a.bmp", hbm);
    DeleteObject(hbm);
}

int main(void)
{
    if (!InitFontSupport()) {
        FreeFontSupport();
        return -1;
    }

    AppMain();

    FreeFontSupport();
    return 0;
}
