
#pragma once

#include <windows.h>
#include <string.h>
#include <strsafe.h>
#include <assert.h>

static inline
VOID
APIENTRY
EngDebugPrint(
    PCHAR StandardPrefix,
    PCHAR DebugMessage,
    va_list ap)
{
    char buf[512];
    StringCchCopyA(buf, _countof(buf), StandardPrefix);
    size_t len = strlen(buf);
    StringCchVPrintfA(&buf[len], _countof(buf) - len, DebugMessage, ap);
    OutputDebugStringA(buf);
}

static inline
ULONG
DebugPrint(
    PCHAR StandardPrefix,
    PCHAR Format, ...)
{
    va_list args;

    va_start(args, Format);
    EngDebugPrint(StandardPrefix, Format, args);
    va_end(args);
    return 0;
}

static inline
VOID
APIENTRY
EngBugCheckEx(IN ULONG BugCheckCode,
              IN ULONG_PTR BugCheckParameter1,
              IN ULONG_PTR BugCheckParameter2,
              IN ULONG_PTR BugCheckParameter3,
              IN ULONG_PTR BugCheckParameter4)
{
    DebugPrint("EngBugCheckEx", "%p, %p, %p, %p",
        (void *)BugCheckParameter1,
        (void *)BugCheckParameter2,
        (void *)BugCheckParameter3,
        (void *)BugCheckParameter4);
    assert(0);
}
