#include <windows.h>
#include <stdio.h>

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

    int diffCount = 0;
    for (int i = 0; i < pixelCount; ++i) {
        if (pixels1[i] != pixels2[i]) {
            BYTE b10 = GetRValue(pixels1[i]);
            BYTE b11 = GetGValue(pixels1[i]);
            BYTE b12 = GetBValue(pixels1[i]);
            BYTE b20 = GetRValue(pixels2[i]);
            BYTE b21 = GetGValue(pixels2[i]);
            BYTE b22 = GetBValue(pixels2[i]);
            int diff_sum = (abs(b10 - b20) + abs(b11 - b21) + abs(b12 - b22)) / 3;
            if (diff_sum >= 255 / 100)
            {
                diffCount++;
            }
        }
    }

    delete[] pixels1;
    delete[] pixels2;
    ReleaseDC(NULL, hdc);

    printf("%d %d\n", diffCount, (bm1.bmWidth * bm1.bmHeight) / 100);
    return diffCount <= (bm1.bmWidth * bm1.bmHeight) / 100;
}
