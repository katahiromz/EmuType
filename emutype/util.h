#pragma once

#define CP_SYMBOL   42
#define MAXTCIINDEX 32

extern const CHARSETINFO g_FontTci[MAXTCIINDEX];

BOOL nearly_equal_bitmap(HBITMAP hbm1, HBITMAP hbm2, COLORREF color1, COLORREF color2);
UINT get_codepage_from_charset(BYTE charset);
