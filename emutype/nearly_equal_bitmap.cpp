#include <windows.h>
#include <stdio.h>

void get_box(BITMAP& bm, COLORREF* pixels, INT& min_x, INT& min_y, INT& max_x, INT& max_y)
{
    min_x = min_y = 10000;
    max_x = max_y = -10000;
    INT i = 0;
    for (INT y = 0; y < bm.bmHeight; ++y) {
        for (INT x = 0; x < bm.bmWidth; ++x) {
            COLORREF color = pixels[i] & 0xFFFFFF;
            if (color != 0xFFFFFF) {
                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;
            }
            ++i;
        }
    }
}

BOOL nearly_equal_bitmap(HBITMAP hbm1, HBITMAP hbm2)
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
    get_box(bm1, pixels1, min_x1, min_y1, max_x1, max_y1);

    INT min_x2, min_y2, max_x2, max_y2;
    get_box(bm2, pixels2, min_x2, min_y2, max_x2, max_y2);

    INT sum = 0;
    sum += abs(min_x1 - min_x2);
    sum += abs(min_y1 - min_y2);
    sum += abs(max_x1 - max_x2);
    sum += abs(max_y1 - max_y2);

    delete[] pixels1;
    delete[] pixels2;
    ReleaseDC(NULL, hdc);

    int threshould = ((bm1.bmWidth + bm1.bmHeight) / 2) * 20 / 100; // 20%
    printf("%d %d\n", sum, threshould);
    return sum <= threshould;
}
