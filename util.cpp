#include <windows.h>
#include <stdio.h>
#include "util.h"

//  For TranslateCharsetInfo
const CHARSETINFO g_FontTci[MAXTCIINDEX] =
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

void get_box(BITMAP& bm, COLORREF* pixels, INT& min_x, INT& min_y, INT& max_x, INT& max_y, COLORREF target_color)
{
    min_x = min_y = 10000;
    max_x = max_y = -10000;
    INT i = 0;
    for (INT y = 0; y < bm.bmHeight; ++y) {
        for (INT x = 0; x < bm.bmWidth; ++x) {
            COLORREF color = pixels[i] & 0xFFFFFF;
            if (color == target_color) {
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;
            }
            ++i;
        }
    }
}

BOOL nearly_equal_bitmap(HBITMAP hbm1, HBITMAP hbm2, COLORREF color1, COLORREF color2)
{
    BITMAP bm1, bm2;
    if (!GetObject(hbm1, sizeof(bm1), &bm1) ||
        !GetObject(hbm2, sizeof(bm2), &bm2))
    {
        return FALSE;
    }

    // Basic dimensions check
    if (bm1.bmWidth != bm2.bmWidth || bm1.bmHeight != bm2.bmHeight)
        return FALSE;

    HDC hdc = GetDC(NULL);
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm1.bmWidth;
    bmi.bmiHeader.biHeight = -bm1.bmHeight; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32; // Force 32-bit for easy comparison
    bmi.bmiHeader.biCompression = BI_RGB;

    int pixelCount = bm1.bmWidth * bm1.bmHeight;
    COLORREF* pixels1 = new COLORREF[pixelCount];
    COLORREF* pixels2 = new COLORREF[pixelCount];

    // Get raw bits for both images
    GetDIBits(hdc, hbm1, 0, bm1.bmHeight, pixels1, &bmi, DIB_RGB_COLORS);
    GetDIBits(hdc, hbm2, 0, bm2.bmHeight, pixels2, &bmi, DIB_RGB_COLORS);

    INT min_x1, min_y1, max_x1, max_y1;
    INT min_x2, min_y2, max_x2, max_y2;

    get_box(bm1, pixels1, min_x1, min_y1, max_x1, max_y1, color1);
    get_box(bm2, pixels2, min_x2, min_y2, max_x2, max_y2, color1);

    INT sum = 0;
    sum += abs(min_x1 - min_x2);
    sum += abs(min_y1 - min_y2);
    sum += abs(max_x1 - max_x2);
    sum += abs(max_y1 - max_y2);

    get_box(bm1, pixels1, min_x1, min_y1, max_x1, max_y1, color2);
    get_box(bm2, pixels2, min_x2, min_y2, max_x2, max_y2, color2);
    sum += abs(min_x1 - min_x2);
    sum += abs(min_y1 - min_y2);
    sum += abs(max_x1 - max_x2);
    sum += abs(max_y1 - max_y2);

    delete[] pixels1;
    delete[] pixels2;
    ReleaseDC(NULL, hdc);

    int threshould = ((bm1.bmWidth + bm1.bmHeight) / 2) * 10 / 100; // 10%
    printf("%d %d\n", sum, threshould);
    return sum <= threshould;
}

UINT get_codepage_from_charset(BYTE charset)
{
    UINT codepage = 1252;
    for (int tci_i = 0; tci_i < MAXTCIINDEX; ++tci_i)
    {
        if (g_FontTci[tci_i].ciCharset == charset &&
            g_FontTci[tci_i].ciACP != 0)
        {
            codepage = g_FontTci[tci_i].ciACP;
            break;
        }
    }
    return codepage;
}
